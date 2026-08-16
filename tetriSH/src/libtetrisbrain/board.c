/*
 * board.c - grid operations: collision, locking, line clearing.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <assert.h>
#include "libtetrisbrain/board.h"
#include "libtetrisbrain/piece.h"

bool board_collides(const GameState *g, Piece p)
{
    int n = piece_size(p);

    for (int r = 0; r < n; r++)
    {
        for (int c = 0; c < n; c++)
        {
            if (!piece_filled(p, r, c))
                continue;

            int bx = p.x + c; /* board column */
            int by = p.y + r; /* board row    */

            /* outside the playfield */
            if (bx < 0 || bx >= BOARD_WIDTH || by < 0 || by >= BOARD_HEIGHT)
                return true;

            /* lands on an already-filled cell */
            if (g->board[by][bx] != PIECE_NONE)
                return true;
        }
    }
    return false;
}

bool board_spawn_piece(GameState *g, PieceKind kind)
{
    Piece p;
    p.kind = kind;
    p.rot = 0;
    p.y = BOARD_BUFFER_HEIGHT - piece_size(p);
    p.x = 3;
    if (board_collides(g, p))
        return false;
    g->active = p;
    g->tick_count = 0;
    return true;
}

void board_lock(GameState *g, Piece p)
{
    int n = piece_size(p);

    for (int r = 0; r < n; r++)
    {
        for (int c = 0; c < n; c++)
        {
            if (!piece_filled(p, r, c))
                continue;

            int bx = p.x + c;
            int by = p.y + r;
            /* defensive: skip any cell outside the board */
            if (bx < 0 || bx >= BOARD_WIDTH || by < 0 || by >= BOARD_HEIGHT)
                continue;

            g->board[by][bx] = (Cell)p.kind; /* falling cell -> locked cell */
        }
    }

    g->hold_used = false;
}

/* Is board row `row` completely filled? */
static bool row_full(const GameState *g, int row)
{
    assert(0 <= row && row <= BOARD_HEIGHT);
    for (int c = 0; c < BOARD_WIDTH; c++)
        if (g->board[row][c] == PIECE_NONE)
            return false;
    return true;
}

int board_clear_lines(GameState *g)
{
    int cleared = 0;

    /* Walk bottom -> top. `dst` is where the next surviving row is written;
     * .`src` scans upward. Full rows are skipped, so rows above collapse down.
     */
    int dst = BOARD_HEIGHT - 1;
    for (int src = BOARD_HEIGHT - 1; src >= 0; src--)
    {
        if (row_full(g, src))
        {
            cleared++;
            continue; /* drop this row */
        }
        if (dst != src)
        {
            for (int c = 0; c < BOARD_WIDTH; c++)
                g->board[dst][c] = g->board[src][c];
        }
        dst--;
    }
    /* Top `cleared` rows are now vacated -> empty them. */
    for (; dst >= 0; dst--)
    {
        for (int c = 0; c < BOARD_WIDTH; c++)
            g->board[dst][c] = PIECE_NONE;
    }

    return cleared;
}

bool board_add_garbage(GameState *g, int rows)
{
    if (rows <= 0)
        return true;
    if (rows > BOARD_HEIGHT)
        rows = BOARD_HEIGHT;

    /* Whatever sits in the top `rows` rows is about to be pushed past row 0.
     * If any of it is a locked cell, the stack no longer fits on the board. */
    bool topped = false;
    for (int r = 0; r < rows && !topped; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            if (g->board[r][c] != PIECE_NONE)
            {
                topped = true;
                break;
            }

    /* Lift the stack. dst always trails src, so a forward scan never
     * overwrites a row it still has to read. */
    for (int dst = 0; dst < BOARD_HEIGHT - rows; dst++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            g->board[dst][c] = g->board[dst + rows][c];

    /* Lay the new rows in underneath, all sharing one hole. */
    for (int r = BOARD_HEIGHT - rows; r < BOARD_HEIGHT; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            g->board[r][c] =
                (c == GARBAGE_HOLE_COL) ? PIECE_NONE : (Cell)PIECE_GARBAGE;

    /* The falling piece kept its coordinates while the stack rose under it, so
     * it may now be buried. Walk it up until it is clear - or off the top, in
     * which case there is nowhere to put it. */
    while (g->active.kind != PIECE_NONE && board_collides(g, g->active))
    {
        if (g->active.y <= -piece_size(g->active))
            return false;
        g->active.y--;
    }

    return !topped;
}
