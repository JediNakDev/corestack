/**
 * @file game_screen.c
 * @brief Live gameplay screen (ncurses board renderer + real-time * loop).
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <ncurses.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>

#include "tetrisu/game_screen.h"
#include "tetrisu/render.h" /* render_game */

/* Redraw cadence when nothing arrives. The server pushes at 20 Hz, so this
 * only bounds how long a dead link or a stale footer goes unnoticed. */
#define GAME_POLL_MS 100

/*
 * Map a key to a command. Returns true if the player asked to leave.
 *
 * Sends through the client_* wrappers rather than a private copy of the send
 * layer, so the wire format is defined in exactly one place (net.c).
 */
static bool handle_key(Client *c, int ch, bool alive)
{
    if (ch == 'q' || ch == 'Q')
        return true;

    /* Once our board is dead the server ignores gameplay methods anyway
     * (session_dispatch gates them on SESSION_PLAYING), so there is nothing to
     * gain by sending them - and the screen is now a spectator waiting for the
     * room's result. */
    if (!alive)
        return false;

    switch (ch)
    {
    case 'a':
    case KEY_LEFT:
        client_move(c, 0);
        break;
    case 'd':
    case KEY_RIGHT:
        client_move(c, 1);
        break;
    case 'z':
        client_rotate(c, 0);
        break;
    case KEY_UP:
        client_rotate(c, 1);
        break;
    case 's':
    case KEY_DOWN:
        client_drop(c, 0);
        break;
    case ' ':
        client_drop(c, 1);
        break;
    case 'c':
        client_hold(c);
        break;
    default:
        break;
    }
    return false;
}

/*
 * One line under the board saying what the screen is doing.
 *
 * Worth the space because "your board stopped updating" is ambiguous on its
 * own: topping out and the link dying look identical otherwise, and a dead
 * player still has to sit there until the last opponent finishes.
 */
static void draw_footer(bool alive)
{
    int row = getmaxy(stdscr) - 1;
    move(row, 0);
    clrtoeol();
    if (alive)
        mvprintw(row, 0,
                 "a/d move  z/UP rotate  s soft  SPACE hard  c hold  q leave");
    else
        mvprintw(row, 0,
                 "Topped out - waiting for the other players.  q leave");
}

ScreenResult screen_game(Client *c)
{
    ScreenResult result = SCR_BACK;
    bool alive = true; /* our board is still playing */

    curs_set(0);
    keypad(stdscr, TRUE);
    /*
     * nodelay: getch() must return ERR rather than block, because poll() - not
     * ncurses - owns the waiting here. Cleared on every exit path below; if it
     * leaked out, the next modal screen would spin at 100% CPU.
     */
    nodelay(stdscr, TRUE);

    /* render_game() does its own erase() and refresh(); the footer is drawn
     * after it and needs a second refresh to reach the terminal. */
    render_game(&c->game);
    draw_footer(alive);
    refresh();

    for (;;)
    {
        struct pollfd pfds[2];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = client_fd(c);
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, GAME_POLL_MS);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            result = SCR_DISCONNECTED;
            goto done;
        }

        if (pfds[0].revents & POLLIN)
        {
            int ch;
            /* Drain: one poll wake-up can cover several buffered keys, and a
             * single getch() would leave the rest until the next wake-up. At
             * 20 Hz that is the difference between responsive and mushy. */
            while ((ch = getch()) != ERR)
            {
                if (handle_key(c, ch, alive))
                {
                    client_leave(c);
                    result = SCR_BACK;
                    goto done;
                }
            }
        }

        if (pfds[1].revents & POLLIN)
        {
            ClientEvent ev = client_service(c);
            switch (ev)
            {
            case CLI_EV_DISCONNECT:
                result = SCR_DISCONNECTED;
                goto done;

            case CLI_EV_RESULT:
                /*
                 * The room is over - every player topped out and the
                 * server ranked them. This is the ONLY reliable exit:
                 * our own game_over just means we are done, not that the
                 * round is (room.c handle_gameover waits for the last
                 * member before broadcasting ADMIN_RESULT).
                 */
                result = SCR_OK;
                goto done;

            case CLI_EV_GAME:
                /* client_service flips phase to CLI_GAME_OVER when the
                 * pushed board says so. The server then stops sending
                 * UPD_GAME, so this is the last frame we will see. */
                if (c->phase == CLI_GAME_OVER)
                    alive = false;
                break;

            case CLI_EV_SESSION:
                /*
                 * The round ended without us seeing UPD_RESULT - the
                 * server pushes UPD_SESSION(WAITING) as part of the same
                 * reset. Leaving on it too means a dropped or reordered
                 * result frame cannot strand us on a frozen board, which
                 * is exactly the bug this screen used to have.
                 */
                if (c->have_session && c->session.phase != SESSION_PLAYING)
                {
                    result = SCR_OK;
                    goto done;
                }
                break;

            case CLI_EV_REJECT:
                /* Nothing we send mid-game is refusable, so this is not
                 * ours to report; drop it rather than let it accumulate
                 * and surface on some later screen. */
                c->last_reject = 0;
                break;

            case CLI_EV_NONE:
                break;

            case CLI_EV_AUTH_OK:
            case CLI_EV_AUTH_REPLY:
                /* Nothing sends LOGIN/REGISTER/GUEST mid-game - the auth
                 * screens (#50) run once, before this one is ever reached,
                 * and CLI_EV_AUTH_REPLY never escapes client_service() in
                 * the first place (#62). Listed rather than defaulted so a
                 * future ClientEvent value fails this build, not silently
                 * falls through. */
                break;

            case CLI_EV_HISTORY:
                /* Nothing sends HISTORY mid-game - screen_history() runs
                 * from the lobby, and the server refuses HISTORY with 409
                 * while SESSION_PLAYING regardless. */
                break;
            }
        }

        if (pfds[1].revents & (POLLHUP | POLLERR))
        {
            result = SCR_DISCONNECTED;
            goto done;
        }

        render_game(&c->game);
        draw_footer(alive);
        refresh();
    }

done:
    nodelay(stdscr, FALSE); /* restore blocking input for the modal screens */
    return result;
}
