#ifndef TETRISBRAIN_BOARD_H
#define TETRISBRAIN_BOARD_H

/*
 * board.h - grid operations: collision, locking, line clearing.
 *
 * The bridge between a piece's shape and the playfield. Every cell a piece
 * covers is (piece.x + col, piece.y + row) on the board; these functions walk
 * the piece box (via piece_filled) and read/write those board cells.
 *
 * Pure: they only inspect or mutate GameState.board / the given piece.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/model.h"

/*
 * True if piece `p` would overlap a locked cell or fall outside the board.
 * Used as the spawn-crash test (summon a new piece in the buffer -> if this
 * is true, the spawn is blocked = game over) and as the move-legality test.
 */
bool board_collides(const GameState *g, Piece p);

/*
 * Summon a new piece of `kind` as the active falling piece, built at the spawn
 * orientation (North) and buffer position. If it already collides the board is
 * topped out: g->active is left unchanged and false is returned. Returns true
 * when spawned successfully (caller ends the game on false).
 */
bool board_spawn_piece(GameState *g, PieceKind kind);

/*
 * Stamp piece `p` into the board grid: for each filled cell, board[y][x] =
 * p.kind. Turns the falling piece into locked cells. Call when the piece has
 * landed (assumes it does not collide). Normally p is g->active.
 */
void board_lock(GameState *g, Piece p);

/* Remove every full row, dropping the rows above down to fill the gap.
 * Returns the number of rows cleared (0..board height). */
int board_clear_lines(GameState *g);

/* The column every garbage row leaves open, so a batch can be drilled out in
 * one go. Fixed rather than random: predictable for the player, and it keeps
 * the engine free of per-board RNG state. */
#define GARBAGE_HOLE_COL 5

/*
 * Push `rows` garbage rows in at the bottom, lifting the whole stack by that
 * much. Each new row is full except GARBAGE_HOLE_COL.
 *
 * The falling piece does not move with the stack, so it can end up overlapping
 * the new rows; it is lifted just clear of them.
 *
 * Returns false if this topped the board out - either locked cells were pushed
 * past row 0, or the falling piece has nowhere left to go. The caller ends the
 * game on false.
 */
bool board_add_garbage(GameState *g, int rows);

#endif /* TETRISBRAIN_BOARD_H */
