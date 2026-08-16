#ifndef TETRISU_SCREENS_H
#define TETRISU_SCREENS_H

/**
 * @file screens.h
 * @brief The lobby UI, everything up to the moment the game starts.
 *
 * Screens read a Client and call client_* on it. They never touch a socket, so
 * the wire format stays in net.c and out of the UI.
 *
 * Two flavours of screen live here, and the difference matters:
 *
 *   - BLOCKING screens (connect, main menu, join). They use libtetrisui's modal
 *     widgets and sit in wgetch(). Safe because the server cannot send us
 *     anything before we join a room: every push path in tetrisd requires
 *     either room membership or SESSION_PLAYING.
 *
 *   - The WAIT screen. Once JOIN is sent the server may push at any moment,
 *     and for a non-owner the arrival of UPD_GAME is the ONLY notification
 *     that the game began. It therefore runs its own poll() loop with ncurses
 *     in nodelay mode and must not call any libtetrisui modal.
 */

#include "tetrisu/client.h"

/* Result of a lobby screen, so main() can drive the state machine without
 * inspecting Client.phase and re-deriving what just happened. */
typedef enum
{
    SCR_QUIT = 0,     /* user asked to leave the program        */
    SCR_BACK,         /* user cancelled; go up one screen       */
    SCR_OK,           /* screen completed normally              */
    SCR_GAME_START,   /* the game is running - hand off to Phase 3 */
    SCR_DISCONNECTED, /* transport died                         */
    SCR_AUTH_OK,      /* authenticated (LOGIN/REGISTER/GUEST), carry on.
                       * One value for all three methods (#50): the state
                       * machine in tetrisu.c does not branch on which one
                       * got you here. */
} ScreenResult;

/*
 * Connect + handshake, with a real progress panel. host/port are seeded from
 * the command line and editable. On SESSION_ERR_AUTH the message distinguishes
 * a rejected certificate from an unreachable server - the reason libtetrisui
 * gained tetrisui_progress_step().
 */
ScreenResult screen_connect(Client *c, const char *host, int port,
                            const char *ca_path);

/*
 * The auth gate: mode choice (LOGIN/REGISTER/GUEST) -> credential form ->
 * result (#50). Blocking-modal like the rest of this file, EXCEPT that the
 * wait for a reply is poll-shaped, not a blocking session_recv(), so a server
 * that never answers cannot hang the mode-choice screen. Called once, right
 * after screen_connect() leaves the client CLI_CONNECTED.
 * Returns SCR_AUTH_OK, SCR_QUIT (user backed out before authenticating) or
 * SCR_DISCONNECTED.
 */
ScreenResult screen_auth(Client *c);

/* Create room / join by id / quit. */
ScreenResult screen_main_menu(Client *c);

/*
 * Send HISTORY, wait for the reply, and show it. Blocking-modal like the rest
 * of this file (client_history() is reachable only from the lobby, and the
 * server pushes nothing unsolicited before JOIN - see the file comment).
 * Returns SCR_OK (back to the lobby) or SCR_DISCONNECTED.
 */
ScreenResult screen_history(Client *c);

/* Ask for a room id and send JOIN. room_id 0 creates a new room. */
ScreenResult screen_join_room(Client *c);

/*
 * Sit in the room until the game starts, the user leaves, or the link dies.
 * The only poll-aware screen; see the note above.
 */
ScreenResult screen_wait_start(Client *c);

#endif /* TETRISU_SCREENS_H */
