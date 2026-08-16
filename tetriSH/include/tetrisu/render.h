#ifndef TETRISU_RENDER_H
#define TETRISU_RENDER_H

/*
 * render.h - ncurses board renderer for tetrisu.
 *
 * Pure rendering: given a GameState, draw it. No networking, so it can be
 * exercised on its own (see tests/test_render_fixtures.c). Assumes ncurses is
 * already initialised (initscr) by the caller; colours are set up on first
 * use. Plain -lncurses is enough - nothing here needs wide characters or a
 * particular locale.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisutil/gamestate.h"

/*
 * Screen columns per board cell.
 *
 * Two, because a terminal character is roughly twice as tall as it is wide, so
 * a two-column block comes out about square. Three columns would let the
 * empty-cell dot sit in a true middle column, but it stretches every block
 * into a landscape rectangle and the whole board reads as distorted - not
 * worth it for the sake of one dot.
 */
#define RENDER_CELL_W 2

/* Columns the board occupies on screen, including both side walls. */
#define RENDER_BOARD_COLS (BOARD_WIDTH * RENDER_CELL_W + 2)

/*
 * Smallest terminal render_game() can draw into without clipping.
 *
 * Board, a gap, then two side columns: stats/HOLD/NEXT, and the room
 * scoreboard to their right. Defined here so every caller sizes its check off
 * the renderer's actual layout - the harnesses used to each carry their own
 * copy of this arithmetic, which silently went stale the moment the layout
 * changed.
 */
#define RENDER_STATS_COLS 14
#define RENDER_STANDINGS_COLS 24 /* id, name, score, lines */
#define RENDER_MIN_COLS                                                        \
    (RENDER_BOARD_COLS + 4 + RENDER_STATS_COLS + RENDER_STANDINGS_COLS)

/* Board rows, plus the origin, the floor, a gap and the GAME OVER banner. */
#define RENDER_MIN_ROWS (BOARD_HEIGHT + 5)

/* Draw the whole game state: board (buffer + main), active piece overlay, and
 * the HOLD / NEXT / score panels. */
void render_game(const GameState *g);

#endif /* TETRISU_RENDER_H */
