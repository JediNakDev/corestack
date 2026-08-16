/**
 * @file net.c
 * @brief The real backend: TCP + tetrissh + htttp.
 *
 * Everything here is the mirror image of tetrisd's session.c. The server
 * accepts and pushes; we connect and receive. Both ends speak the same htttp
 * grammar over the same encrypted framing, and BOTH ends parse requests -
 * UPD_GAME and UPD_SESSION arrive as requests, not responses (see htttp.h).
 *
 */

#include "libhtttp/htttp.h"
#include "tetrisu/client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/crypto.h> /* OPENSSL_cleanse - scrub the typed password */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The server dispatches on METHOD alone and ignores the path (session.c
 * session_dispatch). htttp still requires a syntactically valid one, so send
 * the shortest legal path rather than inventing routes the server would not
 * read. htttp.md documents richer paths; adopt them when tetrisd does. */
/* Content-Type values fixed by the handout: commands one way, state the
 * other. */
#define CT_COMMAND "application/tetris-command"

/* The one path that carries no Player-Id: it is what establishes one. */
#define AUTH_PATH "/auth"

/* --- connect ------------------------------------------------------------- */

static int make_connect_socket(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof srv);
    srv.sin_family = AF_INET;
    srv.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &srv.sin_addr) != 1)
    { /* "1.2.3.4" -> binary */
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&srv, sizeof srv) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int client_connect(Client *c, const char *ip, int port, const char *ca_path)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->phase = CLI_DISCONNECTED;
    c->last_winner = -1;

    int fd = make_connect_socket(ip, port);
    if (fd < 0)
        return CLIENT_ERR_CONNECT; /* distinct from a mid-handshake hangup */

    /* Returned verbatim, not squashed to -1: the caller shows a different
     * message for a rejected certificate than for a dead route, and
     * SESSION_ERR_AUTH is the only way to tell them apart. */
    int rc = session_connect(&c->sh, fd, ca_path);
    if (rc != SESSION_OK)
    {
        close(fd);
        return rc;
    }

    c->fd = fd;
    c->phase = CLI_CONNECTED;
    return 0;
}

/* "No connection implies the auth count is zero" (#57), enforced on the exit
 * edge - client_connect()'s memset is the entry edge. Shared by
 * client_disconnect() and client_service()'s CLI_EV_DISCONNECT arm, so a
 * Client that outlives its socket never keeps counting for a connection the
 * server has already forgotten. reconnect_ok is deliberately NOT cleared
 * here: main()'s reconnect loop (#61) reads it before calling
 * client_disconnect(), and the next client_connect() memset clears it
 * moments later regardless. */
static void clear_auth(Client *c)
{
    c->auth_failures = 0;
    c->auth_refused_last = false;
    c->auth_pending = AUTH_NONE;
    c->auth_reply = AUTH_NONE;
}

void client_disconnect(Client *c)
{
    if (c->fd >= 0)
    {
        session_close(&c->sh);
        close(c->fd);
        c->fd = -1;
    }
    c->phase = CLI_DISCONNECTED;
    clear_auth(c);
}

int client_fd(const Client *c)
{
    return c->fd;
}

/* --- send ---------------------------------------------------------------- */

/* Serialize one request and hand it to the session layer. */
static int send_cmd(Client *c, const char *method, const char *path,
                    const uint8_t *body, uint32_t body_len)
{
    if (c->fd < 0)
        return -1;

    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.path, sizeof req.path, "%s", path);
    if (body_len > 0)
        (void)htttp_header_set(req.headers, &req.n_headers, "Content-Type",
                               CT_COMMAND);
    /*
     * Player-Id rides on everything except the auth methods themselves.
     *
     * Not gated on "auth has succeeded": a client may pipeline JOIN straight
     * behind GUEST without waiting for the reply, and that JOIN is still an
     * authenticated request. Keying off the request rather than the reply
     * removes the race entirely - the auth exchange is the one thing that
     * cannot carry an identity, because it is what establishes one.
     *
     * Before JOIN the value is the default; the server checks only that it is
     * present until it has named us, and checks the value afterwards.
     */
    if (strcmp(path, AUTH_PATH) != 0)
    {
        char player_id[16];
        snprintf(player_id, sizeof player_id, "%d", c->session.player_id);
        (void)htttp_header_set(req.headers, &req.n_headers, "Player-Id",
                               player_id);
    }
    req.body = body;
    req.body_len = body_len;

    uint8_t out[HTTTP_MAX_FRAME];
    uint32_t out_len = sizeof out;
    if (htttp_serialize_request(&req, out, &out_len) != HTTTP_OK)
        return -1;
    return session_send(&c->sh, out, out_len) == SESSION_OK ? 0 : -1;
}

/* The two path shapes the handout fixes. room_path takes an id because JOIN
 * names the room it is ASKING for, before any UPD_SESSION has arrived;
 * player_path is only used mid-round, when both ids are known. */
static void room_path(char *out, size_t cap, int room)
{
    snprintf(out, cap, "/room/%d", room);
}

static void player_path(const Client *c, char *out, size_t cap)
{
    snprintf(out, cap, "/room/%d/player/%d", c->session.room_id,
             c->session.player_id);
}

/* One word of argument: LEFT/RIGHT, CW/CCW, SOFT/HARD. See body_is(). */
static int send_word(Client *c, const char *method, const char *word)
{
    char path[HTTTP_MAX_PATH];
    player_path(c, path, sizeof path);
    return send_cmd(c, method, path, (const uint8_t *)word,
                    (uint32_t)strlen(word));
}

int client_join(Client *c, uint8_t room_id)
{
    c->room_req = room_id;
    c->assume_owner = (room_id == 0); /* see client.h */
    char path[HTTTP_MAX_PATH];
    room_path(path, sizeof path, room_id);
    if (send_cmd(c, "JOIN", path, NULL, 0) != 0)
        return -1;
    c->phase = CLI_WAITING; /* optimistic: no ack exists */
    return 0;
}

int client_leave(Client *c)
{
    /* Built first: the optimistic reset below clears the id the path needs.
     * Falls back to the room we asked for if no UPD_SESSION has landed. */
    char path[HTTTP_MAX_PATH];
    room_path(path, sizeof path,
              c->have_session ? c->session.room_id : c->room_req);

    int rc = send_cmd(c, "LEAVE", path, NULL, 0);
    c->phase = CLI_CONNECTED;
    c->room_req = 0;
    c->assume_owner = false;
    /*
     * Optimistically drop our room membership, the same way phase is set
     * optimistically above - there is no ack to wait for.
     *
     * This is what lets client_service discard the UPD_GAME frames the server
     * had already queued for us before it saw the LEAVE. Without it those
     * frames are still "ours" as far as the receive path can tell, and the
     * next wait screen gets shoved into a game we walked out of.
     */
    c->session.room_id = ROOM_NONE;
    c->have_game = false;
    return rc;
}

int client_start(Client *c)
{
    char path[HTTTP_MAX_PATH];
    room_path(path, sizeof path,
              c->have_session ? c->session.room_id : c->room_req);
    return send_cmd(c, "START", path, NULL, 0);
}
int client_move(Client *c, int right)
{
    return send_word(c, "MOVE", right ? "RIGHT" : "LEFT");
}
int client_rotate(Client *c, int right)
{
    return send_word(c, "ROTATE", right ? "CW" : "CCW");
}
int client_drop(Client *c, int hard)
{
    return send_word(c, "DROP", hard ? "HARD" : "SOFT");
}
int client_hold(Client *c)
{
    char path[HTTTP_MAX_PATH];
    player_path(c, path, sizeof path);
    return send_cmd(c, "HOLD", path, NULL, 0);
}

int client_history(Client *c)
{
    return send_cmd(c, "HISTORY", "/player/history", NULL, 0);
}

/* --- auth ------------------------------------------------------------- */

/* LOGIN/REGISTER body, per libtetrisauth's contract: "username\npassword",
 * split at the first LF, so the password charset is unconstrained by
 * construction and only the username may not contain one. 15 + 1 + 128 is
 * the largest legal pair (#47); comfortably below that is enough headroom
 * for a client that also enforces the allowlist before ever calling this. */
#define AUTH_BODY_MAX 256

static int send_auth(Client *c, AuthMethod method, const char *wire_method,
                     const char *username, const char *password)
{
    c->auth_pending = method;

    char body[AUTH_BODY_MAX];
    int n = snprintf(body, sizeof body, "%s\n%s", username, password);
    int rc = (n < 0 || (size_t)n >= sizeof body)
                 ? -1
                 : send_cmd(c, wire_method, AUTH_PATH, (const uint8_t *)body,
                            (uint32_t)n);
    /* The typed password lived here, on our own stack, for exactly this
     * call - scrub it on every exit rather than let it wait for the frame
     * that reuses this stack slot to overwrite it. */
    OPENSSL_cleanse(body, sizeof body);
    return rc;
}

int client_login(Client *c, const char *username, const char *password)
{
    return send_auth(c, AUTH_LOGIN, "LOGIN", username, password);
}

int client_register(Client *c, const char *username, const char *password)
{
    return send_auth(c, AUTH_REGISTER, "REGISTER", username, password);
}

int client_guest(Client *c)
{
    c->auth_pending = AUTH_GUEST;
    return send_cmd(c, "GUEST", AUTH_PATH, NULL, 0);
}

/* --- receive ------------------------------------------------------------- */

/**
 * Turns an auth reply into an event, noting a refusal on the way.
 *
 * Anything that is not a 2xx is a refusal here, matching the server's own
 * retry rule (auth_retry_handler() re-reads a frame on every non-200), so the
 * two ends agree on what spends an attempt without sharing code. Kept separate
 * from net_service() for readability, not because there is a second caller.
 */
static ClientEvent fold_auth_reply(Client *c)
{
    c->auth_reply = c->auth_pending;
    c->auth_pending = AUTH_NONE;

    if (c->last_reject >= 200 && c->last_reject < 300)
    {
        c->auth_refused_last = false;
        return CLI_EV_AUTH_OK;
    }

    c->auth_failures++;
    c->auth_refused_last = true;
    return CLI_EV_REJECT;
}

/* The receive body: one frame off the wire, turned into a ClientEvent.
 * client_service() wraps it and is the only thing that interprets an event -
 * this function never touches the auth budget. */
static ClientEvent net_service(Client *c)
{
    uint32_t len = sizeof c->rxbuf;
    int rc = session_recv(&c->sh, c->rxbuf, &len);

    if (rc == SESSION_ERR_IO || rc == SESSION_ERR_TOOBIG)
    {
        /* TOOBIG on receive is unrecoverable, not just a bad frame: the stream
         * is out of sync and libtetrissh has already marked the session dead
         * (tetrissh.h). Both mean "stop". */
        c->phase = CLI_DISCONNECTED;
        return CLI_EV_DISCONNECT;
    }
    if (rc != SESSION_OK)
        return CLI_EV_NONE; /* NOSPACE/CRYPTO: frame lost, live on */

    /*
     * The server sends BOTH shapes, so try both.
     *
     * State updates are pushed as requests (UPD_GAME / UPD_SESSION /
     * UPD_RESULT) because they are unsolicited. Refusals - and now auth
     * replies - come back as responses, because they answer a command we
     * just sent. Responses are tried first: a status line is unambiguous,
     * whereas a response would not parse as a request anyway.
     */
    htttp_response_t res;
    if (htttp_parse_response(c->rxbuf, len, &res) == HTTTP_OK)
    {
        /*
         * A 2xx answers a command rather than refusing it.
         *
         * Every other response here is a refusal, and the auth path folds one
         * into the attempt budget - so a success arriving as a response has to
         * be separated out, or a 201 would read as a rejection and, mid-auth,
         * would be counted as a failed login. The only 2xx the server sends
         * unsolicited is the 201 for a room-creating JOIN, whose real payload
         * is the UPD_SESSION right behind it; there is nothing to do but not
         * mistake it for a refusal.
         */
        if (res.status >= 200 && res.status < 300 &&
            c->auth_pending == AUTH_NONE)
            return CLI_EV_NONE;

        c->last_reject = res.status;
        /* An auth reply is folded by client_service(), not here (#62) - this
         * function's job stops at "here is the status that arrived". */
        if (c->auth_pending != AUTH_NONE)
            return CLI_EV_AUTH_REPLY;
        return CLI_EV_REJECT;
    }

    htttp_request_t req;
    if (htttp_parse_request(c->rxbuf, len, &req) != HTTTP_OK)
        return CLI_EV_NONE;

    if (strcmp(req.method, "STATE") == 0)
    {
        /*
         * Exact-length check before the copy.
         *
         * htttp guarantees body_len equals the declared Content-Length and
         * that those bytes are present - but it has no idea what a GameState
         * is. A short frame would leave most of the struct holding the
         * PREVIOUS tick's data while looking like a fresh update, and a long
         * one would be a straight overrun. Equality, not >=.
         */
        if (req.body == NULL || req.body_len != sizeof c->game)
            return CLI_EV_NONE;

        /*
         * Drop frames from a room we are no longer in.
         *
         * The server pushes UPD_GAME at 20 Hz, so leaving mid-round leaves a
         * backlog queued on the socket - and the lobby menu blocks on input
         * without servicing it, so the backlog is still there when we next
         * open a screen. Accepting one flips us to CLI_PLAYING below, which
         * sends the wait screen straight into a game that ended for us, and
         * the client then bounces to the lobby while the server still has us
         * in the new room. Every JOIN after that is refused with 409 and there
         * is no way back.
         *
         * client_leave clears room_id immediately, so this catches the backlog
         * even though the server's own LEAVE acknowledgement is queued behind
         * it.
         */
        if (c->session.room_id == ROOM_NONE)
            return CLI_EV_NONE;

        memcpy(&c->game, req.body, sizeof c->game);
        c->have_game = true;
        c->frames_seen++;

        /* The only signal that the room started: the owner pressed START, the
         * server broadcast ADMIN_SEED, and frames began. Nothing else tells a
         * non-owner (client.h). */
        if (c->phase == CLI_WAITING || c->phase == CLI_CONNECTED)
            c->phase = CLI_PLAYING;
        if (c->game.game_over)
            c->phase = CLI_GAME_OVER;
        return CLI_EV_GAME;
    }

    if (strcmp(req.method, "UPD_SESSION") == 0)
    {
        /*
         * The lobby's only acknowledgement. JOIN, LEAVE and START have no
         * responses of their own, so this push is what tells us the room id we
         * were actually given, our player id, and whether we own the room -
         * replacing every value the UI would otherwise have to infer.
         */
        if (req.body == NULL || req.body_len != sizeof c->session)
            return CLI_EV_NONE;

        memcpy(&c->session, req.body, sizeof c->session);
        c->have_session = true;

        switch (c->session.phase)
        {
        case SESSION_WAITING:
            c->phase = CLI_WAITING;
            break;
        case SESSION_PLAYING:
            c->phase = CLI_PLAYING;
            break;
        case SESSION_GAME_OVER:
            c->phase = CLI_GAME_OVER;
            break;
        case SESSION_IDLE:
            c->phase = CLI_CONNECTED;
            break;
        default:
            break;
        }
        return CLI_EV_SESSION;
    }

    if (strcmp(req.method, "UPD_RESULT") == 0)
    {
        /* Round over. The body is the winning player_id as an int32. */
        int32_t winner;
        if (req.body == NULL || req.body_len != sizeof winner)
            return CLI_EV_NONE;
        memcpy(&winner, req.body, sizeof winner);
        c->last_winner = (int)winner;
        return CLI_EV_RESULT;
    }

    if (strcmp(req.method, "UPD_HISTORY") == 0)
    {
        /* Same exact-length discipline as STATE/UPD_SESSION above: htttp
         * guarantees body_len equals what it declared, not what a
         * player_history_t is. */
        if (req.body == NULL || req.body_len != sizeof c->history)
            return CLI_EV_NONE;
        memcpy(&c->history, req.body, sizeof c->history);
        c->have_history = true;
        return CLI_EV_HISTORY;
    }

    return CLI_EV_NONE; /* unknown method: not ours to judge */
}

ClientEvent client_service(Client *c)
{
    bool possible_cap = c->auth_refused_last;
    c->auth_refused_last = false;

    ClientEvent ev = net_service(c);

    if (ev == CLI_EV_AUTH_REPLY)
        ev = fold_auth_reply(c);

    if (ev == CLI_EV_DISCONNECT)
    {
        c->reconnect_ok = possible_cap;
        clear_auth(c); /* "no connection implies zero" (#57), exit edge */
    }

    return ev;
}

const char *client_phase_str(ClientPhase p)
{
    switch (p)
    {
    case CLI_DISCONNECTED:
        return "disconnected";
    case CLI_CONNECTED:
        return "lobby";
    case CLI_WAITING:
        return "waiting";
    case CLI_PLAYING:
        return "playing";
    case CLI_GAME_OVER:
        return "game over";
    }
    return "?";
}
