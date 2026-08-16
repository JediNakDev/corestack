#ifndef BALLOTCLIENT_INTERNAL_H
#define BALLOTCLIENT_INTERNAL_H

/*
 * internal.h - bcl_ctx's real layout, shared by client.c (owns log/
 * lifecycle) and transport.c (owns `transport`, an opaque pointer only it
 * interprets - a private struct holding the tetrissh session_t/fd). Keeping
 * this out of include/ is the same seam-isolation libtetrissh's own
 * common.h uses: the client's core logic never needs to know a session_t
 * exists, only that bcl_send/bcl_connect do.
 */

#include <stdio.h>

struct bcl_ctx {
  FILE *log;
  void (*before_submit)(void *arg);
  void *before_submit_arg;
  void *transport;  /* NULL until bcl_connect(); owned by transport.c.
                      * Voter ops (JOIN/CAST/UPDATE/RESULTS/CHECK) only. */
  char ctl_path[256]; /* '\0' until bcl_set_ctl_path(); admin ops
                        * (CREATE/OPEN/CLOSE/PUBLISH) only. A plain path
                        * string needs no opacity - unlike `transport`, it
                        * pulls in no libtetrissh types. */
};

#endif /* BALLOTCLIENT_INTERNAL_H */
