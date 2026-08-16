/*
 * rng.c - deterministic, stateless piece generator.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/rng.h"

/* Map a mixed 32-bit value onto a real piece: PIECE_I .. PIECE_L.
 * The range is PIECE_SPAWNABLE, not PIECE_COUNT: only the kinds below that
 * marker can be dealt to a player (PIECE_GARBAGE sits above it). */
static PieceKind piece_from_hash(unsigned h)
{
    unsigned r = (h >> 16) % (unsigned)(PIECE_SPAWNABLE - 1);
    return (PieceKind)(PIECE_I + r);
}

PieceKind rng_piece(unsigned seed, unsigned counter)
{
    /* Fold the counter into the seed so successive positions differ. Because
     * the counter always increases, the stream has no short cycle. */
    unsigned h = (seed ^ (counter * 2654435761u)) /* Knuth mult. hash */
                     * 1664525u +
                 1013904223u;
    return piece_from_hash(h);
}
