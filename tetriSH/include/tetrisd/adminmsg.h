#ifndef TETRISD_ADMINMSG_H
#define TETRISD_ADMINMSG_H

/*
 * adminmsg.h - messages over the session <-> admin socketpair.
 *
 * Server-internal only (NOT libtetrisutil - the client never sees these). Same
 * host, same build, so a raw struct on the wire is fine.
 *
 * The socketpair is SOCK_STREAM, not SOCK_DGRAM. Datagram mode gave one
 * write() == one read() for free, but on macOS a datagram socketpair never
 * reports its peer closing to a poll() that is already blocked - so when a
 * session process exited, the admin thread never woke for it and never reaped
 * it: a leaked fd, a zombie, and a phantom member left in its room forever, so
 * any round that room was playing could no longer end. A stream socketpair
 * delivers POLLIN|POLLHUP and a read() of 0, which is exactly what the admin
 * loop already expects ("r == 0 -> session exited").
 *
 * A stream has no message boundaries, so every AdminMsg goes through the
 * adminmsg_write/adminmsg_read helpers below, which loop until the whole
 * fixed-size struct has moved. They live in this header because tetrisd and
 * the session binary are separate programs with no shared server-side object
 * file, and both ends must frame messages identically.
 *
 * NOTE: this message set is a design starting point - adjust it to match the
 * room rules you settle on.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdbool.h>
#include <errno.h>
#include <unistd.h>

#include "libtetrisutil/sessionstate.h"
#include "libtetrisbrain/gamestate.h" /* PlayerStanding / MAX_STANDINGS */

typedef enum
{
    /* --- session -> admin --- */
    ADMIN_JOIN = 0, /* want room `room_id` (0 = create new)         */
    ADMIN_LEAVE,    /* leave the current room                       */
    ADMIN_START,    /* owner starts the room                        */
    ADMIN_GARBAGE,  /* send `lines` garbage lines to the opponent   */
    ADMIN_GAMEOVER, /* this session topped out; `score` = final     */

    /* --- admin -> session --- */
    ADMIN_SESSION,      /* updated SessionState (phase / room / owner)  */
    ADMIN_SEED,         /* game start: use `seed` (also the start signal) */
    ADMIN_RECV_GARBAGE, /* receive `lines` garbage lines               */
    ADMIN_RESULT,       /* room finished: `winner` = winning player_id  */
    ADMIN_REJECT,       /* the last command was refused; `reason` = the
                         * htttp status the client should be told       */

    /* --- session -> admin --- */
    ADMIN_SCORE, /* my score/lines changed; `score` and `lines` are
                  * the new totals. Sent only when they actually
                  * change - both move solely on a line clear, so this
                  * is a few messages per round, not per tick.      */

    /* --- admin -> session --- */
    ADMIN_STANDINGS, /* the room's scoreboard: `standings` / `standing_count`.
                      * Broadcast whenever any member's score changes, or
                      * the membership does.                            */
} AdminMsgType;

/*
 * Why a command was refused. The values ARE htttp status codes, so the admin
 * thread names the outcome once and the session forwards it to the wire
 * unchanged - no second mapping table to drift out of sync.
 *
 * Restricted to codes htttp_reason() already knows: htttp_serialize_response
 * refuses to emit a status it has no phrase for, so an unlisted code (503,
 * say) would be silently dropped instead of reaching the client.
 */
#define REJECT_NOT_OWNER 403 /* Forbidden - only the room owner may START */
#define REJECT_CONFLICT 409  /* Conflict - already in a room, or in none  */
#define REJECT_FULL 429      /* Too Many Requests - room or table is full */

typedef struct
{
    AdminMsgType type;
    int room_id;                /* ADMIN_JOIN                          */
    char name[MAX_USER_NAME]; /* ADMIN_JOIN: logged-in display name, or ""
                                 * for a guest (libtetrisauth's auth_get_name) */
    int lines;                  /* ADMIN_GARBAGE / ADMIN_RECV_GARBAGE  */
    unsigned seed;              /* ADMIN_SEED                          */
    int score;            /* ADMIN_GAMEOVER: reporting player's final score */
    int winner;           /* ADMIN_RESULT: winning player_id     */
    int reason;           /* ADMIN_REJECT: one of REJECT_* above  */
    SessionState session; /* ADMIN_SESSION                       */
    /* ADMIN_SESSION: this message is the answer to a JOIN that CREATED the
     * room, rather than joining an existing one. Only the admin thread can
     * tell the two apart - the session forwards a JOIN and is done with it -
     * so the distinction has to travel back, and it is what makes 201
     * reachable instead of 200. False on every other ADMIN_SESSION. */
    bool created;

    /*
     * ADMIN_STANDINGS payload. This is what makes an AdminMsg large - the
     * array costs its full size on every message, including a bare JOIN that
     * has no use for it.
     *
     * That waste buys the property adminmsg_read depends on: a fixed-size
     * record, framed by a single sizeof, with no length header to parse and no
     * way for the two programs to disagree about where a message ends. A
     * variable-length payload would mean writing that framing by hand on both
     * sides of a socketpair shared by two separate binaries. At a few hundred
     * bytes over a local socket, the simpler framing is worth more than the
     * bytes.
     */
    PlayerStanding standings[MAX_STANDINGS];
    int standing_count;
} AdminMsg;

/*
 * Write one whole AdminMsg. Returns 0 on success, -1 if the peer is gone.
 *
 * A stream socket may accept a partial struct when its buffer is nearly full,
 * so a bare write() could tear a message in half and desynchronise the reader
 * for the rest of the session's life. Loop until all of it is out.
 */
/* The fd is non-blocking and the peer is not draining it. */
#define ADMINMSG_WOULDBLOCK (-2)

/*
 * Write one whole message. 0 = sent, -1 = peer gone, ADMINMSG_WOULDBLOCK = the
 * peer is not draining (only reachable on the admin's non-blocking ends).
 *
 * A caller seeing ADMINMSG_WOULDBLOCK must CLOSE, not skip the message: some
 * bytes of the record may already be in the pipe, and abandoning the rest
 * leaves the reader mid-struct and the stream silently desynced.
 */
static inline int adminmsg_write(int fd, const AdminMsg *m)
{
    const unsigned char *p = (const unsigned char *)m;
    size_t left = sizeof *m;

    while (left > 0)
    {
        ssize_t n = write(fd, p, left);
        if (n > 0)
        {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return ADMINMSG_WOULDBLOCK;
        }
        return -1; /* peer closed, or an error we cannot resume from */
    }
    return 0;
}

/*
 * Read one whole AdminMsg.
 *
 * Returns 1 on success, 0 when the peer closed cleanly (the session exited),
 * and -1 on error. The caller must treat both 0 and -1 as "this session is
 * gone" - that is the signal a datagram socketpair could not deliver.
 */
static inline int adminmsg_read(int fd, AdminMsg *m)
{
    unsigned char *p = (unsigned char *)m;
    size_t left = sizeof *m;

    while (left > 0)
    {
        ssize_t n = read(fd, p, left);
        if (n > 0)
        {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n == 0)
            return left == sizeof *m ? 0 : -1; /* clean EOF vs torn message */
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 1;
}

#endif /* TETRISD_ADMINMSG_H */
