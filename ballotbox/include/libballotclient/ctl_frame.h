#ifndef BALLOTCLIENT_CTL_FRAME_H
#define BALLOTCLIENT_CTL_FRAME_H

/*
 * ctl_frame.h - wire framing for ballotd's admin channel (a local-only
 * AF_UNIX socket carrying CREATE/OPEN/CLOSE/PUBLISH). A 4-byte big-endian
 * length prefix around plaintext HTTTP - the same prefix discipline as
 * libtetrissh traffic frames, minus the encryption, since this channel is
 * local-only by requirement (see include/ballotd/control_plane.h for the
 * full rationale).
 *
 * Lives here, not under include/ballotd/, because both ends of this channel
 * need it: ballotd's ctl_thread (control_plane.c) AND libballotclient's
 * bcl_send (transport.c), which dials this same socket for admin ops from
 * ballotctl. Same reasoning tetriSH's tetrisctl/control_plane.h gives for
 * putting its own shared framing in the CLIENT's namespace even though the
 * daemon side implements against it too: "both ends must agree exactly or
 * it fails in ways neither side can see."
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* Default socket path, used when no ballotd_ctl_ipc rc directive or -C flag
 * names one. Relative to ballotd's cwd (it is not daemonised/chdir'd in
 * this pass, unlike tetrisd's dspawn-launched deployment). */
#define CTL_SOCK_DEFAULT "var/run/ballotd.ctl"

/* One control message must fit one frame, matching HTTTP_MAX_FRAME. */
#define CTL_MAX_FRAME 65536

/* Length prefix width. Big-endian, so the wire is readable in a hex dump. */
#define CTL_PREFIX_LEN 4

/* Loop until `len` bytes have moved. Returns 0, or -1 if the peer is gone. */
static inline int ctl_write_all(int fd, const uint8_t *p, size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

/* Mirror of ctl_write_all. Returns 0, -1 on error, and -2 on a short read,
 * which the caller must treat as a truncated frame rather than an empty
 * one. */
static inline int ctl_read_all(int fd, uint8_t *p, size_t len) {
  size_t want = len;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n == 0) return len == want ? -2 : -1; /* clean EOF vs torn frame */
    if (errno == EINTR) continue;
    return -1;
  }
  return 0;
}

/* Send one frame: 4-byte big-endian length, then the payload. */
static inline int ctl_frame_write(int fd, const uint8_t *buf, uint32_t len) {
  uint8_t prefix[CTL_PREFIX_LEN] = {
      (uint8_t)(len >> 24),
      (uint8_t)(len >> 16),
      (uint8_t)(len >> 8),
      (uint8_t)len,
  };

  if (ctl_write_all(fd, prefix, sizeof prefix) != 0) return -1;
  return ctl_write_all(fd, buf, len);
}

/*
 * Receive one frame into buf. On success *len is the payload size.
 * Returns 0, -1 on error or a malformed/oversized length, and -2 when the
 * peer closed before sending anything - an ordinary disconnect, not a
 * fault.
 */
static inline int ctl_frame_read(int fd, uint8_t *buf, uint32_t cap, uint32_t *len) {
  uint8_t prefix[CTL_PREFIX_LEN];
  int rc = ctl_read_all(fd, prefix, sizeof prefix);
  if (rc != 0) return rc;

  uint32_t n = ((uint32_t)prefix[0] << 24) | ((uint32_t)prefix[1] << 16) |
               ((uint32_t)prefix[2] << 8) | (uint32_t)prefix[3];

  /* Bound before paying it any attention: the length is attacker-supplied
   * even on a local socket, and 0 would leave the caller parsing stale
   * bytes. */
  if (n == 0 || n > cap) return -1;

  if (ctl_read_all(fd, buf, n) != 0) return -1;

  *len = n;
  return 0;
}

#endif /* BALLOTCLIENT_CTL_FRAME_H */
