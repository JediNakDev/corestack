/*
 * ctl_client.c - transport and decode for the control plane's client half.
 *
 * The JSON reader is deliberately small: it reads bodies this project's own
 * daemon produces, which are flat and escaped (control_plane.c json_escape).
 * A real parser would be more code than every caller combined.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "libhtttp/htttp.h"
#include "libtetrisutil/logmsg.h"
#include "tetrisctl/ctl_client.h"

/* ---- tiny JSON reader ---------------------------------------------------- */

/* Find "key": and return the character after the colon, or NULL. */
static const char *find_key(const char *json, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? p + strlen(pat) : NULL;
}

static long json_int(const char *json, const char *key, long fallback)
{
    const char *p = find_key(json, key);
    return p ? strtol(p, NULL, 10) : fallback;
}

/* Copy a string value into out. Returns false if the key is missing. */
static bool json_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = find_key(json, key);
    if (!p)
        return false;

    while (*p == ' ')
        p++;
    if (*p != '"')
        return false;
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap)
    {
        if (*p == '\\' && p[1])
            p++; /* keep escaped chars literal */
        out[o++] = *p++;
    }
    out[o] = '\0';
    return true;
}

static bool json_bool(const char *json, const char *key)
{
    const char *p = find_key(json, key);
    if (!p)
        return false;
    while (*p == ' ')
        p++;
    return strncmp(p, "true", 4) == 0;
}

/*
 * Walk a top-level array, handing each element to `fn`.
 *
 * Brace-depth counted rather than split on commas: a player name containing a
 * comma would otherwise cut a row in half.
 */
static void json_each(const char *json, void (*fn)(const char *, int, void *),
                      void *ctx, int *count)
{
    const char *p = strchr(json, '[');
    if (!p)
        return;

    int depth = 0;
    const char *start = NULL;

    for (; *p; p++)
    {
        if (*p == '{')
        {
            if (depth++ == 0)
                start = p;
        }
        else if (*p == '}')
        {
            if (--depth == 0 && start)
            {
                char item[1024];
                size_t len = (size_t)(p - start) + 1;
                if (len >= sizeof item)
                    len = sizeof item - 1;
                memcpy(item, start, len);
                item[len] = '\0';
                fn(item, (*count)++, ctx);
                start = NULL;
            }
        }
        else if (*p == ']' && depth == 0)
        {
            break;
        }
    }
}

/* ---- transport ----------------------------------------------------------- */

static int ctl_connect(const char *path, int timeout_ms)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    struct timeval tv = {.tv_sec = timeout_ms / 1000,
                         .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int ctl_request(const char *sock, const char *method, const char *path,
                int timeout_ms, char *body, size_t body_cap)
{
    if (body != NULL && body_cap > 0)
        body[0] = '\0';

    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.path, sizeof req.path, "%s", path ? path : "/");

    static uint8_t frame[CTL_MAX_FRAME];
    uint32_t len = sizeof frame;
    if (htttp_serialize_request(&req, frame, &len) != HTTTP_OK)
    {
        log_send(LOG_ERROR,
                 "operation=ctl_request phase=complete method=%s status=%d",
                 method, CTL_ERR_PROTO);
        return CTL_ERR_PROTO;
    }

    int fd = ctl_connect(sock, timeout_ms);
    if (fd < 0)
    {
        log_send(LOG_ERROR,
                 "operation=ctl_request phase=complete method=%s status=%d",
                 method, CTL_ERR_CONNECT);
        return CTL_ERR_CONNECT;
    }

    if (ctl_frame_write(fd, frame, len) != 0)
    {
        close(fd);
        log_send(LOG_ERROR,
                 "operation=ctl_request phase=complete method=%s status=%d",
                 method, CTL_ERR_IO);
        return CTL_ERR_IO;
    }

    uint32_t rlen = 0;
    if (ctl_frame_read(fd, frame, sizeof frame, &rlen) != 0)
    {
        int e = errno;
        close(fd);
        /* Accepted, then silent: the one failure a wedged daemon produces and
         * the reason the TUI passes a shorter timeout than the CLI. */
        int rc =
            (e == EAGAIN || e == EWOULDBLOCK) ? CTL_ERR_TIMEOUT : CTL_ERR_IO;
        log_send(LOG_ERROR,
                 "operation=ctl_request phase=complete method=%s status=%d",
                 method, rc);
        return rc;
    }
    close(fd);

    htttp_response_t res;
    if (htttp_parse_response(frame, rlen, &res) != HTTTP_OK)
    {
        log_send(LOG_ERROR,
                 "operation=ctl_request phase=complete method=%s status=%d",
                 method, CTL_ERR_PROTO);
        return CTL_ERR_PROTO;
    }

    if (body != NULL && body_cap > 0)
    {
        uint32_t n =
            res.body_len < body_cap - 1 ? res.body_len : (uint32_t)body_cap - 1;
        if (res.body != NULL && n > 0)
            memcpy(body, res.body, n);
        body[n] = '\0';
    }
    log_level_t level = res.status >= 500   ? LOG_ERROR
                        : res.status >= 400 ? LOG_WARN
                                            : LOG_INFO;
    log_send(level, "operation=ctl_request phase=complete method=%s status=%d",
             method, res.status);
    return res.status;
}

const char *ctl_strerror(int rc)
{
    switch (rc)
    {
    case CTL_ERR_CONNECT:
        return "cannot reach tetrisd (is it running?)";
    case CTL_ERR_IO:
        return "connection to tetrisd broke mid-exchange";
    case CTL_ERR_TIMEOUT:
        return "tetrisd accepted but did not answer in time";
    case CTL_ERR_PROTO:
        return "malformed reply from tetrisd";
    default:
        break;
    }
    const char *reason = htttp_reason(rc);
    return reason ? reason : "refused";
}

void ctl_fmt_uptime(char *out, size_t cap, long secs)
{
    if (secs < 60)
        snprintf(out, cap, "%lds", secs);
    else if (secs < 3600)
        snprintf(out, cap, "%ldm%02lds", secs / 60, secs % 60);
    else
        snprintf(out, cap, "%ldh%02ldm", secs / 3600, (secs % 3600) / 60);
}

/* ---- decode -------------------------------------------------------------- */

int ctl_decode_status(const char *body, CtlStatus *out)
{
    if (find_key(body, "uptime") == NULL)
        return -1;
    out->uptime = json_int(body, "uptime", 0);
    out->sessions = (int)json_int(body, "sessions", 0);
    out->rooms = (int)json_int(body, "rooms", 0);
    return 0;
}

typedef struct
{
    CtlRoom *out;
    int cap;
} RoomCtx;

static void room_item(const char *item, int index, void *vctx)
{
    RoomCtx *c = vctx;
    if (index >= c->cap)
        return;
    CtlRoom *r = &c->out[index];
    r->id = (int)json_int(item, "id", 0);
    r->members = (int)json_int(item, "members", 0);
    r->owner = (int)json_int(item, "owner", -1);
    if (!json_str(item, "phase", r->phase, sizeof r->phase))
        snprintf(r->phase, sizeof r->phase, "?");
}

int ctl_decode_rooms(const char *body, CtlRoom *out, int cap)
{
    if (strchr(body, '[') == NULL)
        return -1;
    RoomCtx ctx = {out, cap};
    int n = 0;
    json_each(body, room_item, &ctx, &n);
    return n > cap ? cap : n;
}

typedef struct
{
    CtlPlayer *out;
    int cap;
} PlayerCtx;

static void player_item(const char *item, int index, void *vctx)
{
    PlayerCtx *c = vctx;
    if (index >= c->cap)
        return;
    CtlPlayer *p = &c->out[index];
    p->room = (int)json_int(item, "room", -1);
    p->player = (int)json_int(item, "player", 0);
    p->pid = (int)json_int(item, "pid", 0);
    p->score = (int)json_int(item, "score", 0);
    p->lines = (int)json_int(item, "lines", 0);
    p->is_owner = json_bool(item, "owner");
    if (!json_str(item, "name", p->name, sizeof p->name))
        p->name[0] = '\0';
}

int ctl_decode_players(const char *body, CtlPlayer *out, int cap)
{
    if (strchr(body, '[') == NULL)
        return -1;
    PlayerCtx ctx = {out, cap};
    int n = 0;
    json_each(body, player_item, &ctx, &n);
    return n > cap ? cap : n;
}

/* ---- snapshot ------------------------------------------------------------ */

int ctl_refresh(const char *sock, int timeout_ms, CtlSnapshot *snap)
{
    static char body[CTL_BODY_MAX];
    CtlStatus st;
    CtlRoom rooms[MAX_ROOMS];
    CtlPlayer players[MAX_SESSIONS];
    int rc;

    /* Decoded into locals and committed only once all three succeeded, so a
     * mid-sequence failure leaves the held snapshot whole rather than half
     * updated from two different instants. */
    rc = ctl_request(sock, "STATUS", "/", timeout_ms, body, sizeof body);
    if (rc != 200)
        goto request_failed;
    if (ctl_decode_status(body, &st) != 0)
        goto decode_failed;

    rc = ctl_request(sock, "ROOMS", "/", timeout_ms, body, sizeof body);
    if (rc != 200)
        goto request_failed;
    int n_rooms = ctl_decode_rooms(body, rooms, MAX_ROOMS);
    if (n_rooms < 0)
        goto decode_failed;

    rc = ctl_request(sock, "PLAYERS", "/", timeout_ms, body, sizeof body);
    if (rc != 200)
        goto request_failed;
    int n_players = ctl_decode_players(body, players, MAX_SESSIONS);
    if (n_players < 0)
        goto decode_failed;

    snap->status = st;
    memcpy(snap->rooms, rooms, sizeof(CtlRoom) * (size_t)n_rooms);
    memcpy(snap->players, players, sizeof(CtlPlayer) * (size_t)n_players);
    snap->n_rooms = n_rooms;
    snap->n_players = n_players;
    snap->have = true;
    snap->stale = false;
    snap->taken = time(NULL);
    (void)log_send(
        LOG_INFO,
        "operation=ctl_refresh phase=complete sessions=%d rooms=%d players=%d "
        "status=0",
        snap->status.sessions, snap->n_rooms, snap->n_players);
    return 0;

decode_failed:
    snap->stale = true;
    (void)log_send(LOG_ERROR, "operation=ctl_refresh phase=complete status=%d",
                   CTL_ERR_PROTO);
    return CTL_ERR_PROTO;

request_failed:
    snap->stale = true;
    (void)log_send(LOG_INFO, "operation=ctl_refresh phase=complete status=%d",
                   rc);
    return rc;
}
