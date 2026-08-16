/*
 * tetrisd.c - entry point.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include "tetrisctl/control_plane.h"
#include "tetrisd/adminmsg.h"
#include "tetrisd/room.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* PATH_MAX */
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_BACKLOG 16

char key_path[PATH_MAX], cert_path_buf[PATH_MAX], ca_path[PATH_MAX];
char log_path[PATH_MAX], log_ipc[PATH_MAX];

/* The port from .tetrishrc, written once in main() before any thread starts
 * and never written again. Read by listener_thread() at startup. */
static int g_port;

/*
 * The port actually bound right now, per reload_config()'s admin-thread-only
 * bookkeeping - distinct from g_port, which freezes at the startup value.
 *
 * Single-owner, not locked: seeded from g_port at the top of admin_thread()
 * and from then on only ever read or written by reload_config(), which only
 * ever runs on the admin thread (see the room.c comment this mirrors). It
 * exists so a SIGHUP with an unchanged listen_port is a no-op instead of an
 * EADDRINUSE retry against the listener's own still-open socket.
 */
static int g_active_port;

/* How long a session gets to exit on its own before it is killed outright. */
#define REAP_TIMEOUT_MS 500

/* tetrisd and its session children run from the repository root. */
static const char g_session_bin[] = "bin/session";
/* MAX_SESSIONS / MAX_ROOMS / MAX_ROOM_MEMBERS come from tetrisd.h */

/* One new session, announced by the listener thread to the admin thread. */
typedef struct
{
    pid_t pid; /* the session process           */
    int fd;    /* admin's end of its socketpair  */
} NewSession;

/* Listener -> admin notify pipe. The admin thread always polls g_notify[0];
 * the listener writes a NewSession to g_notify[1] after each fork. This both
 * wakes the admin's poll() and delivers the new fd. */
static int g_notify[2];

/*
 * Control thread -> admin pipe, carrying one already-parsed CtlReq.
 *
 * A second pipe rather than a widened NewSession: the two flows have nothing
 * in common but their destination, and a tagged union would put a branch in
 * the hottest drain loop in the daemon to save one file descriptor.
 */
static int g_ctl_notify[2];

/*
 * Stop signal for both accepting threads (TCP listener and control plane).
 *
 * Written by the SHUTDOWN verb and by the SIGTERM/SIGINT handler; polled, and
 * deliberately NEVER read. Two threads wait on the same read end, so whichever
 * consumed the byte would leave the other blocked forever. Left in the pipe it
 * stays readable for both, permanently - which is what "stop" should mean.
 *
 * A pipe rather than a flag plus a signal because it is level-triggered: a
 * byte written before poll() is even entered still returns immediately. A
 * signal aimed at a thread about to call accept() can be lost in the gap
 * between the flag check and the syscall, and closing the listening fd from
 * another thread races the reuse of that fd number.
 */
static int g_quit[2];

/*
 * main -> admin: teardown may begin.
 *
 * Separate from g_quit because the ordering matters. If admin started closing
 * session fds while the listener was still inside fork(), the listener would
 * write a NewSession nobody ever drains - a leaked fd and an orphaned child.
 * main joins both accepting threads first, and only then writes here, so the
 * session table is provably stable before it is torn down.
 */
static int g_admin_quit[2];
static pthread_mutex_t g_stop_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_stop_requested;

/*
 * SIGHUP / SIGUSR1 self-pipes.
 *
 * Same reasoning as on_terminate/g_quit below: the handler is not allowed to
 * do the real work (rc_reload(), socket(), bind(), the room-table reads a
 * state dump needs), so it only writes one byte and the admin thread - the
 * only thread allowed to touch the room tables - does the rest from its
 * normal poll() loop.
 */
static int g_reload[2]; /* SIGHUP  -> admin thread */
static int g_dump[2];   /* SIGUSR1 -> admin thread */

/*
 * Admin -> listener hand-off for a SIGHUP relisten.
 *
 * The admin thread is the one that reloads .tetrishrc and therefore the one
 * that discovers a new listen_port, but only the listener thread may close
 * or replace its own listening fd without racing that fd number's reuse
 * (same hazard g_quit's comment describes for a cross-thread close()). So
 * admin binds the *new* socket and hands the fd across; the listener closes
 * the old one and swaps in the new one from inside its own poll() loop.
 */
static int g_relisten[2];

/*
 * The admin thread's poll set.
 *
 * File-scope rather than local to admin_thread() because a kick removes a
 * session from outside the scan loop that used to own this array. Still
 * single-owner - only the admin thread touches these, exactly like the room
 * tables in room.c.
 */
#define ADMIN_FIXED_FDS                                                        \
    5 /* g_notify, g_ctl_notify, g_admin_quit, g_reload, g_dump */

static struct pollfd g_fds[ADMIN_FIXED_FDS + MAX_SESSIONS];
static pid_t g_pids[ADMIN_FIXED_FDS + MAX_SESSIONS];
static int g_nfds = ADMIN_FIXED_FDS;

static int rc_config()
{
    if (rc_get_int("listen_port", 0, 1, 65535, &g_port) != RC_VALUE_FOUND ||
        rc_get("cert_path", NULL, cert_path_buf, sizeof cert_path_buf) !=
            RC_VALUE_FOUND ||
        rc_get("key_path", NULL, key_path, sizeof key_path) != RC_VALUE_FOUND ||
        rc_get("ca_path", NULL, ca_path, sizeof ca_path) != RC_VALUE_FOUND ||
        rc_get("log_path", NULL, log_path, sizeof log_path) != RC_VALUE_FOUND ||
        rc_get("log_ipc", NULL, log_ipc, sizeof log_ipc) != RC_VALUE_FOUND)
    {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* shutdown plumbing                                                   */
/* ------------------------------------------------------------------ */

/* Ask both accepting threads to stop. Safe to call more than once. */
static void request_stop(void)
{
    bool first_request = false;

    pthread_mutex_lock(&g_stop_lock);
    if (!g_stop_requested)
    {
        g_stop_requested = true;
        first_request = true;
    }
    pthread_mutex_unlock(&g_stop_lock);

    if (!first_request)
        return;

    /* Keep even non-blocking I/O outside the critical section. The mutex only
     * serialises the flag; no syscall or other lock is acquired while held. */
    (void)log_send(LOG_INFO,
                   "operation=request_stop event=shutdown_requested status=0");
    const char b = 'x';
    if (write(g_quit[1], &b, 1) < 0 && errno != EAGAIN)
        perror("tetrisd: quit pipe");
}

/*
 * Signal entry point for SIGTERM and SIGINT.
 *
 * Does nothing but write the quit pipe. Teardown calls waitpid, unlink and
 * close - none async-signal-safe - so the real work has to happen back on a
 * normal thread. This is the whole reason the quit pipe exists rather than a
 * handler that tries to clean up in place.
 */
static void on_terminate(int sig)
{
    (void)sig;
    const char b = 'x';
    ssize_t n = write(g_quit[1], &b, 1);
    (void)n; /* nothing useful to do about a failure from here */
}

/* SIGHUP: wake the admin thread to reload .tetrishrc. Same shape as
 * on_terminate - write one byte, nothing else. */
static void on_reload(int sig)
{
    (void)sig;
    const char b = 'x';
    ssize_t n = write(g_reload[1], &b, 1);
    (void)n;
    rc_reload();
    (void)rc_config();
}

/* SIGUSR1: wake the admin thread to dump a room/player snapshot to the log. */
static void on_dump(int sig)
{
    (void)sig;
    const char b = 'x';
    ssize_t n = write(g_dump[1], &b, 1);
    (void)n;
}

static int install(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: a blocked syscall should return */

    if (sigaction(sig, &sa, NULL) < 0)
    {
        perror("tetrisd: sigaction");
        return -1;
    }
    return 0;
}

/*
 * Wait for one child, but not forever.
 *
 * A session wedged writing to a dead client never sees the EOF that would make
 * it exit, and an unbounded waitpid would hang shutdown behind it - by which
 * point tetrisctl already has its 200 and is gone, so there is no second
 * lever. Poll briefly, then insist.
 */
static void reap_bounded(pid_t pid, int timeout_ms)
{
    const int step_ms = 10;

    for (int waited = 0; waited < timeout_ms; waited += step_ms)
    {
        pid_t r = waitpid(pid, NULL, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD))
            return;

        struct timespec ts = {.tv_sec = 0, .tv_nsec = step_ms * 1000000L};
        nanosleep(&ts, NULL);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* listener thread                                                     */
/* ------------------------------------------------------------------ */

/* Create the listening socket: socket + reuse + bind + listen. Returns the
 * fd, or -1 on failure. */
static int make_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Owns the listening socket: accepts clients and forks a session per client.
 * This thread is a plain dispatcher - all game work happens in the child.
 *
 * It polls rather than calling accept() directly, so that shutdown has
 * something to wake it with. See g_quit.
 */
static void *listener_thread(void *arg)
{
    (void)arg;

    int lfd = make_listen_socket(g_port);
    if (lfd < 0)
    {
        /* Nothing will ever be accepted, so do not leave the rest of the
         * daemon waiting on a listener that cannot work. */
        (void)log_send(LOG_ERROR,
                       "operation=listener_thread phase=complete port=%d "
                       "status=-1",
                       g_port);
        request_stop();
        return NULL;
    }
    (void)log_send(LOG_INFO,
                   "operation=listener_thread event=started port=%d "
                   "session_bin=%s status=0",
                   g_port, g_session_bin);

    struct pollfd p[3];
    p[0].fd = g_quit[0];
    p[0].events = POLLIN;
    p[1].fd = lfd;
    p[1].events = POLLIN;
    p[2].fd = g_relisten[0];
    p[2].events = POLLIN;
    int status = 0;

    while (true)
    {
        p[0].revents = p[1].revents = p[2].revents = 0;

        if (poll(p, 3, -1) < 0)
        {
            if (errno == EINTR)
                continue; /* interrupted by a signal */
            perror("poll");
            status = -1;
            break;
        }

        if (p[0].revents & POLLIN)
            break; /* checked first: quit wins */

        /*
         * A relisten swap is handled only when there is nothing to accept
         * this round. g_relisten is a level-triggered pipe - an unread byte
         * keeps it readable - so deferring it by one poll() iteration when an
         * accept is also ready costs nothing and means the swap below can
         * never close lfd out from under a connection already queued on it.
         */
        if (!(p[1].revents & POLLIN))
        {
            if (p[2].revents & POLLIN)
            {
                int nfd;
                if (read(g_relisten[0], &nfd, sizeof nfd) ==
                    (ssize_t)sizeof nfd)
                {
                    close(lfd);
                    lfd = nfd;
                    p[1].fd = lfd;
                    (void)log_send(LOG_INFO, "operation=listener_thread "
                                             "event=relistened status=0");
                }
            }
            continue;
        }

        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0)
        {
            if (errno == EINTR)
                continue; /* interrupted by a signal */
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* client left between poll and accept */
            perror("accept");
            status = -1;
            break;
        }

        /* Admin <-> session IPC channel, created BEFORE fork so both the
         * parent (admin end) and child (session end) inherit it. Pre-connected
         * UNIX-domain pair.
         *
         * SOCK_STREAM, not SOCK_DGRAM: the admin thread learns that a session
         * died only from this fd, and on macOS a datagram socketpair never
         * wakes a blocked poll() when its peer closes. Message boundaries come
         * from adminmsg_read/adminmsg_write instead. See adminmsg.h. */
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        {
            perror("socketpair");
            (void)log_send(LOG_ERROR,
                           "operation=listener_thread event=session_refused "
                           "reason=socketpair status=-1");
            close(cfd);
            continue;
        }

        /*
         * The admin's end is non-blocking; the session's end (sv[1]) stays
         * blocking.
         *
         * Asymmetric because the two sides have different exposure. The admin
         * writes to every session in the daemon, so one client that stops
         * reading would fill its socketpair and block the single thread that
         * routes for every room. A session has one peer and nothing else to
         * get on with, so blocking there costs only that session.
         */
        fcntl(sv[0], F_SETFL, O_NONBLOCK);

        /* Prepare argv while every parent thread still exists. After fork(),
         * the child may call only async-signal-safe functions until exec
         * replaces it. */
        char client_buf[16], admin_buf[16];
        snprintf(client_buf, sizeof client_buf, "%d", cfd);
        snprintf(admin_buf, sizeof admin_buf, "%d", sv[1]);

        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            (void)log_send(LOG_ERROR,
                           "operation=listener_thread event=session_refused "
                           "reason=fork status=-1");
            close(cfd);
            close(sv[0]);
            close(sv[1]);
            continue;
        }

        if (pid == 0)
        {
            /* ---- child: replace this process with the session binary ----
             * cfd and sv[1] stay open across exec (no CLOEXEC), so we hand the
             * session both by passing their fd numbers as arguments. exec gives
             * a clean image: none of the parent's threads/locks remain. */
            close(lfd);   /* the child never accepts   */
            close(sv[0]); /* the parent's admin end     */

            execl(g_session_bin, "session", client_buf, admin_buf,
                  (char *)NULL);

            /* write() and _exit() are async-signal-safe. Avoid stdio,
             * allocation, strerror(), or logging in this failed-exec window. */
            static const char msg[] = "tetrisd: session exec failed\n";
            ssize_t _long = write(STDERR_FILENO, msg, sizeof msg - 1);
            (void)_long;
            _exit(1);
        }

        /* ---- parent: keep listening ---- */
        close(cfd);   /* the child owns the client socket now  */
        close(sv[1]); /* the child owns its admin end now       */

        /* Register the new session with the admin thread. */
        NewSession ns = {.pid = pid, .fd = sv[0]};
        if (write(g_notify[1], &ns, sizeof ns) != (ssize_t)sizeof ns)
        {
            perror("notify admin");
            (void)log_send(LOG_ERROR,
                           "operation=listener_thread event=session_refused "
                           "session_pid=%ld reason=admin_notify status=-1",
                           (long)pid);
            close(sv[0]); /* admin will never learn of it -> drop  */
        }
        else
        {
            (void)log_send(LOG_INFO,
                           "operation=listener_thread event=session_accepted "
                           "session_pid=%ld status=0",
                           (long)pid);
        }
    }

    close(lfd);
    if (status != 0)
        request_stop();
    (void)log_send(status == 0 ? LOG_INFO : LOG_ERROR,
                   "operation=listener_thread phase=complete status=%d",
                   status);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* admin thread                                                        */
/* ------------------------------------------------------------------ */

/* Index of a session fd in the poll set, or -1. */
static int session_index(int fd)
{
    for (int i = ADMIN_FIXED_FDS; i < g_nfds; i++)
        if (g_fds[i].fd == fd)
            return i;
    return -1;
}

/*
 * Remove one session: leave its room, close, reap, compact the poll set.
 *
 * Factored out because two callers need it now - the scan loop below, when a
 * session exits by itself, and a kick, which arrives on the control pipe and
 * so cannot be handled inside that loop.
 */
static void drop_session(int i)
{
    (void)log_send(LOG_INFO,
                   "operation=drop_session event=dropped session_pid=%ld fd=%d "
                   "remaining=%d status=0",
                   (long)g_pids[i], g_fds[i].fd, g_nfds - ADMIN_FIXED_FDS - 1);
    client_close(g_fds[i].fd); /* leave room, reassign owner, notify */
    close(g_fds[i].fd);
    reap_bounded(g_pids[i], REAP_TIMEOUT_MS);

    g_fds[i] = g_fds[g_nfds - 1]; /* compact the arrays */
    g_pids[i] = g_pids[g_nfds - 1];
    g_nfds--;
}

/*
 * SIGHUP: reread .tetrishrc and, if listen_port changed, hand a freshly
 * bound socket to the listener thread.
 *
 * Only listen_port needs action here. Every other directive tetrisd itself
 * reads (cert_path, key_path, db_ipc, ...) is read fresh by each session -
 * a freshly forked and exec'd process, see listener_thread - the instant it
 * starts, with no help from this thread; rc_reload() below is what makes
 * that already-live path see the edit too. ctl_ipc is deliberately left
 * alone: rebinding the control socket on a config edit is exactly the kind
 * of self-inflicted unavailability Section 6 exists to prevent.
 */
static void reload_config(void)
{
    rc_reload();

    int new_port = g_active_port;
    rc_get_int("listen_port", g_active_port, 1, 65535, &new_port);

    if (new_port == g_active_port)
    {
        (void)log_send(LOG_INFO,
                       "operation=reload_config event=sighup_received "
                       "port=%d status=0",
                       g_active_port);
        return; /* nothing to relisten; other directives are already live */
    }

    int nfd = make_listen_socket(new_port);
    if (nfd < 0)
    {
        /* Bind failed (bad port, already in use, ...): the existing
         * listener is untouched, exactly what "replace the TCP listener
         * only after the new port binds successfully" requires. */
        (void)log_send(LOG_WARN,
                       "operation=reload_config event=relisten_failed "
                       "port=%d status=-1",
                       new_port);
        return;
    }

    if (write(g_relisten[1], &nfd, sizeof nfd) != (ssize_t)sizeof nfd)
    {
        perror("tetrisd: relisten pipe");
        close(nfd);
        (void)log_send(LOG_ERROR,
                       "operation=reload_config event=relisten_handoff_failed "
                       "port=%d status=-1",
                       new_port);
        return;
    }

    int old_port = g_active_port;
    g_active_port = new_port;
    (void)log_send(LOG_INFO,
                   "operation=reload_config event=sighup_received "
                   "old_port=%d port=%d status=0",
                   old_port, new_port);
}

/*
 * SIGUSR1: write one consistent room/player snapshot to the log.
 *
 * Only the admin thread may call room_snapshot()/player_snapshot() - see the
 * comment on those in room.h - which is why this waits for the same self-pipe
 * dispatch as everything else here rather than running in the signal handler.
 */
static void dump_state(void)
{
    static RoomInfo rooms[MAX_ROOMS];
    static PlayerInfo players[MAX_SESSIONS];
    int nr = room_snapshot(rooms, MAX_ROOMS);
    int np = player_snapshot(players, MAX_SESSIONS);

    (void)log_send(LOG_INFO,
                   "operation=dump_state event=sigusr1_received sessions=%d "
                   "rooms=%d status=0",
                   client_count(), room_count());

    for (int i = 0; i < nr; i++)
        (void)log_send(LOG_INFO,
                       "operation=dump_state room=%d phase=%d members=%d "
                       "owner_player=%d status=0",
                       rooms[i].id, rooms[i].phase, rooms[i].members,
                       rooms[i].owner_player);

    for (int i = 0; i < np; i++)
        (void)log_send(LOG_INFO,
                       "operation=dump_state player=%d room=%d pid=%ld "
                       "owner=%d score=%d lines=%d name=%s status=0",
                       players[i].player, players[i].room, (long)players[i].pid,
                       players[i].is_owner, players[i].score, players[i].lines,
                       players[i].name);
}

/* One control request: answer it, then carry out whatever it asked for. */
static void handle_ctl(const CtlReq *req)
{
    log_send(LOG_DEBUG, "admin command received verb=%d room=%d player=%d",
             req->verb, req->room, req->player);
    int kick_fd = -1;

    switch (ctl_dispatch(req, &kick_fd))
    {
    case CTL_AFTER_SHUTDOWN:
        /* The reply is already on the wire. Waking the accepting threads
         * is all that is left; main sequences the rest. */
        request_stop();
        break;

    case CTL_AFTER_KICK:
    {
        int i = session_index(kick_fd);
        if (i >= 0)
        {
            /* The victim has already been told (a 403 on its admin
             * socket). SIGTERM covers the case where it is wedged and
             * would never notice the socket closing. */
            kill(g_pids[i], SIGTERM);
            drop_session(i);
        }
        break;
    }

    case CTL_AFTER_NONE:
    default:
        break;
    }
    (void)log_send(LOG_INFO,
                   "operation=handle_ctl phase=complete verb=%d status=0",
                   req->verb);
}

/*
 * Close every session down, in the one place where that is safe.
 *
 * Runs only after main has joined both accepting threads, so nothing can fork
 * a new session underneath us. client_close is deliberately NOT called: its
 * job is to tell the roommates, and the roommates are being shut down in this
 * same loop.
 */
static void admin_teardown(void)
{
    (void)log_send(LOG_INFO,
                   "operation=admin_teardown event=started active_sessions=%d "
                   "status=0",
                   g_nfds - ADMIN_FIXED_FDS);
    /* Sessions the listener forked in its last moments, which never made it
     * into the poll set. Without this they would be orphaned with an open fd.
     */
    NewSession ns;
    while (read(g_notify[0], &ns, sizeof ns) == (ssize_t)sizeof ns)
    {
        close(ns.fd);
        kill(ns.pid, SIGTERM);
        reap_bounded(ns.pid, REAP_TIMEOUT_MS);
    }

    /* Control requests that arrived alongside the shutdown. Their clients are
     * waiting on a reply that is no longer coming; closing is the honest
     * answer, and it is what tetrisctl reports as "no reply from tetrisd". */
    CtlReq cr;
    while (read(g_ctl_notify[0], &cr, sizeof cr) == (ssize_t)sizeof cr)
        close(cr.fd);

    for (int i = g_nfds - 1; i >= ADMIN_FIXED_FDS; i--)
    {
        close(g_fds[i].fd); /* the session sees EOF */
        kill(g_pids[i], SIGTERM);
        reap_bounded(g_pids[i], REAP_TIMEOUT_MS);
        g_nfds--;
    }
    (void)log_send(LOG_INFO,
                   "operation=admin_teardown phase=complete status=0");
}

/*
 * Owns the room table: assigns room numbers and routes messages between the
 * client sessions. Multiplexes every session's admin socket (plus the notify
 * pipes) with one poll() - single thread, no locks on the room table.
 */
static void *admin_thread(void *arg)
{
    (void)log_send(LOG_INFO,
                   "operation=admin_thread event=started capacity=%d status=0",
                   MAX_SESSIONS);
    (void)arg;

    g_fds[0].fd = g_notify[0];
    g_fds[0].events = POLLIN;
    g_fds[1].fd = g_ctl_notify[0];
    g_fds[1].events = POLLIN;
    g_fds[2].fd = g_admin_quit[0];
    g_fds[2].events = POLLIN;
    g_fds[3].fd = g_reload[0];
    g_fds[3].events = POLLIN;
    g_fds[4].fd = g_dump[0];
    g_fds[4].events = POLLIN;
    g_nfds = ADMIN_FIXED_FDS;
    g_active_port = g_port; /* seed before any SIGHUP can be handled */

    for (;;)
    {
        if (poll(g_fds, g_nfds, -1) < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* (0) teardown, before anything else: once main says the accepting
         * threads are joined, there is nothing left worth servicing. */
        if (g_fds[2].revents & POLLIN)
            break;

        /* (1) new session registrations from the listener */
        if (g_fds[0].revents & POLLIN)
        {
            NewSession ns;
            while (read(g_notify[0], &ns, sizeof ns) == (ssize_t)sizeof ns)
            {
                if (g_nfds <= ADMIN_FIXED_FDS + MAX_SESSIONS - 1)
                {
                    g_fds[g_nfds].fd = ns.fd;
                    g_fds[g_nfds].events = POLLIN;
                    g_fds[g_nfds].revents = 0;
                    g_pids[g_nfds] = ns.pid;
                    g_nfds++;
                    client_add(ns.fd,
                               ns.pid); /* register with the room module */
                }
                else
                {
                    close(ns.fd); /* table full: drop (TODO: reject client) */
                }
            }
        }

        /* (2) admin commands from the control plane */
        if (g_fds[1].revents & POLLIN)
        {
            CtlReq cr;
            while (read(g_ctl_notify[0], &cr, sizeof cr) == (ssize_t)sizeof cr)
                handle_ctl(&cr);
        }

        /* (2b) SIGHUP: reload .tetrishrc */
        if (g_fds[3].revents & POLLIN)
        {
            char drain[16];
            while (read(g_reload[0], drain, sizeof drain) > 0)
                ; /* coalesce a burst of signals into one reload */
            reload_config();
        }

        /* (2c) SIGUSR1: dump room/player state to the log */
        if (g_fds[4].revents & POLLIN)
        {
            char drain[16];
            while (read(g_dump[0], drain, sizeof drain) > 0)
                ; /* coalesce a burst of signals into one dump */
            dump_state();
        }

        /* (3) messages / hangups from sessions */
        for (int i = ADMIN_FIXED_FDS; i < g_nfds; i++)
        {
            if (!(g_fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            AdminMsg msg;
            int r = adminmsg_read(g_fds[i].fd, &msg);

            if (r == 1)
            {
                client_handle(g_fds[i].fd, &msg); /* route the room command */
                continue;
            }

            /* r == 0 (session exited) or error -> drop this session */
            drop_session(i);
            i--; /* re-check the slot we moved in */
        }

        /*
         * (4) sessions that stopped draining their socketpair.
         *
         * room.c writes to these fds from inside its broadcasts and cannot
         * close them - this thread owns the poll set - so it records them and
         * we do it here, after the scan loop, where compacting g_fds is safe.
         *
         * Dropped rather than retried: the admin's ends are non-blocking so
         * that one stuck client cannot stall routing for every room, and a
         * peer that has let its buffer fill is not slow, it is gone.
         */
        for (;;)
        {
            /* Score and game-over messages can arrive from every member of a
             * room in one poll batch. Publish only the final standings state
             * from that batch instead of filling every session socketpair
             * with intermediate snapshots. */
            room_flush_updates();

            int fd = client_take_wedged();
            if (fd < 0)
                break;
            do
            {
                int i = session_index(fd);
                if (i >= 0)
                    drop_session(i);
                fd = client_take_wedged();
            } while (fd >= 0);
            /* Dropping a batch changes room standings. Loop once more to
             * publish that state before poll() can sleep indefinitely. */
        }
    }

    admin_teardown();
    (void)log_send(LOG_INFO, "operation=admin_thread phase=complete status=0");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Writing to a socket whose peer already died must not kill us. */
    signal(SIGPIPE, SIG_IGN);

    if (rc_config() != 0)
    {
        fprintf(stderr, "tetrisd: .tetrishrc missing a required directive\n");
        return 1;
    }

    /* atexit rather than a close at the end of main, so the error returns
     * below still hand over this process's drop count. */
    (void)log_open_configured();
    atexit(log_close);
    (void)log_send(LOG_INFO,
                   "operation=main event=started program=tetrisd port=%d "
                   "session_bin=%s status=0",
                   g_port, g_session_bin);

    if (pipe(g_notify) < 0 || pipe(g_ctl_notify) < 0 || pipe(g_quit) < 0 ||
        pipe(g_admin_quit) < 0 || pipe(g_reload) < 0 || pipe(g_dump) < 0 ||
        pipe(g_relisten) < 0)
    {
        perror("pipe");
        (void)log_send(LOG_ERROR,
                       "operation=main phase=complete program=tetrisd status=1 "
                       "reason=pipe");
        return 1;
    }
    /* Non-blocking read ends so the admin can drain all pending registrations
     * and commands without the final read() blocking. The quit pipes are read
     * only by poll(), never by read(), so their mode does not matter - but the
     * write end of g_quit is set non-blocking so a signal handler firing
     * repeatedly can never block inside write(). g_reload/g_dump follow the
     * same rule as g_quit: their write ends are the signal handlers
     * (on_reload/on_dump), so those are non-blocking too. g_relisten is
     * written only from admin_thread, a normal thread with nothing better to
     * do while a handoff of a few bytes completes, so its write end is left
     * blocking like g_notify's and g_ctl_notify's. */
    fcntl(g_notify[0], F_SETFL, O_NONBLOCK);
    fcntl(g_ctl_notify[0], F_SETFL, O_NONBLOCK);
    fcntl(g_quit[1], F_SETFL, O_NONBLOCK);
    fcntl(g_reload[0], F_SETFL, O_NONBLOCK);
    fcntl(g_reload[1], F_SETFL, O_NONBLOCK);
    fcntl(g_dump[0], F_SETFL, O_NONBLOCK);
    fcntl(g_dump[1], F_SETFL, O_NONBLOCK);
    fcntl(g_relisten[0], F_SETFL, O_NONBLOCK);

    /* Same teardown for `tetrisctl shutdown`, `kill -TERM` and Ctrl-C. */
    if (install(SIGTERM, on_terminate) < 0 || install(SIGINT, on_terminate) < 0)
    {
        (void)log_send(LOG_ERROR,
                       "operation=main phase=complete program=tetrisd status=1 "
                       "reason=signal");
        return 1;
    }

    /* `tetrisctl reload` raises this same signal (ctl_dispatch, inside this
     * process) rather than duplicating what it does, so there is exactly one
     * reload path to reason about. */
    if (install(SIGHUP, on_reload) < 0)
    {
        (void)log_send(LOG_ERROR,
                       "operation=main phase=complete program=tetrisd status=1 "
                       "reason=signal");
        return 1;
    }

    if (install(SIGUSR1, on_dump) < 0)
    {
        (void)log_send(LOG_ERROR,
                       "operation=main phase=complete program=tetrisd status=1 "
                       "reason=signal");
        return 1;
    }

    char sock_path[PATH_MAX];
    ctl_socket_path(sock_path, sizeof sock_path);

    if (ctl_open(sock_path, g_quit[0], g_quit[1], g_ctl_notify[1]) < 0)
    {
        return 1;
    }
    log_send(LOG_DEBUG, "tetrisd control plane ready socket=%s", sock_path);

    pthread_t listener, admin, ctl;
    int rc;

    rc = pthread_create(&admin, NULL, admin_thread, NULL);
    if (rc != 0)
    {
        fprintf(stderr, "pthread_create admin: %s\n", strerror(rc));
        (void)log_send(LOG_ERROR,
                       "operation=main phase=complete program=tetrisd status=1 "
                       "reason=admin_thread");
        return 1;
    }

    rc = pthread_create(&listener, NULL, listener_thread, NULL);
    if (rc != 0)
    {
        fprintf(stderr, "pthread_create listener: %s\n", strerror(rc));
        (void)log_send(LOG_ERROR,
                       "operation=main phase=complete program=tetrisd status=1 "
                       "reason=listener_thread");
        return 1;
    }

    rc = pthread_create(&ctl, NULL, ctl_thread, NULL);
    if (rc != 0)
    {
        fprintf(stderr, "pthread_create ctl: %s\n", strerror(rc));
        (void)log_send(LOG_ERROR,
                       "operation=main phase=complete program=tetrisd status=1 "
                       "reason=ctl_thread");
        return 1;
    }

    /*
     * Shutdown is sequenced here, and the order is the correctness argument.
     *
     * Both accepting threads are joined first, so by the time admin is told to
     * tear down, nothing can fork a session or accept a command underneath it.
     * The room table is then provably stable while it is being dismantled.
     */
    pthread_join(listener, NULL);
    pthread_join(ctl, NULL);

    const char b = 'x';
    if (write(g_admin_quit[1], &b, 1) < 0)
        perror("tetrisd: admin quit pipe");

    pthread_join(admin, NULL);
    (void)log_send(LOG_INFO,
                   "operation=main phase=complete program=tetrisd status=0");
    return 0;
}
