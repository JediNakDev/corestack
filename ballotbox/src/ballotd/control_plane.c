/*
 * control_plane.c - the admin channel: AF_UNIX listener, HTTTP parsing (via
 * the shared codec), CREATE/OPEN/CLOSE/PUBLISH only. See control_plane.h.
 */

#include "ballotd/control_plane.h"

#include "libballotclient/codec.h"
#include "libhtttp/htttp.h"

#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* How long to wait on a control client that has connected but gone quiet.
 * Only blocks other admin commands, never a voter, but not unbounded, or
 * one wedged ballotctl locks the whole admin channel. */
#define CTL_IO_TIMEOUT_MS 2000

#define CTL_LISTEN_BACKLOG 8

/* Set once by ctl_open, read only by ctl_thread from then on. */
static int g_lfd = -1;
static int g_quit_rd = -1;
static int g_quit_wr = -1;
static int g_notify_wr = -1;
static char g_sock_path[PATH_MAX];

static void set_timeout(int fd, int opt, int ms) {
  struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
}

/*
 * Answer a connection directly, with no body: malformed input, an op
 * outside CREATE/OPEN/CLOSE/PUBLISH, or "the admin channel is too busy to
 * accept this". None of these need bb_ctx, so answering here keeps them off
 * admin_thread entirely - the same reasoning tetrisctl's ctl_thread uses to
 * answer a parse failure itself rather than forwarding it.
 *
 * Stack-local buffer, called only from ctl_thread: no sharing with
 * admin_thread's own reply path (see control_plane.h's header comment on
 * why that avoids the data race tetrisctl's control_plane.c once had).
 */
static void ctl_reply_raw(int fd, int status) {
  htttp_response_t res;
  memset(&res, 0, sizeof res);
  res.status = status;

  uint8_t out[512];
  uint32_t len = sizeof out;
  if (htttp_serialize_response(&res, out, &len) == HTTTP_OK) ctl_frame_write(fd, out, len);

  close(fd);
}

/*
 * One accepted connection: read its frame, decode it, classify it, and
 * either answer it here or forward it. Always consumes cfd - either by
 * closing it (answered here) or by handing it to admin_thread (forwarded).
 */
static void handle_conn(int cfd) {
  set_timeout(cfd, SO_RCVTIMEO, CTL_IO_TIMEOUT_MS);
  set_timeout(cfd, SO_SNDTIMEO, CTL_IO_TIMEOUT_MS);

  uint8_t frame[CTL_MAX_FRAME];
  uint32_t len = 0;
  if (ctl_frame_read(cfd, frame, sizeof frame, &len) != 0) {
    close(cfd); /* truncated, oversized, or just hung up */
    return;
  }

  htttp_request_t http;
  if (htttp_parse_request(frame, len, &http) != HTTTP_OK) {
    ctl_reply_raw(cfd, 400);
    return;
  }

  BallotdCtlReq cr;
  cr.fd = cfd;
  if (bcl_decode_request(&http, &cr.req) != 0) {
    ctl_reply_raw(cfd, 400);
    return;
  }

  /* Only the admin ops belong on this channel; JOIN/CAST/UPDATE and the
   * voter-gated RESULTS must come in over the voter TCP+tetrissh listener
   * instead. ADMIN_RESULTS is this channel's own results read
   * (bb_get_results_admin - no eligible-list check), a distinct op from
   * RESULTS precisely so that gate can never be bypassed by sending the
   * voter op here instead. ADMIN_CHECK is the same reasoning for CHECK on
   * paper, but bb_lookup_hash (both ops call the same function - see
   * dispatch.c) has no gate to bypass in the first place; it stays a
   * separate op anyway because ballotctl has no voter session to route
   * plain CHECK through at all, only ever the admin socket. This is the
   * actual enforcement of "only ballotctl manages elections" - the channel
   * a request arrived on decides what it is even allowed to ask. */
  if (cr.req.op != BCL_CREATE && cr.req.op != BCL_OPEN && cr.req.op != BCL_CLOSE &&
      cr.req.op != BCL_PUBLISH && cr.req.op != BCL_ADMIN_RESULTS &&
      cr.req.op != BCL_ADMIN_CHECK && cr.req.op != BCL_ADMIN_NEXT_ID) {
    ctl_reply_raw(cfd, 400);
    return;
  }

  /*
   * Ownership of cfd transfers with this write. After it succeeds,
   * admin_thread will dispatch, reply, and close it - touching cfd here
   * afterwards would race that.
   */
  if (write(g_notify_wr, &cr, sizeof cr) != (ssize_t)sizeof cr) {
    perror("ballotd: ctl notify");
    ctl_reply_raw(cfd, 500);
  }
}

int ctl_open(const char *path, int quit_rd, int quit_wr, int notify_wr) {
  snprintf(g_sock_path, sizeof g_sock_path, "%s", path);
  g_quit_rd = quit_rd;
  g_quit_wr = quit_wr;
  g_notify_wr = notify_wr;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;

  if (strlen(path) >= sizeof addr.sun_path) {
    fprintf(stderr, "ballotd: control socket path too long: %s\n", path);
    return -1;
  }
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

  /*
   * A socket file on disk means one of two things, and lstat cannot tell
   * them apart: a daemon died without cleaning up, or a daemon is running
   * right now and this is its front door. Ask instead of assume: if
   * connect() succeeds, somebody is serving; ECONNREFUSED means the socket
   * is stale and ours to clear. Same reasoning as tetrisd's ctl_open.
   */
  struct stat st;
  if (lstat(path, &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      fprintf(stderr, "ballotd: refusing to replace non-socket %s\n", path);
      return -1;
    }

    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
      int live = connect(probe, (struct sockaddr *)&addr, sizeof addr);
      close(probe);
      if (live == 0) {
        fprintf(stderr, "ballotd: %s is already served by a running ballotd\n", path);
        return -1;
      }
    }
    unlink(path);
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("ballotd: ctl socket");
    return -1;
  }

  /* bind() applies the umask to the socket file it creates; narrow it
   * around the bind rather than chmod()ing afterwards, which would leave a
   * window with a too-permissive mode already on disk. */
  mode_t old_umask = umask(077);
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
  umask(old_umask);

  if (rc < 0) {
    perror("ballotd: ctl bind");
    close(fd);
    return -1;
  }

  /* Belt and braces: some platforms ignore the umask for socket files. */
  if (chmod(path, 0600) < 0) perror("ballotd: ctl chmod");

  if (listen(fd, CTL_LISTEN_BACKLOG) < 0) {
    perror("ballotd: ctl listen");
    close(fd);
    unlink(path);
    return -1;
  }

  g_lfd = fd;
  return 0;
}

void *ctl_thread(void *arg) {
  (void)arg;

  struct pollfd p[2];
  p[0].fd = g_quit_rd;
  p[0].events = POLLIN;
  p[1].fd = g_lfd;
  p[1].events = POLLIN;

  bool asked_to_stop = false;

  for (;;) {
    p[0].revents = p[1].revents = 0;

    if (poll(p, 2, -1) < 0) {
      if (errno == EINTR) continue;
      perror("ballotd: ctl poll");
      break;
    }

    if (p[0].revents & POLLIN) {
      asked_to_stop = true;
      break; /* checked first: quit wins */
    }

    if (!(p[1].revents & POLLIN)) continue;

    int cfd = accept(g_lfd, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      perror("ballotd: ctl accept");
      break;
    }

    handle_conn(cfd);
  }

  close(g_lfd);
  g_lfd = -1;
  unlink(g_sock_path);

  /* If we got here on an error rather than the quit pipe, the daemon has
   * just lost its only admin channel while still running - take it down
   * rather than fail open, same as tetrisd's ctl_thread. */
  if (!asked_to_stop) {
    fprintf(stderr, "ballotd: control plane failed, shutting down\n");
    const char b = 'x';
    ssize_t n = write(g_quit_wr, &b, 1);
    (void)n;
  }
  return NULL;
}
