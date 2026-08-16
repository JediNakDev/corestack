#ifndef TETRISCTL_CTL_CLIENT_H
#define TETRISCTL_CTL_CLIENT_H

/*
 * ctl_client.h - the client half of the control plane.
 *
 * One request in, one decoded snapshot out. Split from tetrisctl.c so the CLI
 * and the TUI render from the same structs and cannot disagree, and so a
 * has exactly one place to substitute itself.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "libtetrisutil/limits.h"
#include "tetrisctl/control_plane.h"

/* The CLI can afford to wait for a wedged daemon; the TUI redraws every second
 * and must not freeze for longer than one tick. */
#define CTL_TIMEOUT_CLI_MS 5000
#define CTL_TIMEOUT_TUI_MS 800

/* Reply bodies are built into a CTL_MAX_FRAME/2 buffer on the daemon side. */
#define CTL_BODY_MAX (CTL_MAX_FRAME / 2)

/* Negative, so one return value carries both an HTTTP status and a failure. */
enum
{
    CTL_ERR_CONNECT = -1, /* no socket there, or nobody listening */
    CTL_ERR_IO = -2,      /* connected, then the exchange broke   */
    CTL_ERR_TIMEOUT = -3, /* accepted, then went silent           */
    CTL_ERR_PROTO = -4,   /* answered with something unparseable  */
};

typedef struct
{
    long uptime;
    int sessions;
    int rooms;
} CtlStatus;

typedef struct
{
    int id;
    int members;
    int owner; /* < 0: nobody owns it */
    char phase[16];
} CtlRoom;

typedef struct
{
    int room; /* < 0: in the lobby, not in a room */
    int player;
    int pid;
    int score;
    int lines;
    bool is_owner;
    char name[MAX_USER_NAME];
} CtlPlayer;

/*
 * Everything the dashboard shows, held between ticks.
 *
 * Kept across a failed refresh so the screen can grey the last good data
 * rather than blank; `stale` is what the banner reads.
 */
typedef struct
{
    CtlStatus status;
    CtlRoom rooms[MAX_ROOMS];
    CtlPlayer players[MAX_SESSIONS];
    int n_rooms;
    int n_players;
    bool have;  /* ever successfully populated */
    bool stale; /* the most recent refresh failed */
    time_t taken;
} CtlSnapshot;

/*
 * Send one verb, read one reply. Returns the HTTTP status, or a CTL_ERR_*.
 *
 * `body` comes back NUL-terminated; the wire body is not. Pass NULL to discard
 * it. One connection per call, because ctl_dispatch closes every fd it is
 * handed - there is no session to keep open.
 */
int ctl_request(const char *sock, const char *method, const char *path,
                int timeout_ms, char *body, size_t body_cap);

/*
 * Decoders. Return 0 (status) or an element count, or -1 if the body is not
 * what the verb promised.
 *
 * A missing key defaults rather than fails, so a daemon that grows a field
 * does not break an older client. --json always shows the untouched body, so
 * a format change degrades the output instead of hiding it.
 */
int ctl_decode_status(const char *body, CtlStatus *out);
int ctl_decode_rooms(const char *body, CtlRoom *out, int cap);
int ctl_decode_players(const char *body, CtlPlayer *out, int cap);

/*
 * STATUS, ROOMS and PLAYERS into one snapshot. Returns 0, or the first
 * CTL_ERR_*; on failure the previous contents survive and `stale` is set.
 */
int ctl_refresh(const char *sock, int timeout_ms, CtlSnapshot *snap);

/* Human text for a CTL_ERR_*, or for a non-2xx status. */
const char *ctl_strerror(int rc);

/* "42s" / "4m12s" / "1h07m". Shared so the CLI and the dashboard agree. */
void ctl_fmt_uptime(char *out, size_t cap, long secs);

#endif /* TETRISCTL_CTL_CLIENT_H */
