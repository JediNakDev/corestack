#ifndef TETRISBRAIN_PIECE_H
#define TETRISBRAIN_PIECE_H

/*
 * piece.h - static shape data for tetrominoes (pure queries, no logic).
 *
 * A live piece is a `Piece` (kind + rotation + board position; see model.h).
 * That only identifies the piece; it does not describe which cells it fills.
 * This module answers those questions from a const shape table:
 *
 *   - how big is a piece's box?          piece_size()
 *   - which cells does (kind,rot) fill?  piece_grid() / piece_filled()
 *
 * No movement, rotation or collision here - that logic lives in action.c and
 * board.c. The underlying shape table is private to piece.c.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/model.h"

/* Number of rotation states every piece has: North, East, South, West. */
#define PIECE_ROTS 4

/* Side length of piece p's square box (O=2, most=3, I=4). 0 if none/invalid. */
int piece_size(Piece p);

/*
 * Occupancy grid for piece p (its kind at its rotation): a row-major array
 * of size*size ints, 1 = filled. Index as grid[row*size + col].
 * Returns NULL for PIECE_NONE / invalid.
 */
const int *piece_grid(Piece p);

/* True if cell (row r, col c) of piece p's box is filled.
 * Out-of-box (r,c) return false. Convenience wrapper over piece_grid. */
bool piece_filled(Piece p, int r, int c);

#endif /* TETRISBRAIN_PIECE_H */