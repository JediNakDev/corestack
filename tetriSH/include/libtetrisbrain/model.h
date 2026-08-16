#ifndef TETRISBRAIN_MODEL_H
#define TETRISBRAIN_MODEL_H

/*
 * model.h - the data model libtetrisbrain works on.
 *
 * The types themselves (GameState, Piece, PieceKind, Cell, Move, board
 * dimensions) live in libtetrisutil because tetrisd and tetrisu need them too.
 * This header just re-exports them, so the brain's modules keep including
 * "libtetrisbrain/model.h" as before.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/gamestate.h"

#endif /* TETRISBRAIN_MODEL_H */
