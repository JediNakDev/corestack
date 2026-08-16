/*
 * control_plane.c - daemon side: AF_UNIX listener, HTTTP parsing, admin verbs.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "libhtttp/htttp.h"
#include "libtetrisutil/limits.h" /* MAX_SESSIONS / MAX_ROOMS / MAX_USER_NAME */
#include "libtetrisutil/logmsg.h"
#include "tetrisctl/control_plane.h"
#include "tetrisd/room.h"

/* How long to wait on a control client that has connected but gone quiet.
 * It only blocks other admin commands, never the game, so this can be
 * generous - but not unbounded, or one wedged tetrisctl locks the plane. */
#define CTL_IO_TIMEOUT_MS 2000

#define CTL_LISTEN_BACKLOG 8

/* Set once by ctl_listen_open, read by the ctl thread only. */
static int g_lfd = -1;
static int g_quit_rd = -1;
static int g_quit_wr = -1;
static int g_notify_wr = -1;
static char g_sock_path[PATH_MAX];
static struct timespec g_started;

/* ---- helpers ------------------------------------------------------------ */

static long uptime_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - g_started.tv_sec);
}

static void set_timeout(int fd, int opt, int ms)
{
    struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
}

static const char *phase_name(int phase)
{
    switch (phase)
    {
    case SESSION_WAITING:
        return "waiting";
    case SESSION_PLAYING:
        return "playing";
    case SESSION_GAME_OVER:
        return "game_over";
    case SESSION_IDLE:
        return "idle";
    default:
        return "none";
    }
}

/*
 * Append `src` as a JSON string body (no surrounding quotes), escaping what
 * must be escaped.
 *
 * Player names are server-assigned "Player N" today, so nothing here can
 * currently produce a quote or a backslash. It is written anyway because the
 * day names come from clients is the day a name containing a quote turns every
 * reply into malformed JSON, and that bug would surface as tetrisctl printing
 * garbage rather than as anything pointing at this function.
 */
static int json_escape(char *out, size_t cap, const char *src)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++)
    {
        const char *esc = NULL;
        char ubuf[7];

        switch (*p)
        {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        default:
            if (*p < 0x20)
            {
                snprintf(ubuf, sizeof ubuf, "\\u%04x", *p);
                esc = ubuf;
            }
            break;
        }

        if (esc)
        {
            size_t len = strlen(esc);
            if (o + len >= cap)
                return -1;
            memcpy(out + o, esc, len);
            o += len;
        }
        else
        {
            if (o + 1 >= cap)
                return -1;
            out[o++] = (char)*p;
        }
    }
    if (o >= cap)
        return -1;
    out[o] = '\0';
    return (int)o;
}

/*
 * Write one response (ctl thread in tetrisd -> tetrisctl)
 * and close the connection.
 */

static void ctl_reply(int fd, int status, const char *body)
{
    static _Thread_local uint8_t out[CTL_MAX_FRAME];
    int final_status = status;

    htttp_response_t res;
    memset(&res, 0, sizeof res);
    res.status = status;

    if (body != NULL && body[0] != '\0')
    {
        htttp_header_set(res.headers, &res.n_headers, "Content-Type",
                         "application/json");
        res.body = (const uint8_t *)body;
        res.body_len = (uint32_t)strlen(body);
    }

    uint32_t len = sizeof out;
    int rc = htttp_serialize_response(&res, out, &len);
    int serialize_status = rc;

    if (rc != HTTTP_OK && status != 500)
    {
        memset(&res, 0, sizeof res);
        res.status = 500;
        final_status = 500;
        len = sizeof out;
        rc = htttp_serialize_response(&res, out, &len);
    }

    if (rc == HTTTP_OK)
    {
        int write_rc = ctl_frame_write(fd, out, len);
        if (serialize_status != HTTTP_OK)
            (void)log_send(LOG_ERROR,
                           "operation=ctl_reply phase=complete status=%d "
                           "serialize_status=%d write_status=%d",
                           final_status, serialize_status, write_rc);
        else if (write_rc != 0)
            (void)log_send(
                LOG_ERROR,
                "operation=ctl_reply phase=complete status=%d write_status=%d",
                final_status, write_rc);
    }
    else
    {
        log_send(
            LOG_ERROR,
            "operation=ctl_reply phase=complete status=%d serialize_status=%d",
            final_status, rc);
    }

    close(fd);
}

/* ---- listening socket --------------------------------------------------- */

int ctl_open(const char *path, int quit_rd, int quit_wr, int notify_wr)
{
    log_send(LOG_DEBUG, "opening control plane socket=%s", path);
    clock_gettime(CLOCK_MONOTONIC, &g_started);
    snprintf(g_sock_path, sizeof g_sock_path, "%s", path);
    g_quit_rd = quit_rd;
    g_quit_wr = quit_wr;
    g_notify_wr = notify_wr;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof addr.sun_path)
    {
        fprintf(stderr, "tetrisd: control socket path too long: %s\n", path);
        (void)log_send(LOG_ERROR,
                       "operation=ctl_open phase=complete status=-1");
        return -1;
    }
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    /*
     * Clear a leftover socket, but never anything else.
     *
     * A previous run killed with SIGKILL leaves its socket file behind and
     * bind() would fail with EADDRINUSE forever after. Unlinking blindly is
     * the other failure: a typo in ctl_ipc would then quietly delete whatever
     * that path names. lstat, not stat, so a symlink is judged as a symlink
     * rather than as whatever it points at.
     */
    struct stat st;
    if (lstat(path, &st) == 0)
    {
        if (!S_ISSOCK(st.st_mode))
        {
            fprintf(stderr, "tetrisd: refusing to replace non-socket %s\n",
                    path);
            (void)log_send(LOG_ERROR,
                           "operation=ctl_open phase=complete status=-1");
            return -1;
        }

        /*
         * A socket file on disk means one of two very different things, and
         * lstat cannot tell them apart: a daemon died without cleaning up, or
         * a daemon is running right now and this is its front door.
         *
         * Unlinking blindly gets the second case catastrophically wrong. The
         * running daemon keeps its listening fd - it never notices - but the
         * name is gone, so no tetrisctl can ever reach it again and the only
         * way to stop it is kill. Worse, the damage is done before this
         * process discovers it cannot have the TCP port either, so a second
         * start that was always going to fail takes out the first one's
         * control plane on its way down.
         *
         * Ask instead of assume: if connect() succeeds, somebody is serving.
         * ECONNREFUSED means the socket is stale and ours to clear.
         */
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0)
        {
            int live = connect(probe, (struct sockaddr *)&addr, sizeof addr);
            close(probe);
            if (live == 0)
            {
                fprintf(stderr,
                        "tetrisd: %s is already served by a running tetrisd\n",
                        path);
                (void)log_send(LOG_ERROR,
                               "operation=ctl_open phase=complete status=-1");
                return -1;
            }
        }
        unlink(path);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("tetrisd: ctl socket");
        (void)log_send(LOG_ERROR,
                       "operation=ctl_open phase=complete status=-1");
        return -1;
    }

    /*
     * bind() applies the umask to the socket file it creates, and bin/dspawn
     * daemonises us with umask(0) - which would publish a world-writable
     * control plane, letting any local user shut the server down. Narrow the
     * umask around the bind rather than chmod()ing afterwards: a chmod leaves
     * a window where the permissive mode is already on disk.
     */
    mode_t old_umask = umask(077);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old_umask);

    if (rc < 0)
    {
        perror("tetrisd: ctl bind");
        close(fd);
        (void)log_send(LOG_ERROR,
                       "operation=ctl_open phase=complete status=-1");
        return -1;
    }

    /* Belt and braces: some platforms ignore the umask for socket files. */
    if (chmod(path, 0600) < 0)
        perror("tetrisd: ctl chmod");

    if (listen(fd, CTL_LISTEN_BACKLOG) < 0)
    {
        perror("tetrisd: ctl listen");
        close(fd);
        unlink(path);
        (void)log_send(LOG_ERROR,
                       "operation=ctl_open phase=complete status=-1");
        return -1;
    }

    g_lfd = fd;
    (void)log_send(LOG_INFO, "operation=ctl_open phase=complete status=0");
    return 0;
}

/* ---- request parsing (ctl thread) --------------------------------------- */

/* Parse "/room/<id>/player/<pid>". Returns true and fills both ids on match. */
static bool parse_kick_path(const char *path, int *room, int *player)
{
    return sscanf(path, "/room/%d/player/%d", room, player) == 2;
}

/*
 * Map a parsed request onto a verb. Path shape is checked here, on the ctl
 * thread, so the admin thread never receives a request it has to validate.
 */
static void classify(const htttp_request_t *req, CtlReq *out)
{
    out->verb = CTL_VERB_BAD;
    out->room = 0;
    out->player = 0;

    if (strcmp(req->method, "STATUS") == 0)
        out->verb = CTL_VERB_STATUS;
    else if (strcmp(req->method, "SHUTDOWN") == 0)
        out->verb = CTL_VERB_SHUTDOWN;
    else if (strcmp(req->method, "ROOMS") == 0)
        out->verb = CTL_VERB_ROOMS;
    else if (strcmp(req->method, "PLAYERS") == 0)
        out->verb = CTL_VERB_PLAYERS;
    else if (strcmp(req->method, "RELOAD") == 0)
        out->verb = CTL_VERB_RELOAD;
    else if (strcmp(req->method, "KICK") == 0)
    {
        if (parse_kick_path(req->path, &out->room, &out->player))
            out->verb = CTL_VERB_KICK;
    }
}

/*
 * after accept() from ctl -> read request from ctl then hand to admin
 */
static void handle_conn(int cfd)
{
    static _Thread_local uint8_t frame[CTL_MAX_FRAME];

    set_timeout(cfd, SO_RCVTIMEO, CTL_IO_TIMEOUT_MS);
    set_timeout(cfd, SO_SNDTIMEO, CTL_IO_TIMEOUT_MS);

    uint32_t len = 0;
    if (ctl_frame_read(cfd, frame, sizeof frame, &len) != 0)
    {
        log_send(
            LOG_WARN,
            "operation=handle_conn phase=complete status=400 reason=frame");
        close(cfd); /* truncated, oversized, or just hung up */
        return;
    }

    htttp_request_t req;
    if (htttp_parse_request(frame, len, &req) != HTTTP_OK)
    {
        (void)log_send(LOG_WARN,
                       "operation=handle_conn phase=complete status=400 "
                       "reason=parse");
        ctl_reply(cfd, 400, "{\"error\":\"malformed request\"}");
        return;
    }

    CtlReq cr;
    memset(&cr, 0, sizeof cr);
    cr.fd = cfd;

    /*----- translate to CtlReq ----------*/

    classify(&req, &cr);
    log_send(LOG_DEBUG, "control request parsed method=%s path=%s verb=%d",
             req.method, req.path, cr.verb);

    if (cr.verb == CTL_VERB_BAD)
    {
        (void)log_send(LOG_WARN,
                       "operation=handle_conn phase=complete status=400 "
                       "reason=command");
        ctl_reply(cfd, 400, "{\"error\":\"unknown command\"}");
        return;
    }

    /*
     * Ownership of cfd transfers with this write. After it succeeds the admin
     * thread will reply and close, and touching cfd here would race it.
     */
    if (write(g_notify_wr, &cr, sizeof cr) != (ssize_t)sizeof cr)
    {
        perror("tetrisd: ctl notify");
        (void)log_send(LOG_ERROR,
                       "operation=handle_conn phase=complete method=%s "
                       "status=500 reason=busy",
                       req.method);
        ctl_reply(cfd, 500, "{\"error\":\"control plane busy\"}");
    }
    else
        log_send(LOG_INFO,
                 "operation=handle_conn phase=complete method=%s status=0",
                 req.method);
}

void *ctl_thread(void *arg)
{
    log_send(LOG_DEBUG, "control listener thread running socket=%s",
             g_sock_path);
    (void)arg;

    /*
     * Never call accept() blind.
     *
     * A thread parked in accept() cannot be woken: closing its listening fd
     * from elsewhere races fd reuse, and a signal races the moment between the
     * flag check and entering the syscall. A pipe is level-triggered, so a
     * quit byte written before this poll() is even reached still returns
     * immediately - there is no window to lose.
     *
     * The quit byte is deliberately never read. Two threads wait on the same
     * read end (this one and the listener); whichever consumed the byte would
     * leave the other blocked forever. Left in the pipe it stays readable for
     * both, permanently, which is exactly what "stop" should mean.
     */
    struct pollfd p[2];
    p[0].fd = g_quit_rd;
    p[0].events = POLLIN;
    p[1].fd = g_lfd;
    p[1].events = POLLIN;

    bool asked_to_stop = false;

    for (;;)
    {
        p[0].revents = p[1].revents = 0;

        if (poll(p, 2, -1) < 0)
        {
            if (errno == EINTR)
                continue;
            perror("tetrisd: ctl poll");
            break;
        }

        if (p[0].revents & POLLIN)
        {
            asked_to_stop = true;
            break; /* checked first: quit wins */
        }

        if (!(p[1].revents & POLLIN))
            continue;

        int cfd = accept(g_lfd, NULL, NULL);
        if (cfd < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            perror("tetrisd: ctl accept");
            break;
        }

        handle_conn(cfd);
    }

    close(g_lfd);
    g_lfd = -1;

    /* This module created the socket file, so it removes it. Leaving it would
     * make the next start look like a stale-socket recovery every time. */
    unlink(g_sock_path);

    /*
     * If we got here on an error rather than on the quit pipe, the daemon is
     * still running and has just lost its only control channel - so the socket
     * is gone while tetrisd is alive, tetrisctl reports "is tetrisd running?"
     * for a server that is, and nothing short of kill can stop it. Take the
     * daemon down instead of failing open, exactly as listener_thread does
     * when its own socket cannot be created.
     */
    if (!asked_to_stop)
    {
        fprintf(stderr, "tetrisd: control plane failed, shutting down\n");
        const char b = 'x';
        ssize_t n = write(g_quit_wr, &b, 1);
        (void)n;
    }
    (void)log_send(asked_to_stop ? LOG_INFO : LOG_ERROR,
                   "operation=ctl_thread phase=complete status=%d",
                   asked_to_stop ? 0 : -1);
    return NULL;
}

/* ---- verbs (admin thread) ----------------------------------------------- */

/*
 * Bodies are built with snprintf into a static buffer, tracking the offset by
 * hand. Every append checks the remaining space and bails to 500 rather than
 * emitting a truncated array: half a JSON document is worse than an error,
 * because the client cannot tell the difference between "the server is broken"
 * and "there are no more rooms".
 */
static char g_body[CTL_MAX_FRAME / 2];

#define BODY_APPEND(off, ...)                                                  \
    do                                                                         \
    {                                                                          \
        int _n = snprintf(g_body + (off), sizeof g_body - (off), __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= sizeof g_body - (off))                     \
            return -1;                                                         \
        (off) += _n;                                                           \
    } while (0)

static int build_status(void)
{
    int off = 0;
    BODY_APPEND(off, "{\"uptime\":%ld,\"sessions\":%d,\"rooms\":%d}",
                uptime_seconds(), client_count(), room_count());
    return off;
}

static int build_rooms(void)
{
    static RoomInfo rooms[MAX_ROOMS];
    int n = room_snapshot(rooms, MAX_ROOMS);
    int off = 0;

    BODY_APPEND(off, "[");
    for (int i = 0; i < n; i++)
    {
        BODY_APPEND(
            off, "%s{\"id\":%d,\"phase\":\"%s\",\"members\":%d,\"owner\":%d}",
            i ? "," : "", rooms[i].id, phase_name(rooms[i].phase),
            rooms[i].members, rooms[i].owner_player);
    }
    BODY_APPEND(off, "]");
    return off;
}

static int build_players(void)
{
    static PlayerInfo players[MAX_SESSIONS];
    int n = player_snapshot(players, MAX_SESSIONS);
    int off = 0;

    BODY_APPEND(off, "[");
    for (int i = 0; i < n; i++)
    {
        char name[MAX_USER_NAME * 6]; /* worst case: every byte -> \uXXXX */
        if (json_escape(name, sizeof name, players[i].name) < 0)
            return -1;

        BODY_APPEND(off,
                    "%s{\"room\":%d,\"player\":%d,\"pid\":%d,\"owner\":%s,"
                    "\"score\":%d,\"lines\":%d,\"name\":\"%s\"}",
                    i ? "," : "", players[i].room, players[i].player,
                    (int)players[i].pid, players[i].is_owner ? "true" : "false",
                    players[i].score, players[i].lines, name);
    }
    BODY_APPEND(off, "]");
    return off;
}

/*----- process ctlreq and call reply --------------------*/

CtlAfter ctl_dispatch(const CtlReq *req, int *kick_fd)
{
    log_send(LOG_DEBUG,
             "dispatching control command verb=%d room=%d player=%d fd=%d",
             req->verb, req->room, req->player, req->fd);
    /*
     * A control client that stops reading must not wedge the admin thread. The
     * ctl thread already set this on the fd, but the option travels with the
     * file description, not the thread - setting it again costs nothing and
     * documents that this write is deliberately bounded.
     */
    set_timeout(req->fd, SO_SNDTIMEO, CTL_IO_TIMEOUT_MS);

    switch (req->verb)
    {
    case CTL_VERB_STATUS:
        if (build_status() < 0)
        {
            (void)log_send(
                LOG_ERROR,
                "operation=ctl_dispatch phase=complete verb=%d status=500",
                req->verb);
            ctl_reply(req->fd, 500, NULL);
            break;
        }
        (void)log_send(
            LOG_INFO,
            "operation=ctl_dispatch phase=complete verb=%d status=200",
            req->verb);
        ctl_reply(req->fd, 200, g_body);
        break;

    case CTL_VERB_ROOMS:
        if (build_rooms() < 0)
        {
            (void)log_send(
                LOG_ERROR,
                "operation=ctl_dispatch phase=complete verb=%d status=500",
                req->verb);
            ctl_reply(req->fd, 500, NULL);
            break;
        }
        (void)log_send(
            LOG_INFO,
            "operation=ctl_dispatch phase=complete verb=%d status=200",
            req->verb);
        ctl_reply(req->fd, 200, g_body);
        break;

    case CTL_VERB_PLAYERS:
        if (build_players() < 0)
        {
            (void)log_send(
                LOG_ERROR,
                "operation=ctl_dispatch phase=complete verb=%d status=500",
                req->verb);
            ctl_reply(req->fd, 500, NULL);
            break;
        }
        (void)log_send(
            LOG_INFO,
            "operation=ctl_dispatch phase=complete verb=%d status=200",
            req->verb);
        ctl_reply(req->fd, 200, g_body);
        break;

    case CTL_VERB_KICK:
    {
        int fd = client_fd_by_player(req->room, req->player);
        if (fd < 0)
        {
            (void)log_send(
                LOG_WARN,
                "operation=ctl_dispatch phase=complete verb=%d room=%d "
                "player=%d status=404",
                req->verb, req->room, req->player);
            ctl_reply(req->fd, 404, "{\"error\":\"no such player\"}");
            break;
        }

        /*
         * Tell the victim before dropping it. REJECT_* values ARE HTTTP
         * status codes (adminmsg.h), so the session forwards this straight
         * to the wire and the player sees a 403 instead of an unexplained
         * disconnect - with no change to the session or the client.
         *
         * Reply to the operator first: once client_close runs, this
         * function is committed, and a kick that succeeded but reported
         * failure is the worse outcome to debug.
         */
        ctl_reply(req->fd, 200, "{\"kicked\":true}");

        /*
         * Bound this write. adminmsg_write loops until all 500 bytes are out,
         * and a session that has stopped reading - which is exactly what gets
         * a player kicked - fills the 8 KiB socketpair buffer after ~16
         * messages. Unbounded, that blocks the admin thread forever, defeating
         * the bounded reap the caller performs immediately afterwards.
         *
         * Losing the 403 is acceptable; the SIGTERM and the reap that follow
         * do not depend on it.
         */
        set_timeout(fd, SO_SNDTIMEO, CTL_IO_TIMEOUT_MS);

        AdminMsg m;
        memset(&m, 0, sizeof m);
        m.type = ADMIN_REJECT;
        m.reason = REJECT_NOT_OWNER; /* 403 */
        (void)adminmsg_write(fd, &m);

        *kick_fd = fd;
        (void)log_send(
            LOG_INFO,
            "operation=ctl_dispatch phase=complete verb=%d room=%d player=%d "
            "status=200",
            req->verb, req->room, req->player);
        return CTL_AFTER_KICK;
    }

    case CTL_VERB_SHUTDOWN:
        /* Reply before anything begins to come down: once teardown starts
         * the client would see an EOF it cannot distinguish from a crash. */
        (void)log_send(
            LOG_INFO,
            "operation=ctl_dispatch phase=complete verb=%d status=200",
            req->verb);
        ctl_reply(req->fd, 200, "{\"shutdown\":true}");
        return CTL_AFTER_SHUTDOWN;

    case CTL_VERB_RELOAD:
        if (kill(getpid(), SIGHUP) < 0)
        {
            (void)log_send(
                LOG_ERROR,
                "operation=ctl_dispatch phase=complete verb=%d status=500",
                req->verb);
            ctl_reply(req->fd, 500, "{\"error\":\"could not signal self\"}");
            break;
        }
        (void)log_send(
            LOG_INFO,
            "operation=ctl_dispatch phase=complete verb=%d status=200",
            req->verb);
        ctl_reply(req->fd, 200, "{\"reload\":true}");
        break;

    default:
        (void)log_send(
            LOG_WARN,
            "operation=ctl_dispatch phase=complete verb=%d status=400",
            req->verb);
        ctl_reply(req->fd, 400, "{\"error\":\"unknown command\"}");
        break;
    }

    return CTL_AFTER_NONE;
}
