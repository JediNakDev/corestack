#ifndef TETRISBRAIN_RNG_H
#define TETRISBRAIN_RNG_H

/*
 * rng.h - deterministic, stateless piece generator.
 *
 * A piece is a pure function of the shared seed and a counter (the piece's
 * position in the sequence: 0, 1, 2, ...). No evolving RNG state to keep or
 * sync: every player computes the same piece for the same (seed, counter), so
 * a shared seed gives everyone an identical sequence.
 *
 *     for (int k = 0; k < 5; k++)
 *         next[k] = rng_next_piece(seed, count + k);   // fill the queue
 *     // on spawn: count++
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/model.h"

/* Piece at position `counter` in the shared sequence. */
PieceKind rng_piece(unsigned seed, unsigned counter);

#endif /* TETRISBRAIN_RNG_H */