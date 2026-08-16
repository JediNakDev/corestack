#ifndef BALLOTD_CONTROL_PLANE_H
#define BALLOTD_CONTROL_PLANE_H

/*
 * control_plane.h - the admin channel: a local-only AF_UNIX socket for
 * ballotctl (CREATE/OPEN/CLOSE/PUBLISH), separate from the TCP+tetrissh
 * listener ballotu uses (JOIN/CAST/UPDATE/RESULTS/CHECK). Mirrors
 * tetrisctl/control_plane.h closely - same framing discipline, same
 * two-thread split of labor, same reasoning:
 *
 *   "The framing is a 4-byte big-endian length prefix around plaintext
 *   HTTTP - the same prefix discipline as libtetrissh traffic frames, minus
 *   the encryption. This channel is local-only by requirement, so there is
 *   nothing to encrypt against, and a handshake would only add a failure
 *   mode to the one command that has to work when everything else is
 *   broken."
 *
 * Unlike tetrisctl, this channel's verbs are already domain operations
 * (CREATE/OPEN/CLOSE/PUBLISH are bcl_op_t values), so there is no separate
 * verb enum or hand-built JSON here - ctl_thread decodes with exactly the
 * same libballotclient codec session.c uses on the voter channel, and
 * admin_thread replies with the same bcl_encode_response. Only the framing
 * (plaintext + length prefix vs. tetrissh-encrypted frames) differs.
 *
 * Two threads, same split as tetrisctl and for the same reason: ctl_thread
 * accepts, reads one frame, and parses it - every byte that came from
 * outside this process is handled here, where blocking is harmless. Reading
 * attacker-controlled bytes on admin_thread would stall every voter and
 * every other admin command behind one slow or hostile ballotctl.
 * admin_thread only ever sees an already-decoded, already-classified
 * request.
 *
 * On buffer sharing: ctl_thread's own malformed-input replies and
 * admin_thread's dispatched-request replies are two different functions
 * (ctl_reply_raw here vs. the reply path in main.c's admin_thread), each
 * using a stack-local buffer - never a shared static one. tetrisctl hit a
 * real data race from exactly that sharing (see its ctl_reply comment,
 * "a genuine data race between a client sending garbage and a client
 * sending a valid command at the same moment") and fixed it with a
 * _Thread_local buffer; this design avoids the class of bug instead of
 * reapplying that fix, by never having the two reply paths share storage in
 * the first place.
 */

#include "libballotclient/client.h"
#include "libballotclient/ctl_frame.h" /* CTL_SOCK_DEFAULT, CTL_MAX_FRAME,
                                        * ctl_frame_read/write - shared with
                                        * transport.c, see that header for why */

#include <limits.h>

/*
 * One admin request, already parsed and classified as CREATE/OPEN/CLOSE/
 * PUBLISH by ctl_thread. Handed to admin_thread over a pipe (ballotd's
 * g_ctl_notify, mirroring tetrisd's CtlReq/g_ctl_notify).
 *
 * `fd` is the accepted control connection; ownership transfers with the
 * struct - once written to the pipe, ctl_thread must not touch that fd
 * again, and admin_thread is responsible for replying and closing it.
 */
typedef struct {
  int fd;
  bcl_request_t req;
} BallotdCtlReq;

/* ---- daemon side (control_plane.c) ---------------------------------------
 *
 * Bind the control socket. Returns 0, or -1 with the reason already on
 * stderr. Call before any thread starts - failure here must stop the
 * daemon, not race a thread that is already running.
 */
int ctl_open(const char *path, int quit_rd, int quit_wr, int notify_wr);

/* pthread entry point. Runs until the quit pipe becomes readable. Accepts a
 * connection, reads one frame, classifies it, and either answers it
 * directly (malformed input, or an op outside CREATE/OPEN/CLOSE/PUBLISH) or
 * forwards it to admin_thread and returns immediately - it does not wait
 * for admin_thread's reply, so any number of ballotctl connections can be
 * in flight at once. */
void *ctl_thread(void *arg);

#endif /* BALLOTD_CONTROL_PLANE_H */
