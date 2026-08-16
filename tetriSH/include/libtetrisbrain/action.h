#ifndef TETRISBRAIN_ACTION_H
#define TETRISBRAIN_ACTION_H

/*
 * action.h - moving and rotating the active piece.
 *
 * Two layers:
 *   - pure transforms (action_move / action_rotate): produce a *candidate*
 *     Piece. They never read the board and never touch GameState.
 *   - check-and-commit handlers (action_try_*): build a candidate, test it
 *     with board_collides, and write it to g->active only if it is legal.
 *
 * Pure game logic. Mapping keys / network input to these calls belongs in a
 * separate input layer, not here.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/model.h"

/* ---- pure transforms (candidate producers) ---- */

/* Rotate one step: dir > 0 = clockwise (N->E->S->W), dir < 0 = counter-cw.
 * Only the rotation index changes; kind and position are untouched. */
Piece action_rotate(Piece p, int dir);

/* Shift by (dx, dy): +dx right, +dy down. */
Piece action_move(Piece p, int dx, int dy);

/* ---- check-and-commit handlers (update g->active iff legal) ---- */

/* Try to shift the active piece by (dx, dy). Applies it only if the result
 * does not collide. Returns true if applied, false if blocked. */
bool action_try_move(GameState *g, int dx, int dy);

/* Try to rotate the active piece one step, with horizontal wall kicks:
 * if the rotation collides, retry shifted right then left by 1..box-size.
 * Applies the first legal candidate. Returns true if applied, false if blocked.
 */
bool action_try_rotate(GameState *g, int dir);

/* ---- named input actions (one per game input) ----
 * Thin operations the input layer calls. The return value reports whether the
 * input had any effect. */

bool action_lmove(GameState *g); /* move left                          */
bool action_rmove(GameState *g); /* move right                         */
bool action_rrot(GameState *g);  /* rotate clockwise                   */
bool action_lrot(GameState *g);  /* rotate counter-clockwise           */

bool action_softdrop(
    GameState *g); /* fall one row; locks on landing (cannot fall)     */
/* Where action_harddrop would leave the active piece, without moving it.
 * Pure query: g is not modified. PIECE_NONE in, PIECE_NONE out. */
Piece action_ghost(const GameState *g);

int action_harddrop(GameState *g); /* drop to landing (locks); returns
                                      cells dropped                       */

/* Spawn next[0] as the active piece and advance the queue, refilling the tail
 * from the deterministic stream (rng_piece + piece_generated_counter). Returns
 * false if the new piece cannot spawn (board topped out = game over). */
bool spawn_next(GameState *g);

/* Swap the active piece with the hold slot (once per piece, until it locks).
 * If nothing is held, stores the active piece and pulls the next from the
 * queue. Returns false if hold was already used this piece. */
bool action_hold(GameState *g);
#endif /* TETRISBRAIN_ACTION_H */
