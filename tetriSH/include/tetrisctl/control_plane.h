#ifndef TETRISCTL_CONTROL_PLANE_H
#define TETRISCTL_CONTROL_PLANE_H

/*
 * control_plane.h - the admin channel between bin/tetrisd and bin/tetrisctl.
 *
 * Named for the handout's own term: REQUIREMENTS.md has a section called "The
 * control plane", and using the same words here means a question about the
 * spec and a question about this code have the same answer.
 *
 * Both ends of that channel are declared in one header because both ends must
 * agree exactly or it fails in ways neither side can see. Same reasoning as
 * adminmsg.h: tetrisd and tetrisctl are separate programs with no shared
 * object file, so anything they must agree on lives in a header, and the
 * framing helpers are static inline rather than a library nobody else links.
 *
 * Two halves, in two source files, because they land in two binaries:
 *
 *   tetrisctl.c      the CLI                       -> bin/tetrisctl
 *   control_plane.c  the thread inside the daemon  -> bin/tetrisd
 *
 * Merging those would put main() into tetrisd and drag the room tables into
 * the CLI. The split is the linker's, not a matter of taste.
 *
 * What is shared: the wire format, and where the socket is. The framing is a
 * 4-byte big-endian length prefix around plaintext HTTTP - the same prefix
 * discipline as libtetrissh traffic frames, minus the encryption. This channel
 * is local-only by requirement, so there is nothing to encrypt against, and a
 * handshake would only add a failure mode to the one command that has to work
 * when everything else is broken.
 *
 * CtlReq is NOT on the wire. It is the admin thread's view of an already
 * parsed request, handed over the g_ctl_notify pipe by the control thread.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* getenv */
#include <string.h>
#include <unistd.h>

#include "libtetrisutil/rc.h"

/*
 * Default socket path, used when .tetrishrc names no ctl_ipc.
 *
 * Relative on purpose: it hangs off the project root both ends are started
 * from, so a checkout anywhere works with no edit, and two checkouts on one
 * machine do not fight over a single absolute path.
 */
#define CTL_SOCK_DEFAULT "var/run/tetrisd.ctl"

/* One control message must fit one frame, matching HTTTP_MAX_FRAME. */
#define CTL_MAX_FRAME 65536

/* Length prefix width. Big-endian, so the wire is readable in a hex dump. */
#define CTL_PREFIX_LEN 4

/*
 * One admin command, already parsed.
 *
 * Fixed size and POD, so the ctl thread can hand it to the admin thread with a
 * bare write() to a pipe and the admin thread can read it with the same bare
 * read() it already uses for NewSession. No framing to get wrong.
 *
 * `fd` is the accepted control connection. Ownership transfers with the
 * struct: once written to the pipe, the ctl thread must not touch that fd
 * again, and the admin thread is responsible for replying and closing it.
 */
typedef enum
{
    CTL_VERB_STATUS = 0,
    CTL_VERB_SHUTDOWN,
    CTL_VERB_ROOMS,
    CTL_VERB_PLAYERS,
    CTL_VERB_KICK,
    CTL_VERB_RELOAD, /* raises SIGHUP in tetrisd; see ctl_dispatch */
    CTL_VERB_BAD,    /* parsed fine, but not a verb we serve -> 400  */
} CtlVerb;

typedef struct
{
    int fd;     /* accepted control connection; admin owns it now */
    int verb;   /* CtlVerb                                        */
    int room;   /* CTL_VERB_KICK: target room id                  */
    int player; /* CTL_VERB_KICK: target player_id within room    */
} CtlReq;

/* ---- path resolution ---------------------------------------------------- */

/*
 * Work out the control socket path, identically on both ends.
 *
 * ctl_ipc from .tetrishrc overrides CTL_SOCK_DEFAULT, and both are used as
 * written - relative to the project root, which is where both ends of the
 * control plane are started from.
 */
static inline void ctl_socket_path(char *out, size_t out_len)
{
    (void)rc_get("ctl_ipc", CTL_SOCK_DEFAULT, out, out_len);
}

/* ---- framing ------------------------------------------------------------ */

/* Loop until `len` bytes have moved. Returns 0, or -1 if the peer is gone. */
static inline int ctl_write_all(int fd, const uint8_t *p, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        if (n > 0)
        {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* Mirror of ctl_write_all. Returns 0, -1 on error, and -2 on a short read,
 * which the caller must treat as a truncated frame rather than an empty one. */
static inline int ctl_read_all(int fd, uint8_t *p, size_t len)
{
    size_t want = len;
    while (len > 0)
    {
        ssize_t n = read(fd, p, len);
        if (n > 0)
        {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n == 0)
        {
            int status = len == want ? -2 : -1;
            return status; /* clean EOF vs torn frame */
        }
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* Send one frame: 4-byte big-endian length, then the payload. */
static inline int ctl_frame_write(int fd, const uint8_t *buf, uint32_t len)
{
    uint8_t prefix[CTL_PREFIX_LEN] = {
        (uint8_t)(len >> 24),
        (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),
        (uint8_t)len,
    };

    if (ctl_write_all(fd, prefix, sizeof prefix) != 0)
    {
        return -1;
    }
    int status = ctl_write_all(fd, buf, len);
    return status;
}

/*
 * Receive one frame into buf. On success *len is the payload size.
 *
 * Returns 0, -1 on error or a malformed/oversized length, and -2 when the peer
 * closed before sending anything - an ordinary disconnect, not a fault.
 */
static inline int ctl_frame_read(int fd, uint8_t *buf, uint32_t cap,
                                 uint32_t *len)
{
    uint8_t prefix[CTL_PREFIX_LEN];
    int rc = ctl_read_all(fd, prefix, sizeof prefix);
    if (rc != 0)
    {
        return rc;
    }

    uint32_t n = ((uint32_t)prefix[0] << 24) | ((uint32_t)prefix[1] << 16) |
                 ((uint32_t)prefix[2] << 8) | (uint32_t)prefix[3];

    /* Bound before allocating attention to it: the length is attacker-supplied
     * even on a local socket, and 0 would leave the caller parsing stale bytes.
     */
    if (n == 0 || n > cap)
    {
        return -1;
    }

    if (ctl_read_all(fd, buf, n) != 0)
    {
        return -1;
    }

    *len = n;
    return 0;
}

/* ---- daemon side (control_plane.c, linked into bin/tetrisd) -------------
 *
 * Declared here rather than in a header of their own so there is exactly one
 * place to look for "how does the control plane work". The CLI sees these
 * prototypes and cannot call them - they live in the other binary - which is
 * itself a useful statement of where the boundary runs.
 *
 * The split of work between the two threads is the whole design:
 *
 *   control thread  accepts, reads frames, parses HTTTP. Every byte that came
 *                   from outside this process is handled here, on a thread
 *                   whose only job is that. Blocking here is harmless.
 *
 *   admin thread    receives an already-parsed CtlReq over a pipe, reads the
 *                   room tables it exclusively owns, and writes the reply.
 *
 * Reading on the admin thread instead would put a blocking read of
 * attacker-controlled length in the middle of the loop that routes every game
 * message - one stalled tetrisctl would stall every room. That is precisely
 * the failure "the control plane must remain available even when the public
 * TCP listener is saturated" asks us to design against, and it would be
 * embarrassing to reintroduce it on the control plane itself.
 */

/*
 * What the admin thread must do after ctl_dispatch has already replied.
 *
 * Returned rather than done inside ctl_dispatch because these two actions
 * touch the admin thread's poll set and the daemon's lifecycle, neither of
 * which this module owns. ctl.c owns the wire and the JSON; tetrisd.c owns
 * process state. Keeping that line clean is what stops the poll set from
 * acquiring a second writer.
 */
typedef enum
{
    CTL_AFTER_NONE = 0,
    CTL_AFTER_SHUTDOWN, /* reply is out; begin graceful teardown */
    CTL_AFTER_KICK,     /* victim fd returned via kick_fd; drop it */
} CtlAfter;

/*
 * Bind the control socket and hand this module everything it needs. Returns 0,
 * or -1 with the reason already on stderr.
 *
 * One call rather than open-then-configure: the socket path was previously
 * stored by both halves, and two writers of one path is a bug waiting for the
 * day they disagree and the unlink targets the wrong file.
 *
 * Call before any thread starts - it also records the daemon's start time,
 * which STATUS reports as uptime. `quit_wr` is the write end of the same pipe
 * as `quit_rd`: the thread waits on one and, if it ever dies of anything other
 * than that, signals the other so the daemon does not outlive its own control
 * channel.
 */
int ctl_open(const char *path, int quit_rd, int quit_wr, int notify_wr);

/* pthread entry point. Runs until the quit pipe becomes readable. */
void *ctl_thread(void *arg);

/*
 * Admin thread: answer one parsed request and close its connection.
 *
 * Always consumes req->fd, including on error paths, so the caller never has
 * to reason about whether a reply happened.
 */
CtlAfter ctl_dispatch(const CtlReq *req, int *kick_fd);

#endif /* TETRISCTL_CONTROL_PLANE_H */
