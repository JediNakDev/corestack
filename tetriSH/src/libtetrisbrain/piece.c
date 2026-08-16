/*
 * piece.c - tetromino shape tables and rotation.
 *
 * Shapes are transcribed from the project's rotation chart (North/East/
 * South/West). Each rotation is a row-major occupancy grid of size*size
 * ints; 1 = filled. dx grows right, dy grows down.
 *
 * TO ADD A NEW PIECE:
 *   1. add its kind to PieceKind in model.h (before PIECE_SPAWNABLE, so the
 *      generator starts dealing it).
 *   2. add a shape block + a PIECES[] entry below.
 * Nothing else in the engine needs to change; board/render iterate generically.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stddef.h> /* NULL */
#include <assert.h>
#include "libtetrisbrain/piece.h"

/*
 * A piece's immutable shape data - private to this module. Callers use the
 * piece_* query functions instead of touching the table directly.
 *   size    - the box is size x size cells.
 *   grid[r] - rotation r's occupancy, a row-major array of size*size ints,
 *             1 = filled, 0 = empty. grid[r][row*size + col].
 */
typedef struct
{
    int size;
    const int *grid[PIECE_ROTS];
} PieceDef;

/* ---- O (2x2): identical in every rotation ---- */
static const int O_SHAPE[4] = {1, 1, 1, 1};

/* ---- I (4x4) ---- */
static const int I_N[16] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
static const int I_E[16] = {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0};
static const int I_S[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
static const int I_W[16] = {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0};

/* ---- T (3x3) ---- */
static const int T_N[9] = {0, 1, 0, 1, 1, 1, 0, 0, 0};
static const int T_E[9] = {0, 1, 0, 0, 1, 1, 0, 1, 0};
static const int T_S[9] = {0, 0, 0, 1, 1, 1, 0, 1, 0};
static const int T_W[9] = {0, 1, 0, 1, 1, 0, 0, 1, 0};

/* ---- S (3x3) ---- */
static const int S_N[9] = {0, 1, 1, 1, 1, 0, 0, 0, 0};
static const int S_E[9] = {0, 1, 0, 0, 1, 1, 0, 0, 1};
static const int S_S[9] = {0, 0, 0, 0, 1, 1, 1, 1, 0};
static const int S_W[9] = {1, 0, 0, 1, 1, 0, 0, 1, 0};

/* ---- Z (3x3) ---- */
static const int Z_N[9] = {1, 1, 0, 0, 1, 1, 0, 0, 0};
static const int Z_E[9] = {0, 0, 1, 0, 1, 1, 0, 1, 0};
static const int Z_S[9] = {0, 0, 0, 1, 1, 0, 0, 1, 1};
static const int Z_W[9] = {0, 1, 0, 1, 1, 0, 1, 0, 0};

/* ---- J (3x3) ---- */
static const int J_N[9] = {1, 0, 0, 1, 1, 1, 0, 0, 0};
static const int J_E[9] = {0, 1, 1, 0, 1, 0, 0, 1, 0};
static const int J_S[9] = {0, 0, 0, 1, 1, 1, 0, 0, 1};
static const int J_W[9] = {0, 1, 0, 0, 1, 0, 1, 1, 0};

/* ---- L (3x3) ---- */
static const int L_N[9] = {0, 0, 1, 1, 1, 1, 0, 0, 0};
static const int L_E[9] = {0, 1, 0, 0, 1, 0, 0, 1, 1};
static const int L_S[9] = {0, 0, 0, 1, 1, 1, 1, 0, 0};
static const int L_W[9] = {1, 1, 0, 0, 1, 0, 0, 1, 0};

/*
 * Master table, indexed by PieceKind. Designated initializers make the
 * mapping order-independent and obvious. PIECE_NONE stays zeroed.
 */
static const PieceDef PIECES[PIECE_COUNT] = {
    [PIECE_I] = {.size = 4, .grid = {I_N, I_E, I_S, I_W}},
    [PIECE_O] = {.size = 2, .grid = {O_SHAPE, O_SHAPE, O_SHAPE, O_SHAPE}},
    [PIECE_T] = {.size = 3, .grid = {T_N, T_E, T_S, T_W}},
    [PIECE_S] = {.size = 3, .grid = {S_N, S_E, S_S, S_W}},
    [PIECE_Z] = {.size = 3, .grid = {Z_N, Z_E, Z_S, Z_W}},
    [PIECE_J] = {.size = 3, .grid = {J_N, J_E, J_S, J_W}},
    [PIECE_L] = {.size = 3, .grid = {L_N, L_E, L_S, L_W}},
};

/*
 * Private lookup: the definition for `kind`, or NULL if none/invalid.
 *
 * PIECE_NONE is a legal query, not an error - callers use it to ask "is there
 * a piece here?" (an empty hold slot, an unfilled next queue, or a board with
 * no active piece between a lock and the next spawn). It returns NULL, which
 * piece_size/piece_grid/piece_filled turn into 0/NULL/false as piece.h
 * promises. A kind outside the enum, on the other hand, means the GameState is
 * corrupt and we are one step from indexing PIECES[] out of bounds - assert so
 * that shows up at its source rather than as a wild read.
 */
static const PieceDef *piece_def(PieceKind kind)
{
    assert((int)kind >= (int)PIECE_NONE && (int)kind < (int)PIECE_COUNT);
    if (kind <= PIECE_NONE || kind >= PIECE_COUNT)
        return NULL;
    return &PIECES[kind];
}

int piece_size(Piece p)
{
    const PieceDef *d = piece_def(p.kind);
    return d ? d->size : 0;
}

const int *piece_grid(Piece p)
{
    const PieceDef *d = piece_def(p.kind);
    if (!d)
        return NULL;

    /* p.rot assumed already normalised to 0..3 (see action_rotate). */
    return d->grid[p.rot];
}

bool piece_filled(Piece p, int r, int c)
{
    int n = piece_size(p);
    if (n == 0 || r < 0 || c < 0 || r >= n || c >= n)
        return false;

    const int *g = piece_grid(p);
    return g && (g[r * n + c] == 1);
}