/*
 * test_board.c - libtetrisbrain grid rules: collision, locking, line clearing.
 *
 * Pure logic: no ncurses, no sockets, no server. Links against the brain
 * sources directly, so it runs even when the rest of the tree does not build.
 *
 * This is the first automated coverage libtetrisbrain has had. Everything in
 * the engine was previously exercised only through a live server and a
 * terminal, which is why a frozen board or a piece that will not lock is
 * currently only reproducible by playing.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdio.h>
#include "test_output.h"
#include <string.h>

#include "libtetrisutil/gamestate.h"
#include "libtetrisbrain/board.h"
#include "libtetrisbrain/tetrisbrain.h"

static int tests_run = 0, tests_failed = 0;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

/* An empty board with a known seed and no active piece. */
static void blank(GameState *g, unsigned seed)
{
    memset(g, 0, sizeof *g);
    g->seed = seed;
}

/* Fill row `row` completely except for column `gap` (-1 = fill it all). */
static void fill_row(GameState *g, int row, int gap)
{
    for (int c = 0; c < BOARD_WIDTH; c++)
        g->board[row][c] = (c == gap) ? PIECE_NONE : PIECE_T;
}

/* --- collision ---------------------------------------------------------- */

static int test_collides_outside_the_playfield(void)
{
    GameState g;
    blank(&g, 1);

    Piece p = {PIECE_O, 0, 0, 0};
    CHECK(!board_collides(&g, p),
          "an O at the origin should fit an empty board");

    p.x = -1;
    CHECK(board_collides(&g, p), "a piece off the left edge must collide");

    p.x = BOARD_WIDTH;
    CHECK(board_collides(&g, p), "a piece off the right edge must collide");

    p.x = 0;
    p.y = BOARD_HEIGHT;
    CHECK(board_collides(&g, p), "a piece below the floor must collide");
    return 0;
}

static int test_collides_with_locked_cells(void)
{
    GameState g;
    blank(&g, 1);

    Piece p = {PIECE_O, 0, 3, 5};
    CHECK(!board_collides(&g, p), "empty board should not collide");

    /* An O occupies a 2x2 somewhere inside its box; block the whole area. */
    for (int r = 5; r < 5 + 4; r++)
        for (int c = 3; c < 3 + 4 && c < BOARD_WIDTH; c++)
            g.board[r][c] = PIECE_I;

    CHECK(board_collides(&g, p), "a piece over locked cells must collide");
    return 0;
}

/* --- locking ------------------------------------------------------------ */

static int test_lock_writes_the_piece(void)
{
    GameState g;
    blank(&g, 1);
    g.hold_used = true;

    Piece p = {PIECE_O, 0, 4, 10};
    board_lock(&g, p);

    int written = 0;
    for (int r = 0; r < BOARD_HEIGHT; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            if (g.board[r][c] != PIECE_NONE)
            {
                CHECK(g.board[r][c] == PIECE_O, "locked the wrong kind");
                written++;
            }
    CHECK(written == 4, "an O must lock exactly four cells");
    CHECK(g.hold_used == false, "locking must re-arm hold");
    return 0;
}

/* --- line clearing ------------------------------------------------------ */

static int test_full_row_clears(void)
{
    GameState g;
    blank(&g, 1);
    fill_row(&g, BOARD_HEIGHT - 1, -1);

    CHECK(board_clear_lines(&g) == 1, "a full row must clear");
    for (int c = 0; c < BOARD_WIDTH; c++)
        CHECK(g.board[BOARD_HEIGHT - 1][c] == PIECE_NONE, "row not emptied");
    return 0;
}

static int test_incomplete_row_survives(void)
{
    GameState g;
    blank(&g, 1);
    fill_row(&g, BOARD_HEIGHT - 1, 4); /* one hole */

    CHECK(board_clear_lines(&g) == 0, "a row with a hole must not clear");
    CHECK(g.board[BOARD_HEIGHT - 1][0] == PIECE_T, "row was wrongly cleared");
    return 0;
}

static int test_four_rows_clear_at_once(void)
{
    GameState g;
    blank(&g, 1);
    for (int r = BOARD_HEIGHT - 4; r < BOARD_HEIGHT; r++)
        fill_row(&g, r, -1);

    CHECK(board_clear_lines(&g) == 4, "a tetris must clear four rows");
    for (int r = 0; r < BOARD_HEIGHT; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            CHECK(g.board[r][c] == PIECE_NONE, "board should be empty");
    return 0;
}

/* Rows above a cleared line collapse onto it, and keep their contents. */
static int test_rows_above_collapse(void)
{
    GameState g;
    blank(&g, 1);
    fill_row(&g, BOARD_HEIGHT - 1, -1);     /* full: will clear   */
    g.board[BOARD_HEIGHT - 2][7] = PIECE_S; /* lone cell above it */

    CHECK(board_clear_lines(&g) == 1, "bottom row must clear");
    CHECK(g.board[BOARD_HEIGHT - 1][7] == PIECE_S, "cell above did not fall");
    CHECK(g.board[BOARD_HEIGHT - 2][7] == PIECE_NONE, "cell was not moved");
    return 0;
}

/* --- spawning ----------------------------------------------------------- */

static int test_spawn_fails_when_blocked(void)
{
    GameState g;
    blank(&g, 1);
    CHECK(board_spawn_piece(&g, PIECE_O) == true, "spawn into an empty board");

    blank(&g, 1);
    for (int r = 0; r < BOARD_BUFFER_HEIGHT; r++) /* fill the spawn zone */
        fill_row(&g, r, -1);
    CHECK(board_spawn_piece(&g, PIECE_O) == false,
          "spawn into a blocked zone must fail - this is how a round ends");
    return 0;
}

/* --- the engine end to end ---------------------------------------------- */

/* Hard-dropping without ever moving sideways stacks pieces in the middle and
 * must eventually top out. If this hangs or fails, a round can never end. */
static int test_repeated_hard_drops_top_out(void)
{
    GameState g;
    tetrisbrain_init(&g, 42);

    int drops = 0;
    while (!tetrisbrain_game_over(&g) && drops < 1000)
    {
        tetrisbrain_input(&g, MOVE_HARD_DROP);
        drops++;
    }
    CHECK(tetrisbrain_game_over(&g),
          "hard drops alone never topped the board out");
    return 0;
}

/* Same seed, same inputs, same board - the whole multiplayer design rests on
 * this, because every player replays the round from one shared seed. */
static int test_same_seed_same_game(void)
{
    GameState a, b;
    tetrisbrain_init(&a, 8675309);
    tetrisbrain_init(&b, 8675309);

    for (int i = 0; i < 20; i++)
    {
        tetrisbrain_input(&a, MOVE_HARD_DROP);
        tetrisbrain_input(&b, MOVE_HARD_DROP);
    }
    CHECK(memcmp(a.board, b.board, sizeof a.board) == 0,
          "same seed produced different boards");
    CHECK(a.piece_generated_counter == b.piece_generated_counter,
          "piece streams diverged");
    return 0;
}

/* --- harness ------------------------------------------------------------ */

/* --- garbage ------------------------------------------------------------- */

static int test_garbage_lifts_the_stack(void)
{
    GameState g;
    blank(&g, 1);
    g.board[BOARD_HEIGHT - 1][0] = PIECE_S; /* marker on the floor */

    CHECK(board_add_garbage(&g, 2) == true,
          "two rows must fit on an empty board");
    CHECK(g.board[BOARD_HEIGHT - 3][0] == PIECE_S,
          "the stack must rise by two");

    for (int r = BOARD_HEIGHT - 2; r < BOARD_HEIGHT; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
        {
            Cell want = (c == GARBAGE_HOLE_COL) ? PIECE_NONE : PIECE_GARBAGE;
            CHECK(g.board[r][c] == want, "garbage row has the wrong shape");
        }
    return 0;
}

/* Every row of a batch shares one hole, so plugging that column clears the
 * lot. This is what makes garbage answerable instead of merely punishing. */
static int test_garbage_shares_one_hole(void)
{
    GameState g;
    blank(&g, 1);

    CHECK(board_add_garbage(&g, 3) == true, "three rows must fit");
    CHECK(board_clear_lines(&g) == 0, "garbage must not clear itself");

    for (int r = BOARD_HEIGHT - 3; r < BOARD_HEIGHT; r++)
        g.board[r][GARBAGE_HOLE_COL] = PIECE_T;
    CHECK(board_clear_lines(&g) == 3,
          "filling one column must clear the batch");
    return 0;
}

static int test_garbage_can_top_out(void)
{
    GameState g;
    blank(&g, 1);
    g.board[0][4] = PIECE_Z; /* a cell in the very top row */

    CHECK(board_add_garbage(&g, 1) == false,
          "pushing a locked cell past row 0 must report a top out");
    return 0;
}

/*
 * The realistic way garbage kills.
 *
 * Pushing locked cells off the top is hard: there are BOARD_BUFFER_HEIGHT
 * spare rows above the main field to absorb the lift. What actually happens
 * first is that the raised stack reaches the spawn zone, and the next piece
 * has nowhere to appear - the ordinary top-out, reached by a new route.
 */
static int test_garbage_kills_by_blocking_spawn(void)
{
    GameState g;
    tetrisbrain_init(&g, 3);

    /* A wall filling the whole main field, column 9 left open so that none of
     * it clears. The buffer above is still empty. */
    for (int r = BOARD_HEIGHT - BOARD_MAIN_HEIGHT; r < BOARD_HEIGHT; r++)
        fill_row(&g, r, 9);

    CHECK(board_add_garbage(&g, 3) == true,
          "with a clear buffer, 3 rows must not push anything off the top");
    CHECK(board_spawn_piece(&g, PIECE_O) == false,
          "the raised stack must leave the next piece nowhere to spawn");
    return 0;
}

static int test_garbage_zero_is_a_noop(void)
{
    GameState g, before;
    blank(&g, 1);
    fill_row(&g, BOARD_HEIGHT - 1, 3);
    before = g;

    CHECK(board_add_garbage(&g, 0) == true, "zero rows must succeed");
    CHECK(memcmp(&g, &before, sizeof g) == 0, "zero rows must change nothing");
    CHECK(board_add_garbage(&g, -2) == true, "negative rows must succeed");
    CHECK(memcmp(&g, &before, sizeof g) == 0,
          "negative rows must change nothing");
    return 0;
}

/* The falling piece keeps its coordinates while the stack rises under it, so
 * it has to be lifted or it ends up embedded in the new rows. */
static int test_garbage_lifts_the_falling_piece(void)
{
    GameState g;
    blank(&g, 1);
    CHECK(board_spawn_piece(&g, PIECE_O) == true, "spawn an O");

    Piece step = g.active;
    for (step.y = g.active.y + 1; !board_collides(&g, step); step.y++)
        g.active.y = step.y;
    int landed = g.active.y;

    CHECK(board_add_garbage(&g, 2) == true, "two rows must fit");
    CHECK(g.active.y == landed - 2,
          "the falling piece must ride up with the stack");
    CHECK(!board_collides(&g, g.active),
          "the falling piece must not be buried");
    return 0;
}

/*
 * The attack rule, driven through the public API: N rows cleared at once earn
 * N-1 rows of garbage, and a single earns nothing.
 *
 * Both set up rows that need only columns 3 and 4 - exactly where an O spawns
 * and exactly its width - so one hard drop completes them.
 */
static void need_o_footprint(GameState *g, int rows)
{
    for (int r = BOARD_HEIGHT - rows; r < BOARD_HEIGHT; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            g->board[r][c] = (c == 3 || c == 4) ? PIECE_NONE : PIECE_T;
}

static int test_single_earns_no_garbage(void)
{
    GameState g;
    tetrisbrain_init(&g, 7);
    need_o_footprint(&g, 1);
    CHECK(board_spawn_piece(&g, PIECE_O) == true, "spawn an O");

    tetrisbrain_input(&g, MOVE_HARD_DROP);
    CHECK(g.lines == 1, "the O must complete one row");
    CHECK(g.garbage_out == 0, "a single must earn no garbage");
    return 0;
}

static int test_double_earns_one_garbage_row(void)
{
    GameState g;
    tetrisbrain_init(&g, 7);
    need_o_footprint(&g, 2);
    CHECK(board_spawn_piece(&g, PIECE_O) == true, "spawn an O");
    CHECK(g.garbage_out == 0, "nothing earned before the drop");

    tetrisbrain_input(&g, MOVE_HARD_DROP);
    CHECK(g.lines == 2, "the O must complete both rows");
    CHECK(g.garbage_out == 1, "a double must earn one garbage row");
    return 0;
}

static void run(const char *name, int (*fn)(void))
{
    tests_run++;
    if (fn() == 0)
    {
        test_output_pass(name);
    }
    else
    {
        tests_failed++;
        test_output_fail(name);
    }
}

int main(void)
{
    test_output_begin("test_board");
    run("collides outside the playfield", test_collides_outside_the_playfield);
    run("collides with locked cells", test_collides_with_locked_cells);
    run("lock writes the piece", test_lock_writes_the_piece);
    run("a full row clears", test_full_row_clears);
    run("a row with a hole survives", test_incomplete_row_survives);
    run("four rows clear at once", test_four_rows_clear_at_once);
    run("rows above a clear collapse", test_rows_above_collapse);
    run("spawn fails when blocked", test_spawn_fails_when_blocked);
    run("hard drops eventually top out", test_repeated_hard_drops_top_out);
    run("same seed gives the same game", test_same_seed_same_game);
    run("garbage lifts the stack", test_garbage_lifts_the_stack);
    run("garbage shares one hole", test_garbage_shares_one_hole);
    run("garbage can top the board out", test_garbage_can_top_out);
    run("garbage kills by blocking spawn",
        test_garbage_kills_by_blocking_spawn);
    run("zero or negative garbage is a no-op", test_garbage_zero_is_a_noop);
    run("garbage lifts the falling piece",
        test_garbage_lifts_the_falling_piece);
    run("a single earns no garbage", test_single_earns_no_garbage);
    run("a double earns one garbage row", test_double_earns_one_garbage_row);

    test_output_summary(tests_run, tests_failed, 0);
    return tests_failed == 0 ? 0 : 1;
}
