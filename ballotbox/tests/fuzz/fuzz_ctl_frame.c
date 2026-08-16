/*
 * fuzz_ctl_frame.c - ctl_frame_read() on a hostile stream.
 *
 * ballotd's admin channel: a 4-byte big-endian length prefix, then that many
 * bytes. The length is the attacker's, the buffer is ours, and ctl_frame.h is
 * explicit that "local-only" is not the same as trusted ("the length is
 * attacker-supplied even on a local socket"). Anything that can open
 * var/run/ballotd.ctl can drive this function, and what it drives is a read
 * of an attacker-chosen size into a fixed buffer.
 *
 * Fed over a real socketpair rather than a fake fd, because the bugs in
 * length-prefixed framing are short-read bugs: a frame arriving in pieces, a
 * peer closing mid-payload, a prefix split across two segments. A buffer-based
 * fake would deliver every read whole and find none of them.
 *
 * The single check that matters: on success, *len bytes were actually written
 * and *len <= cap. A frame declaring 4 GiB into a 4 KiB buffer must come back
 * -1, and the malloc'd buffer's redzone is what proves it did.
 */

#include "libballotclient/ctl_frame.h"
#include "fuzz_support.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Smaller than CTL_MAX_FRAME on purpose: cap and the protocol maximum are
 * different numbers, and a bound check written against the wrong one only
 * shows up when they differ. */
#define FUZZ_CAP 4096

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  /* Bounded by the socket buffer: nothing drains the write end while we are
   * writing, so an oversized write would block forever rather than fail. */
  if (size > 8192) return 0;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;

  int bufsz = 64 * 1024;
  (void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
  (void)setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);

  if (size > 0) {
    ssize_t w = write(sv[0], data, size);
    if (w < 0 || (size_t)w != size) {
      close(sv[0]);
      close(sv[1]);
      return 0;
    }
  }
  /* Close the writer: the reader must terminate on EOF rather than block, and
   * a torn frame (-2 vs -1) is exactly what the close distinguishes. */
  close(sv[0]);

  uint8_t *buf = (uint8_t *)malloc(FUZZ_CAP);
  if (!buf) {
    close(sv[1]);
    return 0;
  }
  memset(buf, 0xC3, FUZZ_CAP); /* poison: only bytes the frame delivered may
                                * differ from this on success */

  uint32_t len = 0xFFFFFFFFu; /* poison: must be overwritten on success */
  int rc = ctl_frame_read(sv[1], buf, FUZZ_CAP, &len);

  FUZZ_CHECK(rc == 0 || rc == -1 || rc == -2);

  if (rc == 0) {
    /* The header's contract, and the only thing standing between a 4-byte
     * prefix and a heap overflow. */
    FUZZ_CHECK(len > 0 && len <= FUZZ_CAP);

    /* The payload must be the bytes that followed the prefix on the wire -
     * not a short read reported as a whole frame. */
    FUZZ_CHECK(size >= CTL_PREFIX_LEN + (size_t)len);
    FUZZ_CHECK(memcmp(buf, data + CTL_PREFIX_LEN, len) == 0);

    /* Nothing past *len was touched: a caller reading buf[len] must find its
     * own memory, not the tail of some earlier frame. */
    for (size_t i = len; i < FUZZ_CAP; i++) FUZZ_CHECK(buf[i] == 0xC3);
  } else {
    /* On failure the caller has no frame, so nothing may have been reported
     * as one. */
    FUZZ_CHECK(len == 0xFFFFFFFFu || len == 0);
  }

  /* -2 is documented as "the peer closed before sending anything", an
   * ordinary disconnect. Any byte on the wire means it was not that. */
  if (rc == -2) FUZZ_CHECK(size == 0);

  free(buf);
  close(sv[1]);
  return 0;
}
