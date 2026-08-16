/*
 * tetrisbrain.c - the game-rules facade: init, gravity tick, input dispatch.
 *
 * The conductor. It owns game start-up and the gravity timer, and dispatches
 * player input to the action layer. The lock cycle itself (lock -> clear ->
 * score -> spawn) lives with the action that triggers it (see action.c).
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/tetrisbrain.h"
#include "libtetrisbrain/action.h" /* action_*, spawn_next */
#include "libtetrisbrain/rng.h"    /* rng_piece            */

/* Ticks between auto-falls. Constant for now (constant speed). */
#define GRAVITY_TICKS 20

void tetrisbrain_init(GameState *g, unsigned seed)
{
    /* Empty the board. */
    for (int r = 0; r < BOARD_HEIGHT; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            g->board[r][c] = PIECE_NONE;

    /* Reset all game state. */
    g->hold = PIECE_NONE;
    g->hold_used = false;
    g->score = 0;
    g->lines = 0;
    g->level = 0;
    g->tick_count = 0;
    g->tick_trigger = GRAVITY_TICKS;
    g->seed = seed;
    g->game_over = false;
    g->garbage_out = 0;

    /* Fill the queue with the first pieces of the deterministic stream. */
    int len = (int)(sizeof g->next / sizeof g->next[0]);
    for (int k = 0; k < len; k++)
        g->next[k] = rng_piece(seed, (unsigned)k);
    g->piece_generated_counter = (unsigned)len;

    /*
     * g->standings / g->standing_count are deliberately NOT reset here.
     *
     * They describe the whole room, which this engine knows nothing about -
     * tetrisd's admin thread owns them and pushes them down. Clearing them at
     * the start of a round would blank the opponents' panel every time the
     * owner pressed START, until the next update happened to arrive; the
     * server resets the scores themselves when a round ends, so carrying the
     * array across is both harmless and steadier to look at.
     *
     * The engine touches exactly one board. Keep it that way: nothing in
     * libtetrisbrain should ever read or write these two fields.
     */

    spawn_next(g); /* pop the first piece as active; refills the queue tail */
}

void tetrisbrain_tick(GameState *g)
{
    if (g->game_over)
    {
        return;
    }

    /* softdrop resets tick_count and runs the full lock cycle on landing. */
    if (++g->tick_count >= g->tick_trigger)
        action_softdrop(g);
}

void tetrisbrain_input(GameState *g, Move m)
{
    if (g->game_over)
    {
        return;
    }

    switch (m)
    {
    case MOVE_LEFT:
        action_lmove(g);
        break;
    case MOVE_RIGHT:
        action_rmove(g);
        break;
    case MOVE_ROT_RIGHT:
        action_rrot(g);
        break;
    case MOVE_ROT_LEFT:
        action_lrot(g);
        break;
    case MOVE_SOFT_DROP:
        action_softdrop(g);
        break;
    case MOVE_HARD_DROP:
        action_harddrop(g);
        break;
    case HOLD:
        action_hold(g);
        break;
    default:
        break;
    }
}

bool tetrisbrain_game_over(const GameState *g)
{
    return g->game_over;
}

/* Forwarded rather than reimplemented: the landing rule lives beside
 * action_harddrop so the two cannot drift apart. See action_ghost. */
Piece tetrisbrain_ghost(const GameState *g)
{
    return action_ghost(g);
}
