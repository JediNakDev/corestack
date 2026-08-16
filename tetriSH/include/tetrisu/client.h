#ifndef TETRISU_CLIENT_H
#define TETRISU_CLIENT_H

/**
 * @file client.h
 * @brief Everything tetrisu knows, and the only way it talks to a server.
 *
 * The screens render from a Client and call the functions below; they never
 * touch a socket, a frame buffer or libhtttp. That split is what keeps the
 * wire format in net.c and out of every screen.
 *
 * WHAT THE SERVER TELLS US:
 *   UPD_GAME     - the board, every 50 ms while the room is PLAYING
 *   UPD_SESSION  - phase / room / player id / owner, on every lobby change
 *   UPD_RESULT   - the winning player_id when a round ends
 *   a response   - 403/409/429 when a command is refused
 *
 *   All but the last are pushed REQUESTS, because they are unsolicited; a
 *   refusal is a RESPONSE, because it answers a command we just sent. So
 *   client_service() parses both shapes.
 *
 *   assume_owner is the fallback for the window before the first UPD_SESSION
 *   arrives: once have_session is true, session.is_owner is authoritative and
 *   the inference is ignored.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdbool.h>
#include <stdint.h>

#include "libtetrissh/tetrissh.h"
#include "libtetrisutil/gamestate.h"
#include "libtetrisutil/historyview.h"
#include "libtetrisutil/sessionstate.h"

/* Where this client believes it is. Distinct from SessionPhase, which is the
 * server's view: these are the states the UI actually branches on, and some of
 * them (CONNECTED vs WAITING) the server never reports. */
typedef enum
{
    CLI_DISCONNECTED = 0,
    CLI_CONNECTED, /* handshake done, not in a room                     */
    CLI_WAITING,   /* JOIN sent; waiting for the owner to START         */
    CLI_PLAYING,   /* first UPD_GAME seen - the game is running         */
    CLI_GAME_OVER,
} ClientPhase;

/* Which of the three libtetrisauth methods a reply is answering, or
 * AUTH_NONE if none is in flight. Two jobs (#57): it scopes the budget to
 * auth responses (a 401 answering JOIN would be a server bug, not a bad
 * password), and it is the bit that tells "409, name taken" from "409,
 * already authenticated" apart - the status line alone cannot, and the
 * server sends no body to say. Single-slot: safe only because the auth
 * screens are modal and never have two auth requests outstanding at once. */
typedef enum
{
    AUTH_NONE = 0,
    AUTH_LOGIN,
    AUTH_REGISTER,
    AUTH_GUEST,
} AuthMethod;

/* What client_service() folded into the Client on this call. */
typedef enum
{
    CLI_EV_NONE = 0,   /* nothing usable arrived (bad frame, partial, etc.) */
    CLI_EV_GAME,       /* game was updated from an UPD_GAME push            */
    CLI_EV_SESSION,    /* session was updated from an UPD_SESSION push      */
    CLI_EV_RESULT,     /* the round ended; see last_winner                  */
    CLI_EV_REJECT,     /* the server refused a command; see last_reject     */
    CLI_EV_DISCONNECT, /* transport is dead; the caller must stop           */
    CLI_EV_HISTORY,    /* UPD_HISTORY arrived; see Client.history           */
    CLI_EV_AUTH_OK, /* LOGIN/REGISTER/GUEST succeeded (2xx); see auth_reply */
    /* net_service() answered an in-flight auth request but has not yet
     * decided success/failure or counted it. client_service() folds this into
     * CLI_EV_AUTH_OK or CLI_EV_REJECT via fold_auth_reply() before returning,
     * so it NEVER escapes client_service() and no caller ever sees it. It
     * exists to keep the counting/2xx-split logic in one place, off the
     * receive path. */
    CLI_EV_AUTH_REPLY,
} ClientEvent;

typedef struct
{
    /* --- transport --- */
    int fd;       /* connected socket, or -1 when disconnected     */
    session_t sh; /* libtetrissh session (valid while fd >= 0)     */

    /* --- what we believe --- */
    ClientPhase phase;
    int room_req;      /* the room id we ASKED for; 0 = "a new one"  */
    bool assume_owner; /* inferred: room_req == 0 => we created it,
                        * so we are its first member and its owner.
                        * Wrong if the id we named did not exist -
                        * the server creates it and we own it after
                        * all. Harmless: the server discards START
                        * from a non-owner either way.              */

    /* --- what the server told us, if it ever does --- */
    SessionState session; /* meaningful only while have_session        */
    bool have_session;    /* true once a real UPD_SESSION has arrived  */

    /* --- game state, pushed at 20 Hz once playing --- */
    GameState game;
    bool have_game;

    /* Last refusal from the server, as an htttp status (403 not the owner,
     * 409 conflict, 429 full). 0 = nothing refused since it was last read.
     * Refusals arrive as RESPONSES, not pushed requests - they answer a
     * specific command - so client_service parses both shapes. */
    int last_reject;
    int last_winner; /* player_id from UPD_RESULT; -1 = none yet */

    /* The reply to the most recent client_history(), from UPD_HISTORY.
     * have_history is false until the first reply arrives, so screen_history()
     * can tell "no reply yet" from "the reply says EMPTY" - the latter is a
     * real answer, the former is still waiting on the network. */
    player_history_t history;
    bool have_history;

    unsigned frames_seen; /* diagnostic: proves pushes are flowing, and
                           * separates "the network is dead" from "the
                           * renderer is slow" without a debugger      */

    /* --- auth, a property of the CONNECTION, not of a screen (#57) ---
     *
     * tetrisu is not one connection the way a session process is: one
     * tetrisu is one USER, and screen_connect()/the reconnect loop (#61)
     * both call client_connect() more than once in that user's lifetime. A
     * file-static counter, mirroring the server's shape, would survive a
     * connection the server has already forgotten. So this lives here, on
     * Client, and the invariant "no connection implies zero" is enforced on
     * BOTH edges: client_connect()'s memset zeroes it below, and
     * client_disconnect() plus the CLI_EV_DISCONNECT arm in client_service()
     * clear it again on the way out - so a lingering Client never outlives
     * the connection it counted on. */

    /** Refused auth replies seen on this connection. Cosmetic only: it feeds
     * the warning line in screen_auth.c and nothing branches on it, so it may
     * lag the server's own count without any correctness consequence.
     * Survives leaving the auth screens and coming back. */
    int auth_failures;

    /** The last auth reply was a refusal, so a hangup arriving now is most
     * likely the server spending its attempt cap. Consumed (and cleared) by
     * client_service() on every call. */
    bool auth_refused_last;

    /** Which auth method is awaiting a response, AUTH_NONE if none. net.c is
     * the only writer, set at send time - there is no correlation on this
     * wire, so the in-flight method cannot be recovered from last_reject. */
    AuthMethod auth_pending;

    /** The method the last refusal or success answered, so a screen can still
     * read it once client_service() has cleared auth_pending. */
    AuthMethod auth_reply;

    /** Latched true only when the call returning CLI_EV_DISCONNECT finds
     * auth_refused_last set - i.e. this disconnect is most likely cap
     * exhaustion, not a dead route. Meaningless once phase !=
     * CLI_DISCONNECTED; main()'s reconnect loop (#61) is the only reader. */
    bool reconnect_ok;

    /* Receive buffer. Lives HERE, not on the stack of client_service():
     * htttp parses zero-copy, so req.body points into this array and must
     * stay valid for as long as the parsed message is used. */
    uint8_t rxbuf[SESSION_MAX_FRAME];
} Client;

/* --- lifecycle ---------------------------------------------------------- */

/*
 * Returned by client_connect when the TCP connection itself could not be made.
 *
 * Deliberately NOT SESSION_ERR_IO: libtetrissh returns that for any I/O
 * failure, including a peer that accepts and then closes partway through the
 * handshake. Reusing SESSION_ERR_IO here made the UI report "no route to
 * server" about a server it had just successfully connected to. Kept outside
 * the SESSION_ERR_* range so it cannot collide.
 */
#define CLIENT_ERR_CONNECT (-100)

/* Real backend: TCP connect + tetrissh handshake. Returns 0 on success,
 * CLIENT_ERR_CONNECT if the socket never came up, or the negative
 * SESSION_ERR_* from the handshake so the caller can tell a rejected
 * certificate from a peer that hung up. */
int client_connect(Client *c, const char *ip, int port, const char *ca_path);

void client_disconnect(Client *c);

/* The fd to hand to poll(). -1 when there is no connection, which poll()
 * ignores. */
int client_fd(const Client *c);

/* --- commands (client -> server) ---------------------------------------- */
/* All return 0 on success, -1 if the frame could not be sent. Arguments go in
 * the body as ONE RAW BYTE, matching the server's body_byte() in session.c. */

int client_join(Client *c, uint8_t room_id); /* 0 = create a new room */
int client_leave(Client *c);
int client_start(Client *c);
int client_move(Client *c, int right);   /* 0 left,  1 right      */
int client_rotate(Client *c, int right); /* 0 left,  1 right      */
int client_drop(Client *c, int hard);    /* 0 soft,  1 hard       */
int client_hold(Client *c);

/*
 * Asks for the connected player's own history. 0 = sent, -1 = could not send.
 * The reply arrives later as CLI_EV_HISTORY from client_service() (see
 * Client.history) - poll for it, never block on it, so a server that never
 * answers cannot hang the screen that asked.
 */
int client_history(Client *c);

/* --- auth (client -> server, pre-gate) ----------------------------------- */
/*
 * The three libtetrisauth methods (#55, #50). Body is "username\npassword"
 * for LOGIN/REGISTER, empty for GUEST. Sets auth_pending so client_service()
 * can fold the reply; returns 0 if the frame was sent, -1 if it could not be.
 * The reply itself arrives later as CLI_EV_AUTH_OK or CLI_EV_REJECT from
 * client_service() - poll for it, never block on it, or a server that never
 * answers hangs the screen.
 */
int client_login(Client *c, const char *username, const char *password);
int client_register(Client *c, const char *username, const char *password);
int client_guest(Client *c);

/* --- receive (server -> client) ----------------------------------------- */

/*
 * Read and fold in exactly one frame. Call only when poll() reports the fd
 * readable. Never blocks longer than one frame.
 */
ClientEvent client_service(Client *c);

/* Human-readable phase, for the status bar. Never returns NULL. */
const char *client_phase_str(ClientPhase p);

#endif /* TETRISU_CLIENT_H */
