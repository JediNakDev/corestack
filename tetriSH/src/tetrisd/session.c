/*
 * session.c - one client's game session.
 *
 * Its own binary: tetrisd forks and execs it per client, passing the accepted
 * socket's fd as argv[1]. Runs the game for that one client, then exits.
 *
 * Layering (bottom to top):
 *   libtetrissh  - encrypted, authenticated frames  (session_accept/recv/send)
 *   libhtttp     - frame bytes <-> request struct    (htttp_parse_request)
 *   this file    - dispatch method -> brain / admin  (the app logic)
 *   libtetrisbrain - the game rules
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "tetrisd/session.h"
#include "libhtttp/htttp.h"
#include "libtetrisauth/auth.h"
#include "libtetrisbrain/board.h" /* board_add_garbage */
#include "libtetrisbrain/tetrisbrain.h"
#include "libtetrissh/tetrissh.h"
#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include "tetrisd/adminmsg.h"
#include "tetrisd/history.h"
#include <errno.h>
#include <limits.h>
#include <openssl/pem.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Load the server's RSA private key.
 *
 * Plain OpenSSL rather than libtetrissh's load_private_key(): that lives in
 * core/src/libtetrissh/common.h, which is deliberately NOT in include/ so the
 * OpenSSL helper surface stays private to that library. session_accept() takes
 * an EVP_PKEY* precisely so the application owns key loading.
 */
static EVP_PKEY *load_server_key(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return NULL;
    EVP_PKEY *key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);
    return key;
}

/* Gravity clock: the brain advances one tick per TICK_MS of real time. */
#define TICK_HZ 20
#define TICK_MS (1000 / TICK_HZ) /* 50 ms */

/* Monotonic millisecond clock (immune to wall-clock changes). */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* One AdminMsg to/from the admin thread over the socketpair. The pair is a
 * stream, so both directions go through the framing helpers in adminmsg.h.
 * Return 0 on success, -1 on error or a closed peer. */
static int admin_send(int admin_fd, const AdminMsg *m)
{
    return adminmsg_write(admin_fd, m);
}
static int admin_recv(int admin_fd, AdminMsg *m)
{
    return adminmsg_read(admin_fd, m) == 1 ? 0 : -1;
}

/* Fresh session: connected, but not in a room and not playing yet. */
static void session_reset(Session *s, int client_fd, int admin_fd)
{
    s->client_fd = client_fd;
    s->admin_fd = admin_fd;
    s->session.phase = SESSION_IDLE;
    s->session.room_id = ROOM_NONE;
    s->session.player_id = 0;
    s->session.is_owner = false;

    /*
     * The board is only initialised once the room starts (START), so every
     * player in the room gets the same seed and piece sequence.
     *
     * The scoreboard, though, has to be cleared HERE. tetrisbrain_init
     * deliberately leaves standings alone - they are not the engine's data -
     * so nothing else would ever zero them, and s->game is uninitialised stack
     * until START. Without this, the first UPD_GAME after joining would ship
     * whatever bytes happened to be on the stack as opponents' names and
     * scores.
     */
    memset(&s->game, 0, sizeof s->game);
    s->game.my_player_id = -1; /* not in a room yet */

    s->user_name[0] = '\0';
    s->ts_start = 0;
}

/* The room started: seed comes from the admin thread, shared by all players. */
static void session_start_game(Session *s, unsigned seed)
{
    tetrisbrain_init(&s->game, seed);
    s->session.phase = SESSION_PLAYING;
    s->ts_start = (long long)time(NULL);
}

/*
 * Forward any attack the brain has banked, and clear it.
 *
 * The rules decide how much garbage a clear is worth (see garbage_earn in
 * action.c); this only carries it. Cleared only once the admin has taken it,
 * so a full socketpair delays an attack rather than losing it.
 */
static void session_flush_garbage(Session *s)
{
    if (s->game.garbage_out <= 0)
        return;

    AdminMsg out = {.type = ADMIN_GARBAGE, .lines = s->game.garbage_out};
    if (admin_send(s->admin_fd, &out) == 0)
        s->game.garbage_out = 0;
}

/* Content-Type values fixed by the handout: commands one way, state the
 * other. */
#define CT_COMMAND "application/tetris-command"
#define CT_STATE "application/tetris-state"

/* Does the body hold exactly this token? Arguments are words - LEFT/RIGHT,
 * CW/CCW, SOFT/HARD - not the raw 0/1 bytes an earlier revision sent, where
 * 0x01 and '1' were easy to confuse and only one worked. */
static bool body_is(const htttp_request_t *req, const char *token)
{
    size_t n = strlen(token);
    return req->body != NULL && req->body_len == n &&
           memcmp(req->body, token, n) == 0;
}

/* Defined below, next to the other push helpers. */
static void session_push_reject(session_t *sh, int status);

/* --- routing -------------------------------------------------------------
 *
 * One row per method, so adding one is a row rather than a branch.
 *
 * A strcmp ladder falling off the bottom knows only "nothing matched"; the
 * table separates no-such-method (501) from right-method-wrong-phase (409).
 *
 * Paths are fixed by the handout:
 *   JOIN | LEAVE | START   /room/<id>
 *   MOVE | ROTATE | DROP   /room/<id>/player/<pid>
 * HOLD is ours, and rides with the other per-player actions.
 */

typedef enum
{
    ROUTE_ANY,     /* valid in any phase          */
    ROUTE_PLAYING, /* only while a round is running */
} RouteWhen;

typedef enum
{
    PATH_ROOM,        /* /room/<id>              */
    PATH_ROOM_PLAYER, /* /room/<id>/player/<pid> */
} RouteShape;

/* Returns the status earned, rather than logging it, so the log line lives in
 * one place. room comes from the path, already checked against the session. */
typedef int (*RouteFn)(Session *s, const htttp_request_t *req, int room);

static int route_move(Session *s, const htttp_request_t *req, int room)
{
    (void)room;
    if (body_is(req, "LEFT"))
        tetrisbrain_input(&s->game, MOVE_LEFT);
    else if (body_is(req, "RIGHT"))
        tetrisbrain_input(&s->game, MOVE_RIGHT);
    else
        return 400;
    return 200;
}

static int route_rotate(Session *s, const htttp_request_t *req, int room)
{
    (void)room;
    if (body_is(req, "CW"))
        tetrisbrain_input(&s->game, MOVE_ROT_RIGHT);
    else if (body_is(req, "CCW"))
        tetrisbrain_input(&s->game, MOVE_ROT_LEFT);
    else
        return 400;
    return 200;
}

static int route_drop(Session *s, const htttp_request_t *req, int room)
{
    (void)room;
    if (body_is(req, "SOFT"))
        tetrisbrain_input(&s->game, MOVE_SOFT_DROP);
    else if (body_is(req, "HARD"))
        tetrisbrain_input(&s->game, MOVE_HARD_DROP);
    else
        return 400;
    session_flush_garbage(s); /* a drop can complete a clear */
    return 200;
}

static int route_hold(Session *s, const htttp_request_t *req, int room)
{
    (void)req; /* HOLD() takes no argument */
    (void)room;
    tetrisbrain_input(&s->game, HOLD);
    return 200;
}

/* The three below hand off to the admin thread. A failed send is ours, not
 * the client's, so it reports as 500. */

static int route_join(Session *s, const htttp_request_t *req, int room)
{
    (void)req;
    /* roomid 0=new, >0=that room (htttp.md). The one id the dispatcher does
     * NOT validate: a JOIN asks for a room, it does not claim to be in one. */
    AdminMsg out = {.type = ADMIN_JOIN, .room_id = room};
    auth_get_name(out.name, sizeof out.name);
    snprintf(s->user_name, sizeof s->user_name, "%s", out.name);
    /* admin replies with ADMIN_SESSION; handled in the main loop. */
    return admin_send(s->admin_fd, &out) == 0 ? 200 : 500;
}

static int route_leave(Session *s, const htttp_request_t *req, int room)
{
    (void)req;
    (void)room;
    AdminMsg out = {.type = ADMIN_LEAVE};
    return admin_send(s->admin_fd, &out) == 0 ? 200 : 500;
}

static int route_start(Session *s, const htttp_request_t *req, int room)
{
    (void)req;
    (void)room;
    AdminMsg out = {.type = ADMIN_START};
    /* admin replies with ADMIN_SEED -> session_start_game(s, seed). */
    return admin_send(s->admin_fd, &out) == 0 ? 200 : 500;
}

static const struct
{
    const char *method;
    RouteWhen when;
    RouteShape shape;
    bool owns_room; /* false only for JOIN; see route_join */
    RouteFn fn;
} ROUTES[] = {
    {"MOVE", ROUTE_PLAYING, PATH_ROOM_PLAYER, true, route_move},
    {"ROTATE", ROUTE_PLAYING, PATH_ROOM_PLAYER, true, route_rotate},
    {"DROP", ROUTE_PLAYING, PATH_ROOM_PLAYER, true, route_drop},
    {"HOLD", ROUTE_PLAYING, PATH_ROOM_PLAYER, true, route_hold},
    {"JOIN", ROUTE_ANY, PATH_ROOM, false, route_join},
    {"LEAVE", ROUTE_ANY, PATH_ROOM, true, route_leave},
    {"START", ROUTE_ANY, PATH_ROOM, true, route_start},
};

#define N_ROUTES (sizeof ROUTES / sizeof ROUTES[0])

/* Ids out of a path of the given shape; false if it is not that shape (404).
 * %n is checked against the full length because sscanf stops happily at the
 * end of its format - otherwise /room/1/player/2/../../etc routes as room 1. */
static bool path_ids(const char *path, RouteShape shape, int *room,
                     int *player_id)
{
    int n = 0;
    size_t len = strlen(path);

    if (shape == PATH_ROOM)
    {
        if (sscanf(path, "/room/%d%n", room, &n) != 1 || (size_t)n != len)
            return false;
        *player_id = -1;
        return true;
    }
    if (sscanf(path, "/room/%d/player/%d%n", room, player_id, &n) != 2 ||
        (size_t)n != len)
        return false;
    return true;
}

/*
 * Route one parsed request. Method and path are the fixed table in htttp.md.
 */
static void session_dispatch(Session *s, session_t *sh,
                             const htttp_request_t *req)
{
    /* Guest -> account upgrade (LOGIN/REGISTER/GUEST, pre-auth gate already
     * passed). Ahead of the table: it owns its methods and answers them
     * itself. See core/include/libtetrisauth/auth.h. */
    if (auth_offer(req, &s->session))
        return;

    /* HISTORY, same shape as auth_offer() above: it owns its method end to
     * end (query, guest handling, the 409-while-playing refusal) and answers
     * on the wire itself. See include/tetrisd/history.h. */
    if (history_offer(req, sh, s->user_name, s->session.phase))
        return;

    int status = 501; /* no row matched: the method does not exist here */
    bool answer = true;

    for (size_t i = 0; i < N_ROUTES; i++)
    {
        if (strcmp(req->method, ROUTES[i].method) != 0)
            continue;

        if (ROUTES[i].when == ROUTE_PLAYING &&
            s->session.phase != SESSION_PLAYING)
        {
            status = 409; /* real method, wrong moment */
            answer = false;
            break;
        }

        /* Player-Id is required on every authenticated request, and every row
         * here is one: the auth methods are answered by auth_offer above.
         * Absent means the request cannot say who it is - 401.
         *
         * Only presence is checked before a room exists, because the server
         * has not named this client yet; room.c hands ids out in join order.
         * Once it has, the value must match, for the same reason the path
         * must - it is a claim about identity. */
        const char *player_id =
            htttp_header_get(req->headers, req->n_headers, "Player-Id");
        if (player_id == NULL)
        {
            status = 401;
            break;
        }
        if (s->session.room_id != ROOM_NONE &&
            atoi(player_id) != s->session.player_id)
        {
            status = 403;
            break;
        }

        int room = 0, player_id_in_path = -1;
        if (!path_ids(req->path, ROUTES[i].shape, &room, &player_id_in_path))
        {
            status = 404; /* right method, path is not the shape it must be */
            break;
        }

        /* The path is an identity claim, so it is checked, not trusted: MOVE
         * /room/5/player/9 on player 3's socket would otherwise move someone
         * else's piece. JOIN is exempt - see route_join. */
        if (ROUTES[i].owns_room &&
            (room != s->session.room_id ||
             (ROUTES[i].shape == PATH_ROOM_PLAYER &&
              player_id_in_path != s->session.player_id)))
        {
            status = 403;
            break;
        }

        status = ROUTES[i].fn(s, req, room);
        break;
    }

    /*
     * Answer the client's mistakes; stay silent otherwise.
     *
     * Not successes: the client reads ANY response as a refusal (net.c) and
     * folds it into the auth budget while auth is in flight, so a 200 per MOVE
     * would look like a rejection AND corrupt the attempt count. Success shows
     * up as the STATE push that follows, which is what the client watches.
     *
     * Not the 409 above either, which is a race rather than a mistake: START is
     * answered by the admin thread, so commands sent in the window before the
     * phase flips are wrong-but-inevitable. Answering them would put a burst of
     * refusals on the wire per player at exactly the busiest moment, and tell a
     * correct client something it can do nothing with. 409 stays reachable from
     * room.c, which refuses deliberate conflicts.
     *
     * What is left - 400, 403, 404, 501, 500 - is malformed, forged, or ours.
     * Each is actionable, and none is on the hot path.
     */
    if (answer && status != 200)
        session_push_reject(sh, status);

    (void)log_send(status >= 500   ? LOG_ERROR
                   : status >= 400 ? LOG_WARN
                                   : LOG_INFO,
                   "operation=session_dispatch phase=complete method=%s "
                   "status=%d",
                   req->method, status);
}

/* Serialize and send one request to the client. Shared by every push.
 * ctype may be NULL for a push with no body worth typing. */
static void push_request(session_t *sh, const char *method, const char *path,
                         const char *ctype, const void *body, uint32_t body_len)
{
    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.path, sizeof req.path, "%s", path);
    if (ctype != NULL)
        (void)htttp_header_set(req.headers, &req.n_headers, "Content-Type",
                               ctype);
    req.body = (const uint8_t *)body;
    req.body_len = body_len;

    uint8_t out[SESSION_MAX_FRAME];
    uint32_t out_len = sizeof out;
    int rc = htttp_serialize_request(&req, out, &out_len);
    if (rc != HTTTP_OK)
    {
        (void)log_send(
            LOG_ERROR,
            "operation=push_request phase=complete method=%s status=%d", method,
            rc);
        return;
    }
    rc = session_send(sh, out, out_len);
    if (rc != SESSION_OK)
    {
        (void)log_send(
            LOG_ERROR,
            "operation=push_request phase=complete method=%s status=%d", method,
            rc);
        return;
    }
    (void)log_send(strcmp(method, "STATE") == 0 ? LOG_DEBUG : LOG_INFO,
                   "operation=push_request phase=complete method=%s status=0",
                   method);
}

/*
 * Push the session state (phase, room, player id, owner flag) to the client.
 *
 * This is the client's ONLY source of truth about the lobby: JOIN, LEAVE and
 * START have no responses of their own, so this push IS their acknowledgement.
 * Without it a client cannot tell a successful join from a refused one, and a
 * non-owner is never told the game began.
 */
static void session_push_session(Session *s, session_t *sh)
{
    push_request(sh, "UPD_SESSION", "/session/state", CT_STATE, &s->session,
                 (uint32_t)sizeof s->session);
}

/*
 * Tell the client a request was refused, as an htttp RESPONSE.
 *
 * Refusals are the one server->client message that is a response rather than a
 * pushed request: it answers a specific command the client just sent, and
 * htttp already defines the status codes for it (403 not the owner, 409 already
 * in a room, 503 server full). Previously every one of these paths just
 * returned silently, which is why a refused JOIN looked exactly like a
 * successful one.
 */
/* Send one bare response. Named for its usual job, but 201 goes out here too:
 * it is the one status that answers rather than refuses. */
static void session_push_reject(session_t *sh, int status)
{
    htttp_response_t res;
    memset(&res, 0, sizeof res);
    res.status = status;

    uint8_t out[SESSION_MAX_FRAME];
    uint32_t out_len = sizeof out;
    int rc = htttp_serialize_response(&res, out, &out_len);
    if (rc == HTTTP_OK)
        rc = session_send(sh, out, out_len);

    /* Responses are one of the five categories the brief requires logged, and
     * this is the only place one is sent. */
    (void)log_send(rc != 0         ? LOG_ERROR
                   : status >= 400 ? LOG_WARN
                                   : LOG_INFO,
                   "operation=push_response phase=complete status=%d rc=%d",
                   status, rc);
}

/* Defined below; the standings handler needs it to refresh a client whose own
 * game has ended and whose tick therefore no longer pushes anything. */
static void session_push_game(Session *s, session_t *sh);

/* Apply one message from the admin thread. */
static void session_handle_admin(Session *s, session_t *sh, const AdminMsg *m)
{
    switch (m->type)
    {
    case ADMIN_SESSION:
        /* The one success the server answers. A JOIN that CREATED a room is
         * reported as 201; a JOIN that joined an existing one needs no reply,
         * because the session push below already says where we landed. Sent
         * before that push so the answer precedes the state it describes. */
        if (m->created)
            session_push_reject(sh, 201);
        s->session = m->session; /* new phase / room / owner */
        /* Keep the board's copy in step: this is the only place our
         * player_id is ever assigned or cleared, and the renderer reads it
         * from the GameState to know which scoreboard row is ours. */
        s->game.my_player_id =
            (s->session.room_id == ROOM_NONE) ? -1 : s->session.player_id;
        session_push_session(s, sh);
        break;
    case ADMIN_SEED:
        session_start_game(s, m->seed);
        /* Phase is now PLAYING; tell the client before the first UPD_GAME
         * so it can switch screens rather than infer from frame arrival. */
        session_push_session(s, sh);
        break;
    case ADMIN_RECV_GARBAGE:
        /* Only mid-round: a late attack must not disturb a finished board
         * or one waiting to start. Pushed straight out so the player sees
         * the rows arrive rather than on the next tick.
         *
         * game_over is checked as well as the phase: the phase only flips
         * on the next tick, leaving a window in which a dead board would
         * still take a hit and shift under the GAME OVER banner. */
        if (s->session.phase == SESSION_PLAYING && !s->game.game_over)
        {
            if (!board_add_garbage(&s->game, m->lines))
                s->game.game_over = true;
            session_push_game(s, sh);
        }
        break;
    case ADMIN_STANDINGS:
        /*
         * The room's scoreboard, assembled and ranked by the admin thread.
         *
         * Copied into the GameState rather than pushed to the client on
         * its own: the board is already going out ~20 times a second, so
         * the scoreboard rides along for free and arrives in step with the
         * frame it describes. A separate push would need its own method,
         * its own client-side decode, and could arrive out of order with
         * the board it belongs to.
         *
         * Clamped rather than trusted. Both sides compile the same header,
         * so a count above MAX_STANDINGS should be impossible - but this
         * value is about to index an array, and the cost of being sure is
         * one comparison.
         */
        {
            int n = m->standing_count;
            if (n < 0)
                n = 0;
            if (n > MAX_STANDINGS)
                n = MAX_STANDINGS;
            memcpy(s->game.standings, m->standings,
                   (size_t)n * sizeof s->game.standings[0]);
            s->game.standing_count = n;

            /*
             * While we are playing, the next tick pushes the board anyway
             * and this rides along. Once we have topped out the tick stops
             * pushing - so without this, a dead player's scoreboard would
             * freeze at the instant they died, which is precisely when
             * they have nothing to do but watch the others finish.
             *
             * GAME_OVER specifically, NOT "anything but PLAYING". A frame
             * is the client's start signal - net.c promotes CLI_WAITING to
             * CLI_PLAYING on any UPD_GAME - so pushing one while merely
             * WAITING throws the player into the game screen on a board
             * that has never been initialised and is not being ticked.
             */
            if (s->session.phase == SESSION_GAME_OVER)
                session_push_game(s, sh);
        }
        break;
    case ADMIN_RESULT:
        /* Room finished; m->winner = winning player_id. Back to the lobby.
         * The winner id rides in the body so the client can say who won
         * rather than only that the round ended. */
        s->session.phase = SESSION_WAITING;
        {
            int32_t winner = (int32_t)m->winner;
            push_request(sh, "UPD_RESULT", "/game/result", CT_STATE, &winner,
                         (uint32_t)sizeof winner);
        }
        session_push_session(s, sh);
        break;
    case ADMIN_REJECT:
        /* A command the admin thread refused. reason carries the htttp
         * status so the wire code and the log agree. */
        session_push_reject(sh, m->reason);
        break;
    default:
        /* session->admin types shouldn't arrive here; ignore. */
        break;
    }
}

/*
 * Push the whole game state as STATE /room/<id>. A request, not a response,
 * because the server initiates it.
 *
 * ASSUMPTION (unconfirmed): the body is the raw GameState bytes. Both ends
 * compile libtetrisutil, so the layout matches on the same arch - confirm the
 * format with the client author before relying on it.
 */
static void session_push_game(Session *s, session_t *sh)
{
    char path[HTTTP_MAX_PATH];
    snprintf(path, sizeof path, "/room/%d", s->session.room_id);
    push_request(sh, "STATE", path, CT_STATE, &s->game,
                 (uint32_t)sizeof s->game);
}

void session_main(int client_fd, int admin_fd)
{
    (void)log_send(LOG_INFO,
                   "operation=session_main event=started client_fd=%d "
                   "admin_fd=%d status=0",
                   client_fd, admin_fd);
    Session s;
    session_reset(&s, client_fd, admin_fd);

    char key_path[PATH_MAX], cert_path_buf[PATH_MAX];

    if (rc_get("key_path", NULL, key_path, sizeof key_path) != RC_VALUE_FOUND ||
        rc_get("cert_path", NULL, cert_path_buf, sizeof cert_path_buf) !=
            RC_VALUE_FOUND)
        exit(1);

    EVP_PKEY *priv = load_server_key(key_path);
    if (priv == NULL)
    {
        fprintf(stderr, "session: cannot load private key %s\n", key_path);
        (void)log_send(LOG_ERROR,
                       "operation=session_main phase=complete status=-1 "
                       "reason=private_key");
        close(client_fd);
        close(admin_fd);
        return;
    }

    session_t sh;
    int hs = session_accept(&sh, client_fd, priv, cert_path_buf);
    log_send(LOG_DEBUG, "player session handshake status=%d", hs);
    /* The key is only needed to sign the handshake nonce. Free it immediately
     * so a long-lived session is not sitting on private key material it can no
     * longer use. */
    EVP_PKEY_free(priv);

    if (hs != SESSION_OK)
    {
        log_level_t level = hs == SESSION_ERR_CRYPTO ? LOG_ERROR
                            : hs == SESSION_ERR_AUTH ||
                                    hs == SESSION_ERR_PROTO ||
                                    hs == SESSION_ERR_TOOBIG
                                ? LOG_WARN
                                : LOG_INFO;
        (void)log_send(level,
                       "operation=session_main phase=complete status=%d "
                       "reason=handshake",
                       hs);
        close(client_fd);
        close(admin_fd);
        return;
    }

    /* Pre-auth gate: owns sh until the client is an account or an accepted
     * guest. Must run before the poll loop starts - see
     * core/include/libtetrisauth/auth.h ("THE SEAM"). */
    if (auth_retry_handler(&sh) != AUTH_OK)
    {
        session_close(&sh);
        close(client_fd);
        close(admin_fd);
        return;
    }
    auth_get_name(s.user_name, sizeof s.user_name);
    (void)log_send(LOG_INFO,
                   "operation=session_main event=authenticated client_fd=%d "
                   "status=0",
                   client_fd);

    /* --- main loop ---
     * One poll() multiplexes three things: client htttp frames, admin messages,
     * and the gravity tick deadline. The poll timeout is "ms until the next
     * tick", so gravity fires on time while we still react to input/admin. */
    uint8_t buf[SESSION_MAX_FRAME];
    long next_tick = now_ms() + TICK_MS;

    /* Last figures reported to the admin, so ADMIN_SCORE is sent on change
     * rather than on every tick. Start at -1 so the first real value (0 at the
     * start of a round) still counts as a change and seeds everyone's board. */
    int last_score = -1, last_lines = -1;

    while (true)
    {
        long timeout = next_tick - now_ms();
        if (timeout < 0)
            timeout = 0;

        struct pollfd pfds[2];
        pfds[0].fd = client_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = admin_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, (int)timeout);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* (1) client htttp frame.
         * NOTE: session_recv blocks until a full frame is in (it handles short
         * reads internally). poll only tells us bytes are available, so a
         * fragmented frame could briefly stall the tick. Acceptable for now;
         * revisit if latency matters. */
        if (pfds[0].revents & POLLIN)
        {
            uint32_t len = sizeof buf;
            int rc = session_recv(&sh, buf, &len);
            if (rc == SESSION_OK)
            {
                htttp_request_t req;
                if (htttp_parse_request(buf, len, &req) == HTTTP_OK)
                    session_dispatch(&s, &sh, &req);
            }
            else if (rc == SESSION_ERR_TOOBIG)
            {
                /*
                 * Oversize frame. The receive stream is desynced for good, but
                 * the send side is untouched (tetrissh.h), so the client gets
                 * told why before the socket goes. Best effort by definition -
                 * if the peer has already gone this write just fails - but
                 * silence here is the difference between "too big" and "the
                 * server crashed".
                 */
                session_push_reject(&sh, 413);
                (void)log_send(
                    LOG_WARN,
                    "operation=session_recv phase=complete status=413");
                break;
            }
            else if (rc == SESSION_ERR_IO)
            {
                break; /* client closed */
            }
            /* other recoverable errors: skip this frame, keep going */
        }
        if (pfds[0].revents & (POLLHUP | POLLERR))
            break; /* client disconnected */

        /* (2) admin message (seed on start, garbage, room/owner updates) */
        if (pfds[1].revents & POLLIN)
        {
            AdminMsg m;
            if (admin_recv(admin_fd, &m) == 0)
                session_handle_admin(&s, &sh, &m);
            else
                break; /* admin gone -> tetrisd is shutting down */
        }
        if (pfds[1].revents & (POLLHUP | POLLERR))
            break;

        /* (3) gravity tick, only while a game is running */
        if (now_ms() >= next_tick)
        {
            next_tick += TICK_MS;
            if (s.session.phase == SESSION_PLAYING)
            {
                tetrisbrain_tick(&s.game);
                session_flush_garbage(&s); /* gravity can complete a clear */

                /*
                 * Tell the admin when our figures move, so it can republish
                 * the room's scoreboard and everyone can watch this player's
                 * progress.
                 *
                 * Only on an actual change. Score and lines both move solely
                 * on a line clear, so this is a handful of messages per round
                 * rather than one per tick - the difference between a quiet
                 * socketpair and 20 fan-out broadcasts a second per player.
                 */
                if (s.game.score != last_score || s.game.lines != last_lines)
                {
                    last_score = s.game.score;
                    last_lines = s.game.lines;
                    AdminMsg sc = {.type = ADMIN_SCORE,
                                   .score = s.game.score,
                                   .lines = s.game.lines};
                    admin_send(admin_fd, &sc);
                }

                session_push_game(&s, &sh);

                if (tetrisbrain_game_over(&s.game))
                {
                    s.session.phase = SESSION_GAME_OVER;
                    /* report final score so the admin can rank the room */
                    AdminMsg go = {.type = ADMIN_GAMEOVER,
                                   .score = s.game.score};
                    admin_send(admin_fd, &go);
                    history_db_insert(s.session.player_id, s.user_name,
                                      s.game.score, s.game.lines, s.ts_start,
                                      (long long)time(NULL));
                }
            }
        }
    }

    session_close(&sh);
    close(client_fd);
    close(admin_fd);
    (void)log_send(LOG_INFO, "operation=session_main phase=complete status=0");
}

/*
 * Entry point of the session binary. tetrisd execs it as:
 *     session <client_fd> <admin_fd>
 * where client_fd is the client socket and admin_fd is this session's end of
 * the socketpair to the admin thread.
 */
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <client_fd> <admin_fd>\n", argv[0]);
        return 1;
    }

    /* A dead client socket must not kill us with SIGPIPE on session_send. */
    signal(SIGPIPE, SIG_IGN);

    (void)log_open_configured();
    atexit(log_close);

    int client_fd = atoi(argv[1]);
    int admin_fd = atoi(argv[2]);
    session_main(client_fd, admin_fd);
    return 0;
}
