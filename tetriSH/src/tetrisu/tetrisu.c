/**
 * @file tetrisu.c
 * @brief Terminal game client: entry point and lobby state machine.
 *
 * Mirror of a server session: connects to tetrisd, runs the tetrissh
 * handshake, then sends the player's commands as htttp requests and receives
 * the server's UPD_GAME / UPD_SESSION pushes (which are also requests - see
 * htttp.h; both ends parse and both ends serialize).
 *
 * The work is split so that none of it needs a terminal to be correct:
 *   client.h / net.c  - transport and state, no UI, no ncurses
 *   screens.c         - the lobby UI, talks only to a Client
 *   render.c          - draws one GameState, no networking
 *   game_screen.c     - the gameplay loop, also talks only to a Client
 *   this file         - argument handling and the screen state machine
 *
 */

#include <limits.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libtetrisutil/rc.h"
#include "libtetrisui/tetrisui.h"
#include "tetrisu/client.h"
#include "tetrisu/game_screen.h"
#include "tetrisu/screens.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 5555 /* listen_port, when .tetrishrc is silent */
/* ca_path, when .tetrishrc is silent. Relative to the project root, which is
 * where the client is run from. */
#define DEFAULT_CA_PATH "auth/cacsertificate.crt"

/* port is the configured default, not a constant: .tetrishrc owns it, so the
 * help text has to be told what it currently is. */
static void usage(const char *argv0, int port)
{
    fprintf(stderr,
            "usage: %s [server-ip] [port]\n"
            "\n"
            "  no arguments      connect to %s:%d\n",
            argv0, DEFAULT_HOST, port);
}

/* One yes/no prompt, offered only when reconnect_ok says this disconnect was
 * cap-exhaustion rather than a dead route (#61). */
static bool offer_reconnect(void)
{
    return tetrisui_confirm(
               "Disconnected",
               "The server closed the connection after too many failed "
               "login attempts. Reconnect?") != 0;
}

static void handle_sigint(int signo)
{
    (void)signo;
    tetrisui_shutdown();
    _Exit(128 + SIGINT);
}

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST;

    /* The server's own directives, read by the client on purpose: a checkout
     * is one deployment, and a client carrying its own port and CA would be a
     * second place to change them. Before the argument scan, because an
     * explicit [port] overrides listen_port. */
    char ca_path[PATH_MAX];
    (void)rc_get("ca_path", DEFAULT_CA_PATH, ca_path, sizeof ca_path);

    int port;
    (void)rc_get_int("listen_port", DEFAULT_PORT, 1, 65535, &port);

    if (argc >= 2 && strcmp(argv[1], "--help") == 0)
    {
        usage(argv[0], port);
        return 0;
    }

    if (argc >= 2)
        host = argv[1];
    /* The check belongs inside the argc test, not after it: a bad listen_port
     * is already refused above, and reporting it as argv[2] would name an
     * argument nobody passed. */
    if (argc >= 3)
    {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535)
        {
            fprintf(stderr, "bad port: %s\n", argv[2]);
            return 1;
        }
    }

    /* A dead server socket must not kill us with SIGPIPE inside session_send.
     */
    signal(SIGPIPE, SIG_IGN);

    Client c;
    memset(&c, 0, sizeof c);

    tetrisui_init();
    signal(SIGINT, handle_sigint);

    ScreenResult r = screen_connect(&c, host, port, ca_path);

    /*
     * One outer iteration per CONNECTION; the auth gate and the lobby state
     * machine both live inside it, because reconnecting (#61) means
     * re-authenticating too - a fresh connection is unauthenticated again.
     * frames_seen and the auth counters are properties of the connection,
     * not the program: client_connect()'s memset wipes them on every pass, so
     * total_frames_seen accumulates what is about to be erased. The exit code
     * needs no equivalent - see below.
     */
    unsigned total_frames_seen = 0;
    for (;;)
    {
        /* The auth gate (#50): mode choice -> credential form -> result. It
         * expects a Client that is CLI_CONNECTED, which is what
         * screen_connect() leaves behind. */
        if (r == SCR_OK)
            r = screen_auth(&c);

        /*
         * Lobby state machine.
         *
         * Each screen returns what happened rather than leaving main to
         * re-derive it from Client.phase, so every transition is readable in
         * one place.
         *
         *   screen_main_menu   OK           JOIN sent; fall through to the wait
         *                      BACK         join cancelled; draw the menu again
         *                      QUIT         leave the loop, exit 0
         *
         *   screen_wait_start  GAME_START   the room started; hand to
         * screen_game BACK         left the room, or JOIN/START was refused -
         * the screen has already reported the 403/409/429 itself
         *
         *   screen_game        OK           the round finished; report the
         * winner BACK         player pressed q; LEAVE already sent
         *
         * Any screen may also return DISCONNECTED, which ends the loop.
         * QUIT and DISCONNECTED are the ONLY results that end it; everything
         * else comes back round to the menu. (screen_auth() itself may also
         * hand the loop a QUIT or DISCONNECTED before the menu is ever
         * drawn - the while condition below covers that for free.)
         *
         * Rounds loop INSIDE the room (the inner while below), because a
         * finished round does not take us out of it: the server's
         * handle_gameover resets the room to SESSION_WAITING and keeps every
         * member, so the owner can START again. Falling back to the menu
         * instead would offer only JOIN, which the server then refuses with 409
         * - we are already in a room. The way out of a room is 'q' in the wait
         * screen, which sends LEAVE.
         */
        while (r != SCR_QUIT && r != SCR_DISCONNECTED)
        {
            r = screen_main_menu(&c);
            if (r != SCR_OK)
                continue; /* BACK -> menu again, QUIT -> exit */

            r = screen_wait_start(&c);
            while (r == SCR_GAME_START)
            {
                r = screen_game(&c);

                /* SCR_OK means the round finished rather than that the player
                 * walked out, so it is the only case with a result to report.
                 * SCR_BACK and SCR_DISCONNECTED belong to the outer loop, which
                 * sends us to the menu or exits. */
                if (r != SCR_OK)
                    break;

                /*
                 * Only announce a round we actually have a RESULT for.
                 *
                 * last_winner < 0 means no UPD_RESULT arrived, and the server
                 * only withholds one when the round did not end for us - most
                 * obviously when we left the room, which takes us out of the
                 * membership before room_check_round_end runs. Announcing "the
                 * round ended" there is a lie: it ended for the players who
                 * stayed, not for us, and it stacks a meaningless popup in
                 * front of whatever the next screen has to say (e.g. the 409
                 * from trying to rejoin a room that is still playing).
                 *
                 * No result, nothing to report - fall through to the wait
                 * screen.
                 */
                if (c.last_winner >= 0)
                {
                    char who[64];
                    snprintf(who, sizeof who, "Player %d wins.", c.last_winner);
                    const char *lines[] = {who, "", "Back to the room."};
                    tetrisui_message("Round over", lines, 3);
                    /* Clear it, or the next round shows this round's winner if
                     * that round's UPD_RESULT goes missing. */
                    c.last_winner = -1;
                }

                /*
                 * Wait for the next START rather than for a JOIN the server
                 * would refuse - but only while we are actually still a member.
                 *
                 * A round can also end for us by our leaving it, and the server
                 * then reports room_id = ROOM_NONE. Waiting on a room we are no
                 * longer in would sit there forever: no START will ever be
                 * broadcast to us. Back to the menu instead, which is the only
                 * place that can put us in a room again.
                 */
                if (c.have_session && c.session.room_id == ROOM_NONE)
                    break;

                r = screen_wait_start(&c);
            }
        }

        /*
         * Offer a reconnect only when THIS disconnect was cap-exhaustion
         * (#61) - reconnect_ok is set by client_service() exactly when the
         * event it is about to return is a POLLHUP immediately behind a
         * counted auth 401, which is the only disconnect a fresh connection
         * can plausibly fix. Every other DISCONNECTED (a dead route, the
         * server going away mid-game) falls straight through and exits, as
         * it always has.
         */
        if (r != SCR_DISCONNECTED || !c.reconnect_ok)
            break;

        total_frames_seen += c.frames_seen; /* about to be memset away */
        if (!offer_reconnect())
            break;

        client_disconnect(&c); /* close the dead fd first */
        r = screen_connect(&c, host, port, ca_path);
    }

    ScreenResult final = r;
    client_disconnect(&c);
    tetrisui_shutdown();

    /*
     * frames_seen and the exit code are properties of the CURRENT
     * connection, not the program - client_connect()'s memset wipes them on
     * every reconnect (#61). total_frames_seen
     * accumulates what memset is about to erase; the exit code needs no
     * equivalent because SCR_DISCONNECTED only reaches here when no further
     * reconnect was offered, accepted, or possible.
     *
     * Printed after endwin() so it survives on the real terminal. frames_seen
     * is the quickest proof that the push path actually ran.
     */
    total_frames_seen += c.frames_seen;
    if (final == SCR_DISCONNECTED)
        fprintf(stderr, "tetrisu: disconnected (%u game frames received)\n",
                total_frames_seen);
    else if (total_frames_seen > 0)
        printf("tetrisu: %u game frames received\n", total_frames_seen);

    return final == SCR_DISCONNECTED ? 1 : 0;
}
