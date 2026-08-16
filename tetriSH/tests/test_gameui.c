/*
 * test_gameui.c - standalone GUI test for the tetrisu renderer.
 *
 * No networking: it drives libtetrisbrain locally (single-player) and feeds
 * each frame's GameState to render_game(). Lets you eyeball the board, colours,
 * active piece, NEXT/HOLD panels and score - and doubles as a playable demo.
 *
 * Build (links only the renderer + the brain + ncurses, no tetrissh/htttp):
 *   gcc -Wall -Wextra -Iinclude tests/test_gameui.c src/tetrisu/render.c \
 *       src/libtetrisbrain/[all].c -lncurses -o bin/test_gameui
 * Run:
 *   ./bin/test_gameui     (a/d or arrows move, z/up rotate, s/down soft,
 *                          space hard drop, c hold, q quit)
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <ncurses.h>
#include <time.h>

#include "tetrisu/render.h"
#include "libtetrisbrain/tetrisbrain.h"
#include "libtetrisutil/gamestate.h"

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void)
{
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(10); /* getch() waits up to 10ms, then returns ERR */

    GameState g;
    tetrisbrain_init(&g, (unsigned)time(NULL));

    long last_tick = now_ms();
    for (;;)
    {
        int c = getch();
        if (c == 'q')
            break;
        switch (c)
        {
        case 'a':
        case KEY_LEFT:
            tetrisbrain_input(&g, MOVE_LEFT);
            break;
        case 'd':
        case KEY_RIGHT:
            tetrisbrain_input(&g, MOVE_RIGHT);
            break;
        case 'z':
            tetrisbrain_input(&g, MOVE_ROT_LEFT);
            break;
        case KEY_UP:
            tetrisbrain_input(&g, MOVE_ROT_RIGHT);
            break;
        case 's':
        case KEY_DOWN:
            tetrisbrain_input(&g, MOVE_SOFT_DROP);
            break;
        case ' ':
            tetrisbrain_input(&g, MOVE_HARD_DROP);
            break;
        case 'c':
            tetrisbrain_input(&g, HOLD);
            break;
        default:
            break;
        }

        /* gravity: one brain tick per ~50ms (20 ticks/sec) */
        long t = now_ms();
        if (t - last_tick >= 50)
        {
            tetrisbrain_tick(&g);
            last_tick = t;
        }

        render_game(&g);
    }

    endwin();
    return 0;
}
