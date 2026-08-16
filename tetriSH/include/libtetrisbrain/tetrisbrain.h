#ifndef TETRISBRAIN_H
#define TETRISBRAIN_H

/*
 * libtetrisbrain - pure Tetris rules engine.
 *
 * No I/O, no threads, no clock. tetrisd drives it: init once, then feed it
 * gravity ticks and player inputs. Every call just mutates GameState.
 *
 *     GameState g;
 *     tetrisbrain_init(&g, seed);
 *     ... each ~50ms tick:  tetrisbrain_tick(&g);
 *     ... on player action: tetrisbrain_input(&g, move);
 *     ... stop when tetrisbrain_game_over(&g);
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/model.h"

/* Start a new game: clear the board, seed the piece stream, fill the queue,
 * and spawn the first piece. */
void tetrisbrain_init(GameState *g, unsigned seed);

/* Advance one gravity tick. Falls / locks / spawns as the timer dictates. */
void tetrisbrain_tick(GameState *g);

/* Apply one player action (move, rotate, drop, hold). */
void tetrisbrain_input(GameState *g, Move m);

/* True once the board has topped out. */
bool tetrisbrain_game_over(const GameState *g);

/*
 * Where the active piece would land if hard-dropped right now, without moving
 * it. Pure query - the state is not touched. Intended for drawing a landing
 * preview ("ghost"), so the player can see the result of a hard drop before
 * committing to it. Returns a PIECE_NONE piece when there is nothing falling.
 */
Piece tetrisbrain_ghost(const GameState *g);

#endif /* TETRISBRAIN_H */
