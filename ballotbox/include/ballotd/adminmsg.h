#ifndef BALLOTD_ADMINMSG_H
#define BALLOTD_ADMINMSG_H

/*
 * adminmsg.h - messages over the ballot_session <-> admin_thread socketpair
 * (Channel A's internal hop: one TCP voter connection <-> the daemon's one
 * bb_ctx). Mirrors tetriSH's tetrisd/adminmsg.h: same host, same build, so a
 * raw struct is fine on this wire - the client-facing wire (HTTTP, over
 * tetrissh) is not, see libballotclient/codec.h for why.
 *
 * Unlike AdminMsg, this carries exactly one message shape per direction.
 * BallotBox has no unsolicited server push (no UPD_GAME-equivalent) and no
 * per-client clock, so the exchange is strictly one request then one reply -
 * bcl_request_t.op is already the type tag, nothing else is multiplexed
 * over this pair.
 *
 * SOCK_STREAM, not SOCK_DGRAM, for the same reason as tetrisd/adminmsg.h: a
 * stream reliably delivers POLLHUP and a 0-byte read when the peer closes,
 * which a datagram socketpair does not on every platform.
 */

#include "libballotclient/client.h"

#include <errno.h>
#include <unistd.h>

typedef struct {
  bcl_request_t req;
} BallotdReq;

typedef struct {
  bcl_response_t resp;
} BallotdResp;

/*
 * Loop until exactly `len` bytes have moved. Returns 0 on success, -1 if the
 * peer is gone or on an unresumable error.
 */
static inline int ballotmsg_write_all(int fd, const void *buf, size_t len) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t left = len;

  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n > 0) {
      p += n;
      left -= (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return -1; /* peer closed, or an error we cannot resume from */
  }
  return 0;
}

/*
 * Mirror of ballotmsg_write_all. Returns 1 on a full read, 0 on a clean EOF
 * before any byte arrived (the ordinary "peer is gone between messages"
 * case), -1 on a torn message or another error - both must be treated as
 * "this side of the pipe is dead", the same as tetrisd/adminmsg.h's
 * adminmsg_read.
 */
static inline int ballotmsg_read_all(int fd, void *buf, size_t len) {
  unsigned char *p = (unsigned char *)buf;
  size_t left = len;

  while (left > 0) {
    ssize_t n = read(fd, p, left);
    if (n > 0) {
      p += n;
      left -= (size_t)n;
      continue;
    }
    if (n == 0) return left == len ? 0 : -1; /* clean EOF vs torn message */
    if (errno == EINTR) continue;
    return -1;
  }
  return 1;
}

static inline int ballotmsg_write_req(int fd, const BallotdReq *m) {
  return ballotmsg_write_all(fd, m, sizeof *m);
}
static inline int ballotmsg_read_req(int fd, BallotdReq *m) {
  return ballotmsg_read_all(fd, m, sizeof *m);
}
static inline int ballotmsg_write_resp(int fd, const BallotdResp *m) {
  return ballotmsg_write_all(fd, m, sizeof *m);
}
static inline int ballotmsg_read_resp(int fd, BallotdResp *m) {
  return ballotmsg_read_all(fd, m, sizeof *m);
}

#endif /* BALLOTD_ADMINMSG_H */
