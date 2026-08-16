/*
 * main.c - ballotd entry point.
 *
 * Two channels into one bb_ctx (see include/ballotd/control_plane.h and
 * include/ballotd/adminmsg.h for why the shape is what it is):
 *
 *   listener_thread : TCP + tetrissh, accept -> socketpair -> fork/exec
 *                      bin/ballot_session per voter connection
 *   ctl_thread       : local AF_UNIX socket for ballotctl (control_plane.c)
 *   admin_thread     : poll()s every worker socketpair + the ctl-notify pipe
 *                      + the new-worker notify pipe; owns the ONE bb_ctx
 *                      (no mutex needed - single thread, matching bb_ctx's
 *                      own write-lock assumption that callers are already
 *                      serialized)
 *
 * Mirrors tetrisd.c's shape closely: same pipe-based handoff, same
 * quit-pipe shutdown sequencing (both accepting threads joined before
 * admin_thread is told to tear down, so nothing can still be forking or
 * forwarding while bb_ctx is destroyed).
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#include "ballotd/adminmsg.h"
#include "ballotd/control_plane.h"
#include "ballotd/dispatch.h"
#include "libballotbrain/ballotbrain.h"
#include "libballotbrain/db.h"
#include "libballotclient/codec.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "libtetrisutil/rc.h"

#define DEFAULT_PORT                                                           \
  7676 /* TODO: confirm no collision once other daemons pick theirs */
#define DEFAULT_CERT "auth/server_signed.crt"
#define DEFAULT_KEY "auth/private_key.pem"
#define LISTEN_BACKLOG 16
#define SESSION_BIN "bin/ballot_session"
#define REAP_TIMEOUT_MS 500
#define MAX_WORKERS 254

typedef struct {
  int port;
  char cert_path[PATH_MAX];
  char key_path[PATH_MAX];
  char ctl_path[PATH_MAX];
  char db_dir[PATH_MAX];  /* db_ensure_table() target; db_dir rc key */
  char db_sock[PATH_MAX]; /* SocketRunner unix socket; db_ipc rc key */
  int db_timeout_ms;      /* db_timeout rc key */
} ballotd_opts_t;

/* One new worker, announced by listener_thread to admin_thread. */
typedef struct {
  pid_t pid;
  int fd;
} NewWorker;

static int g_notify[2];     /* listener -> admin: NewWorker */
static int g_ctl_notify[2]; /* ctl_thread -> admin: BallotdCtlReq (see
                               control_plane.h) */
static int g_quit[2]; /* listener_thread + ctl_thread stop signal (polled, never
                       * read - see the note on tetrisd's identical g_quit) */
static int g_admin_quit[2]; /* main -> admin: teardown may begin */

#define ADMIN_FIXED_FDS 3 /* g_notify, g_ctl_notify, g_admin_quit */
static struct pollfd g_fds[ADMIN_FIXED_FDS + MAX_WORKERS];
static pid_t g_pids[ADMIN_FIXED_FDS + MAX_WORKERS];
static int g_nfds = ADMIN_FIXED_FDS;

static bb_ctx *g_ctx;

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *out, const char *argv0) {
  fprintf(out,
          "usage: %s [-p port] [-c cert] [-k key] [-C ctl_socket] [-d db_dir] "
          "[-i db_sock] [-h]\n"
          "  -p port        TCP port for the voter channel (default %d)\n"
          "  -c cert        server certificate PEM (default %s)\n"
          "  -k key         server private key PEM (default %s)\n"
          "  -C ctl_socket  admin Unix socket path (default %s)\n"
          "  -d db_dir      SimpleDB data directory, table provisioning "
          "(default %s)\n"
          "  -i db_sock     SocketRunner unix socket (default %s)\n"
          "  -h             show this help\n",
          argv0, DEFAULT_PORT, DEFAULT_CERT, DEFAULT_KEY, CTL_SOCK_DEFAULT,
          DB_DEFAULT_DIR, DB_DEFAULT_IPC);
}

/* ------------------------------------------------------------------ */
/* shutdown plumbing (mirrors tetrisd.c)                               */
/* ------------------------------------------------------------------ */

static void request_stop(void) {
  const char b = 'x';
  if (write(g_quit[1], &b, 1) < 0 && errno != EAGAIN)
    perror("ballotd: quit pipe");
}

static void on_terminate(int sig) {
  (void)sig;
  const char b = 'x';
  ssize_t n = write(g_quit[1], &b, 1);
  (void)n;
}

static int install(int sig, void (*fn)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* no SA_RESTART: a blocked syscall should return */

  if (sigaction(sig, &sa, NULL) < 0) {
    perror("ballotd: sigaction");
    return -1;
  }
  return 0;
}

static void reap_bounded(pid_t pid, int timeout_ms) {
  const int step_ms = 10;

  for (int waited = 0; waited < timeout_ms; waited += step_ms) {
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
/* listener_thread: TCP + tetrissh, fork/exec bin/ballot_session       */
/* ------------------------------------------------------------------ */

static int make_listen_socket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
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

  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    perror("bind");
    close(fd);
    return -1;
  }
  if (listen(fd, LISTEN_BACKLOG) < 0) {
    perror("listen");
    close(fd);
    return -1;
  }
  return fd;
}

static void *listener_thread(void *arg) {
  const ballotd_opts_t *opts = arg;

  int lfd = make_listen_socket(opts->port);
  if (lfd < 0) {
    request_stop();
    return NULL;
  }

  struct pollfd p[2];
  p[0].fd = g_quit[0];
  p[0].events = POLLIN;
  p[1].fd = lfd;
  p[1].events = POLLIN;

  while (true) {
    p[0].revents = p[1].revents = 0;

    if (poll(p, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    if (p[0].revents & POLLIN)
      break; /* checked first: quit wins */
    if (!(p[1].revents & POLLIN))
      continue;

    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      perror("accept");
      break;
    }

    /* worker <-> admin IPC channel, created before fork so both ends are
     * inherited. SOCK_STREAM, not SOCK_DGRAM: same reason as tetrisd's
     * socketpair - reliable POLLHUP/0-byte-read on peer death. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
      perror("socketpair");
      close(cfd);
      continue;
    }

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      close(cfd);
      close(sv[0]);
      close(sv[1]);
      continue;
    }

    if (pid == 0) {
      /* child: become bin/ballot_session. cfd and sv[1] have no CLOEXEC, so
       * they survive exec; hand them over as argv, along with the cert/key
       * paths the worker needs for its own handshake. */
      close(lfd);
      close(sv[0]);

      char client_buf[16], admin_buf[16];
      snprintf(client_buf, sizeof client_buf, "%d", cfd);
      snprintf(admin_buf, sizeof admin_buf, "%d", sv[1]);
      execl(SESSION_BIN, "ballot_session", client_buf, admin_buf,
            opts->cert_path, opts->key_path, (char *)NULL);

      fprintf(stderr, "ballotd: exec %s: %s\n", SESSION_BIN, strerror(errno));
      _exit(1);
    }

    /* parent: keep listening */
    close(cfd);
    close(sv[1]);

    NewWorker nw = {.pid = pid, .fd = sv[0]};
    if (write(g_notify[1], &nw, sizeof nw) != (ssize_t)sizeof nw) {
      perror("ballotd: notify admin");
      close(sv[0]);
    }
  }

  close(lfd);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* admin_thread: the ONE bb_ctx owner                                  */
/* ------------------------------------------------------------------ */

static void drop_worker(int i) {
  close(g_fds[i].fd);
  reap_bounded(g_pids[i], REAP_TIMEOUT_MS);

  g_fds[i] = g_fds[g_nfds - 1];
  g_pids[i] = g_pids[g_nfds - 1];
  g_nfds--;
}

/* Answer one admin request: dispatch, encode, write straight to its fd
 * (plaintext, length-prefixed - the ctl framing, not tetrissh), close. */
static void handle_ctl_req(const BallotdCtlReq *cr) {
  BallotdResp resp;
  ballotd_dispatch(g_ctx, &cr->req, &resp.resp);

  uint8_t out[CTL_MAX_FRAME];
  uint32_t len = sizeof out;
  if (bcl_encode_response(cr->req.op, &resp.resp, out, &len) == 0)
    ctl_frame_write(cr->fd, out, len);

  close(cr->fd);
}

static void admin_teardown(void) {
  /* Workers listener_thread forked in its last moments, never registered. */
  NewWorker nw;
  while (read(g_notify[0], &nw, sizeof nw) == (ssize_t)sizeof nw) {
    close(nw.fd);
    kill(nw.pid, SIGTERM);
    reap_bounded(nw.pid, REAP_TIMEOUT_MS);
  }

  /* Admin requests that arrived alongside shutdown: close rather than hang
   * the waiting ballotctl. */
  BallotdCtlReq cr;
  while (read(g_ctl_notify[0], &cr, sizeof cr) == (ssize_t)sizeof cr)
    close(cr.fd);

  for (int i = g_nfds - 1; i >= ADMIN_FIXED_FDS; i--) {
    close(g_fds[i].fd);
    kill(g_pids[i], SIGTERM);
    reap_bounded(g_pids[i], REAP_TIMEOUT_MS);
    g_nfds--;
  }
}

static void *admin_thread(void *arg) {
  (void)arg;

  g_fds[0].fd = g_notify[0];
  g_fds[0].events = POLLIN;
  g_fds[1].fd = g_ctl_notify[0];
  g_fds[1].events = POLLIN;
  g_fds[2].fd = g_admin_quit[0];
  g_fds[2].events = POLLIN;
  g_nfds = ADMIN_FIXED_FDS;

  for (;;) {
    if (poll(g_fds, (nfds_t)g_nfds, -1) < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    /* (0) teardown, before anything else. */
    if (g_fds[2].revents & POLLIN)
      break;

    /* (1) new voter workers from listener_thread */
    if (g_fds[0].revents & POLLIN) {
      NewWorker nw;
      while (read(g_notify[0], &nw, sizeof nw) == (ssize_t)sizeof nw) {
        if (g_nfds <= ADMIN_FIXED_FDS + MAX_WORKERS - 1) {
          g_fds[g_nfds].fd = nw.fd;
          g_fds[g_nfds].events = POLLIN;
          g_fds[g_nfds].revents = 0;
          g_pids[g_nfds] = nw.pid;
          g_nfds++;
        } else {
          close(nw.fd); /* table full: drop (TODO: reject client) */
        }
      }
    }

    /* (2) admin requests from ctl_thread */
    if (g_fds[1].revents & POLLIN) {
      BallotdCtlReq cr;
      while (read(g_ctl_notify[0], &cr, sizeof cr) == (ssize_t)sizeof cr)
        handle_ctl_req(&cr);
    }

    /* (3) requests / hangups from voter workers */
    for (int i = ADMIN_FIXED_FDS; i < g_nfds; i++) {
      if (!(g_fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;

      BallotdReq req;
      int r = ballotmsg_read_req(g_fds[i].fd, &req);

      if (r == 1) {
        BallotdResp resp;
        ballotd_dispatch(g_ctx, &req.req, &resp.resp);
        /* Best effort: if this fails the worker is already gone and the
         * next pass's POLLHUP/EOF drops it - nothing more to do from here. */
        ballotmsg_write_resp(g_fds[i].fd, &resp);
        continue;
      }

      /* r == 0 (worker exited) or an error: drop it. */
      drop_worker(i);
      i--; /* re-check the slot we moved in */
    }
  }

  admin_teardown();
  return NULL;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  ballotd_opts_t opts;
  opts.port = DEFAULT_PORT;
  snprintf(opts.cert_path, sizeof opts.cert_path, "%s", DEFAULT_CERT);
  snprintf(opts.key_path, sizeof opts.key_path, "%s", DEFAULT_KEY);
  snprintf(opts.ctl_path, sizeof opts.ctl_path, "%s", CTL_SOCK_DEFAULT);
  snprintf(opts.db_dir, sizeof opts.db_dir, "%s", DB_DEFAULT_DIR);
  snprintf(opts.db_sock, sizeof opts.db_sock, "%s", DB_DEFAULT_IPC);
  opts.db_timeout_ms = DB_DEFAULT_TIMEOUT_MS;

  (void)rc_get_int("ballotd_port", DEFAULT_PORT, 1, 65535, &opts.port);
  (void)rc_get("ballotd_cert", DEFAULT_CERT, opts.cert_path, sizeof opts.cert_path);
  (void)rc_get("ballotd_key", DEFAULT_KEY, opts.key_path, sizeof opts.key_path);
  (void)rc_get("ballotd_ctl_ipc", CTL_SOCK_DEFAULT, opts.ctl_path, sizeof opts.ctl_path);
  (void)rc_get("db_dir", DB_DEFAULT_DIR, opts.db_dir, sizeof opts.db_dir);
  (void)rc_get("db_ipc", DB_DEFAULT_IPC, opts.db_sock, sizeof opts.db_sock);
  (void)rc_get_int("db_timeout", DB_DEFAULT_TIMEOUT_MS, DB_TIMEOUT_MIN_MS,
                   DB_TIMEOUT_MAX_MS, &opts.db_timeout_ms);

  int opt;
  while ((opt = getopt(argc, argv, "p:c:k:C:d:i:h")) != -1) {
    switch (opt) {
    case 'p': {
      char *end;
      long n = strtol(optarg, &end, 10);
      if (*end != '\0' || n <= 0 || n >= 65536) {
        fprintf(stderr, "ballotd: invalid port '%s'\n", optarg);
        return 2;
      }
      opts.port = (int)n;
      break;
    }
    case 'c':
      snprintf(opts.cert_path, sizeof opts.cert_path, "%s", optarg);
      break;
    case 'k':
      snprintf(opts.key_path, sizeof opts.key_path, "%s", optarg);
      break;
    case 'C':
      snprintf(opts.ctl_path, sizeof opts.ctl_path, "%s", optarg);
      break;
    case 'd':
      snprintf(opts.db_dir, sizeof opts.db_dir, "%s", optarg);
      break;
    case 'i':
      snprintf(opts.db_sock, sizeof opts.db_sock, "%s", optarg);
      break;
    case 'h':
      usage(stdout, argv[0]);
      return 0;
    default:
      usage(stderr, argv[0]);
      return 2;
    }
  }
  if (optind != argc) {
    fprintf(stderr, "ballotd: unexpected argument '%s'\n", argv[optind]);
    usage(stderr, argv[0]);
    return 2;
  }

  /* A dead client/worker/ctl socket must not kill us. */
  signal(SIGPIPE, SIG_IGN);

  if (pipe(g_notify) < 0 || pipe(g_ctl_notify) < 0 || pipe(g_quit) < 0 ||
      pipe(g_admin_quit) < 0) {
    perror("pipe");
    return 1;
  }
  /* Non-blocking read ends so admin_thread can drain everything pending
   * without the final read() blocking (see tetrisd.c's identical note).
   * The write end of g_quit is non-blocking so a repeatedly-firing signal
   * handler can never block inside write(). */
  fcntl(g_notify[0], F_SETFL, O_NONBLOCK);
  fcntl(g_ctl_notify[0], F_SETFL, O_NONBLOCK);
  fcntl(g_quit[1], F_SETFL, O_NONBLOCK);

  if (install(SIGTERM, on_terminate) < 0 || install(SIGINT, on_terminate) < 0)
    return 1;

  /* Bind the admin socket before any thread starts: failure here must stop
   * the daemon outright, not race a thread that is already running. */
  if (ctl_open(opts.ctl_path, g_quit[0], g_quit[1], g_ctl_notify[1]) < 0)
    return 1;

  /*
   * Provision every BallotBox table before doing anything else. Like
   * bin/tetrisdb ensuring TETRISAUTH_DB_TABLE in its own main() before
   * spawning the runner, this only takes effect if it runs before the
   * SocketRunner has started for this db_dir - the runner reads catalog.txt
   * once at startup and never again (db_ensure_table's contract). It is
   * therefore purely a filesystem operation here, independent of whether the
   * runner is currently reachable.
   */
  if (db_ensure_table(opts.db_dir, BB_DB_TABLE_ELECTION,
                       BB_DB_SCHEMA_ELECTION) != 0 ||
      db_ensure_table(opts.db_dir, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) !=
          0 ||
      db_ensure_table(opts.db_dir, BB_DB_TABLE_ELIGIBLE,
                       BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      db_ensure_table(opts.db_dir, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) !=
          0 ||
      db_ensure_table(opts.db_dir, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) !=
          0 ||
      db_ensure_table(opts.db_dir, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) !=
          0) {
    fprintf(stderr, "ballotd: failed to provision tables under '%s'\n",
            opts.db_dir);
    return 1;
  }

  g_ctx = bb_create();
  if (g_ctx == NULL) {
    fprintf(stderr, "ballotd: bb_create failed\n");
    return 1;
  }

  db_socket_opts_t db_opts;
  db_socket_opts_load(&db_opts);
  snprintf(db_opts.sock, sizeof db_opts.sock, "%s", opts.db_sock);
  db_opts.timeout_ms = opts.db_timeout_ms;
  bb_set_db_opts(g_ctx, &db_opts);

  pthread_t listener, admin, ctl;
  int rc;

  rc = pthread_create(&admin, NULL, admin_thread, NULL);
  if (rc != 0) {
    fprintf(stderr, "pthread_create admin: %s\n", strerror(rc));
    return 1;
  }

  rc = pthread_create(&listener, NULL, listener_thread, &opts);
  if (rc != 0) {
    fprintf(stderr, "pthread_create listener: %s\n", strerror(rc));
    return 1;
  }

  rc = pthread_create(&ctl, NULL, ctl_thread, NULL);
  if (rc != 0) {
    fprintf(stderr, "pthread_create ctl: %s\n", strerror(rc));
    return 1;
  }

  /*
   * Shutdown order is the correctness argument: both accepting threads are
   * joined first, so by the time admin_thread is told to tear down, nothing
   * can register a new worker or forward a new admin request underneath it.
   */
  pthread_join(listener, NULL);
  pthread_join(ctl, NULL);

  const char b = 'x';
  if (write(g_admin_quit[1], &b, 1) < 0)
    perror("ballotd: admin quit pipe");

  pthread_join(admin, NULL);
  bb_destroy(g_ctx);
  return 0;
}
