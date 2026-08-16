/*
 * test_render_input.c - play the real engine from the keyboard, no networking.
 *
 * The difference from tests/test_gameui.c is the clock. That one runs gravity
 * off a wall-clock timer, so the game moves whether you touch it or not, and
 * two runs are never quite the same. Here gravity is a keystroke: 't' is the
 * only thing that calls tetrisbrain_tick(). Nothing happens unless you press
 * something, which makes it possible to sit on a single frame and study it, to
 * count ticks up to the gravity trigger by hand, and to reproduce a sequence
 * exactly when reporting a bug.
 *
 * Everything goes through the public brain API - tetrisbrain_init/input/tick.
 * The harness never writes to GameState itself.
 *
 * The status bar reports whether the last key actually changed anything: the
 * whole GameState is compared before and after, so a refused move shows as
 * "no change" instead of looking like a missed keypress.
 *
 * Build:  make gui
 * Run:    ./bin/test_render_input           seed 0 (reproducible)
 *         ./bin/test_render_input 12345     pick a seed
 *
 * Keys:
 *   a / <-    move left            s / v     soft drop
 *   d / ->    move right           space     hard drop
 *   z         rotate CCW           c         hold
 *   x / ^     rotate CW            t         ONE gravity tick
 *   r         restart              q         quit
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tetrisu/render.h"
#include "libtetrisutil/gamestate.h"
#include "libtetrisbrain/tetrisbrain.h"

#define MIN_ROWS (BOARD_HEIGHT + 5)
#define MIN_COLS RENDER_MIN_COLS

static const char *KINDCH = "?IOTSZJL";

int main(int argc, char **argv)
{
    unsigned seed = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 10) : 0u;

    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    /* No timeout(): getch blocks. Gravity is manual, so there is nothing to
     * poll for and no reason to spin. */

    if (LINES < MIN_ROWS || COLS < MIN_COLS)
    {
        int rows = LINES, cols = COLS;
        endwin();
        fprintf(stderr, "terminal too small: have %dx%d, need at least %dx%d\n",
                rows, cols, MIN_ROWS, MIN_COLS);
        return 1;
    }

    GameState g;
    memset(&g, 0, sizeof g);
    tetrisbrain_init(&g, seed);

    const char *last = "(start)";
    const char *effect = "";

    for (;;)
    {
        render_game(&g);

        /* Status bar. tick_count/tick_trigger is the interesting number when
         * gravity is manual: press 't' that many times and the piece drops. */
        char bar[256];
        snprintf(bar, sizeof bar,
                 "seed %u | tick %d/%d | %c hold=%c next=%c | %s %s", seed,
                 g.tick_count, g.tick_trigger, KINDCH[g.active.kind],
                 KINDCH[g.hold], KINDCH[g.next[0]], last, effect);
        mvaddnstr(LINES - 1, 0, bar, COLS - 1);
        clrtoeol();

        if (g.game_over)
            mvaddnstr(LINES - 2, 0, "GAME OVER - r to restart, q to quit",
                      COLS - 1);
        refresh();

        int ch = getch();
        if (ch == 'q')
            break;

        if (ch == 'r')
        {
            memset(&g, 0, sizeof g);
            tetrisbrain_init(&g, seed);
            last = "restart";
            effect = "";
            continue;
        }

        /* Snapshot so we can tell a refused move from an unread keypress. */
        GameState before = g;

        switch (ch)
        {
        case 'a':
        case KEY_LEFT:
            tetrisbrain_input(&g, MOVE_LEFT);
            last = "LEFT";
            break;
        case 'd':
        case KEY_RIGHT:
            tetrisbrain_input(&g, MOVE_RIGHT);
            last = "RIGHT";
            break;
        case 'z':
            tetrisbrain_input(&g, MOVE_ROT_LEFT);
            last = "ROT_CCW";
            break;
        case 'x':
        case KEY_UP:
            tetrisbrain_input(&g, MOVE_ROT_RIGHT);
            last = "ROT_CW";
            break;
        case 's':
        case KEY_DOWN:
            tetrisbrain_input(&g, MOVE_SOFT_DROP);
            last = "SOFT_DROP";
            break;
        case ' ':
            tetrisbrain_input(&g, MOVE_HARD_DROP);
            last = "HARD_DROP";
            break;
        case 'c':
            tetrisbrain_input(&g, HOLD);
            last = "HOLD";
            break;
        case 't':
            tetrisbrain_tick(&g);
            last = "TICK";
            break;
        default:
            last = "(unbound key)";
            break;
        }

        effect =
            (memcmp(&before, &g, sizeof g) == 0) ? "-> no change" : "-> ok";
    }

    endwin();
    return 0;
}
