/*
 * action.c - moving and rotating the active piece.
 *
 * Pure transforms build a candidate; the try_* handlers validate it against
 * the board before committing. No input mapping, no I/O.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/action.h"
#include "libtetrisbrain/board.h" /* board_collides, board_lock, board_clear_lines */
#include "libtetrisbrain/piece.h"   /* PIECE_ROTS */
#include "libtetrisbrain/rng.h"     /* rng_piece */
#include "libtetrisbrain/scoring.h" /* scoring_add */

/* ---- pure transforms ---- */

Piece action_rotate(Piece p, int step)
{
    p.rot = ((p.rot + step) % PIECE_ROTS + PIECE_ROTS) % PIECE_ROTS;
    return p;
}

Piece action_move(Piece p, int dx, int dy)
{
    p.x += dx;
    p.y += dy;
    return p;
}

/* ---- check-and-commit handlers ---- */

bool action_try_move(GameState *g, int dx, int dy)
{
    Piece cand = action_move(g->active, dx, dy);
    if (board_collides(g, cand))
        return false;
    g->active = cand;
    return true;
}

bool action_try_rotate(GameState *g, int dir)
{
    Piece cand = action_rotate(g->active, dir);
    int n = piece_size(cand);
    if (!board_collides(g, cand))
        goto able;
    for (int i = 1; i <= n; i++)
    {
        // try moving right first
        cand = action_rotate(g->active, dir);
        cand = action_move(cand, i, 0);
        if (!board_collides(g, cand))
            goto able;

        // then try moving left
        cand = action_rotate(g->active, dir);
        cand = action_move(cand, -i, 0);
        if (!board_collides(g, cand))
            goto able;
    }
    goto unable;
able:
    g->active = cand;
    return true;
unable:
    return false;
}

bool action_lmove(GameState *g)
{
    return action_try_move(g, -1, 0);
}

bool action_rmove(GameState *g)
{
    return action_try_move(g, +1, 0);
}

bool action_rrot(GameState *g)
{
    return action_try_rotate(g, +1);
}

bool action_lrot(GameState *g)
{
    return action_try_rotate(g, -1);
}

/*
 * Attack earned by clearing `n` rows in one move: a double sends 1 row, a
 * triple 2, a tetris 3. A single sends nothing.
 *
 * Accumulates rather than assigns - the server may not have drained the last
 * clear yet, and an unsent attack must not be dropped.
 */
static void garbage_earn(GameState *g, int n)
{
    if (n >= 2)
        g->garbage_out += n - 1;
}

/* After the active piece locks into the board: clear full rows, score them,
 * bank any garbage they earned, re-enable hold, and spawn the next piece
 * (top out -> game over). */
static void post_lock(GameState *g)
{
    int n = board_clear_lines(g);
    scoring_add(g, n);
    garbage_earn(g, n);
    g->hold_used = false;
    if (!spawn_next(g))
        g->game_over = true;
}

bool action_softdrop(GameState *g)
{
    bool result = action_try_move(g, 0, +1);
    if (!result)
    {
        board_lock(g, g->active);
        post_lock(g); /* finish the lock cycle */
    }
    g->tick_count = 0;
    return result;
}
int action_harddrop(GameState *g)
{
    int dropped = 0;
    while (action_softdrop(g))
        dropped++;
    /* softdrop locks the piece once it can no longer fall. */
    return dropped;
}

/*
 * Where action_harddrop would leave the active piece, without moving it.
 *
 * Kept right next to action_harddrop on purpose: these two must always agree.
 * A landing preview that is off by even one row is worse than no preview at
 * all, because the player trusts it. Keeping them adjacent means a change to
 * one is made staring at the other.
 *
 * It cannot simply call action_harddrop: that runs action_softdrop, which
 * LOCKS the piece on landing and starts the next spawn. This is a pure query -
 * GameState is const and nothing here mutates it.
 */
Piece action_ghost(const GameState *g)
{
    Piece p = g->active;
    if (p.kind == PIECE_NONE)
        return p;

    while (!board_collides(g, action_move(p, 0, +1)))
        p = action_move(p, 0, +1);
    return p;
}

bool spawn_next(GameState *g)
{
    int len = (int)(sizeof g->next / sizeof g->next[0]);
    PieceKind k = g->next[0];

    /* shift the queue down one slot */
    for (int i = 0; i + 1 < len; i++)
        g->next[i] = g->next[i + 1];
    /* refill the freed tail with the next piece in the deterministic stream */
    g->next[len - 1] = rng_piece(g->seed, g->piece_generated_counter++);

    return board_spawn_piece(g, k); /* false -> board topped out (game over) */
}

/*
 * Swap the active piece with the hold slot.
 *
 * The return value answers one question only: was the hold allowed? `false`
 * means it was refused because this piece has already used its hold. A top out
 * is NOT reported that way - by the time a spawn can fail the hold has
 * genuinely happened (on the empty-slot path spawn_next has already advanced
 * the queue), so returning false would claim nothing changed. Topping out is
 * reported like every other spawn failure, via g->game_over; see post_lock.
 */
bool action_hold(GameState *g)
{
    if (g->hold_used)
        return false; /* only one hold per piece */

    PieceKind cur = g->active.kind;
    g->hold_used = true; /* both paths below consume the hold */

    if (g->hold == PIECE_NONE)
    {
        /* Nothing held yet: store this piece and pull the next one up. The
         * queue advances inside spawn_next before it tries to spawn, so this
         * branch cannot be made atomic - if the spawn fails, all we can do is
         * report the top out. */
        g->hold = cur;
        if (!spawn_next(g))
            g->game_over = true;
    }
    else
    {
        /* Straight swap; the queue is untouched. board_spawn_piece only
         * commits g->active when the piece actually fits, so test it BEFORE
         * overwriting the hold slot - otherwise a failed spawn loses the held
         * piece and duplicates the active one into the slot. */
        PieceKind held = g->hold;
        if (!board_spawn_piece(g, held))
            g->game_over = true;
        else
            g->hold = cur;
    }
    return true;
}
