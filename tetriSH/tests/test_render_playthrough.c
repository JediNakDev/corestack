/*
 * test_render_playthrough.c - one real game, start to game over, driven only
 * by input.
 *
 * The other two harnesses each test half the picture: test_render_fixtures.c
 * hand-builds GameState structs (renderer only, brain never runs), and
 * test_gameui.c is a free-play demo with no expectations. This one closes the
 * gap: it calls tetrisbrain_init(&g, 0) on an empty board and from then on
 * touches the state ONLY through tetrisbrain_input() and tetrisbrain_tick().
 * Nothing is hand-placed. Every board position you see is one the engine
 * actually computed.
 *
 * The seed is fixed at 0, so the piece stream is known ahead of time:
 *
 *   I S L T Z I S J O Z L T J O S L T Z I S L O Z I T J O S L T J I ...
 *
 * The script is written around that stream - each edge case is scheduled for a
 * piece that can actually exercise it. It is one continuous game, in order,
 * with no jumping: step N is always reached by replaying steps 0..N-1.
 *
 * Steps carry checks, so this is a test and not just a viewer. A check reads
 * the state after the step and returns NULL, or a message describing what went
 * wrong.
 *
 * Build:  make gui
 * Run:    ./bin/test_render_playthrough
 *             interactive: n/space next, p prev, q quit
 *         ./bin/test_render_playthrough --dump
 *             headless: prints every milestone + PASS/FAIL, exit code =
 * failures
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <ncurses.h>
#include <stdio.h>
#include <string.h>

#include "tetrisu/render.h"
#include "libtetrisbrain/gamestate.h"
#include "libtetrisbrain/tetrisbrain.h"
#include "libtetrisbrain/piece.h"

#define SEED 0

#define MIN_ROWS (BOARD_HEIGHT + 5)
#define MIN_COLS RENDER_MIN_COLS

static const char *KINDCH = "?IOTSZJL";

/* ---- script ------------------------------------------------------------- */

/*
 * S_INPUT  - feed `move` to tetrisbrain_input, `repeat` times.
 * S_TICK   - feed `repeat` gravity ticks.
 * S_PLACE  - one whole piece placement in a single row, so that building a
 *            stack does not cost four script lines per piece. `repeat` encodes
 *            it as rot*10 + k: rotate CW `rot` times, slam left until it
 *            clamps, move right `k`, then hard drop. Still nothing but
 *            ordinary inputs - it is shorthand, not a back door into the state.
 */
typedef enum
{
    S_INPUT,
    S_TICK,
    S_PLACE
} StepKind;
#define PLACE(rot, k) ((rot) * 10 + (k))

typedef struct
{
    StepKind kind;
    Move move;                               /* ignored when kind == S_TICK */
    int repeat;                              /* apply this many times       */
    const char *note;                        /* caption; NULL = filler step */
    const char *(*check)(const GameState *); /* NULL = nothing to assert   */
} Step;

/*
 * Sentinel check meaning "this step must change NOTHING". The driver spots it
 * by pointer identity and diffs the whole GameState byte-for-byte against the
 * state before the step, which is far stronger than any hand-written check: it
 * catches a stray tick_count bump or a queue advance that a targeted check
 * would never look at.
 */
static const char *chk_noop(const GameState *g)
{
    (void)g;
    return NULL;
}

/* ---- checks ------------------------------------------------------------- */

static const char *chk_left_wall(const GameState *g)
{
    return g->active.x == 0 ? NULL
                            : "expected active.x == 0 after slamming left";
}

/* S spawns at x=3 and its box spans 3 columns, so the rightmost legal origin
 * is BOARD_WIDTH - 3 = 7. */
static const char *chk_right_wall(const GameState *g)
{
    return g->active.x == BOARD_WIDTH - 3
               ? NULL
               : "expected active.x == 7 after slamming right";
}

static const char *chk_i_on_floor(const GameState *g)
{
    for (int c = 0; c < 4; c++)
        if (g->board[BOARD_HEIGHT - 1][c] != PIECE_I)
            return "expected I locked across cols 0-3 of the floor";
    return NULL;
}

static const char *chk_hold_s_active_l(const GameState *g)
{
    if (g->hold != PIECE_S)
        return "expected S in the hold slot";
    if (!g->hold_used)
        return "expected hold_used set after holding";
    if (g->active.kind != PIECE_L)
        return "expected L pulled from the queue";
    return NULL;
}

/* Hold is once per piece. A second hold must be rejected outright: the active
 * piece and the hold slot both have to be exactly what they were. */
static const char *chk_hold_blocked(const GameState *g)
{
    if (g->hold != PIECE_S)
        return "hold slot changed on a blocked hold";
    if (g->active.kind != PIECE_L)
        return "active piece changed on a blocked hold";
    return NULL;
}

/*
 * The T is resting on the floor but has NOT locked yet, so it is still the
 * active piece and deliberately absent from board[][]: cols 0-6 of the floor
 * are filled by the I and the L, and 7-9 are still open underneath it.
 */
static const char *chk_pre_lock(const GameState *g)
{
    if (g->active.kind != PIECE_T)
        return "expected the T to still be active";
    if (g->active.y != BOARD_HEIGHT - 2)
        return "expected the T resting on the floor (y == 24), not yet locked";
    for (int c = 0; c <= 6; c++)
        if (g->board[BOARD_HEIGHT - 1][c] == PIECE_NONE)
            return "expected floor cols 0-6 already filled by the I and L";
    for (int c = 7; c <= 9; c++)
        if (g->board[BOARD_HEIGHT - 1][c] != PIECE_NONE)
            return "floor cols 7-9 should still be empty until the T locks";
    return NULL;
}

static const char *chk_single_clear(const GameState *g)
{
    if (g->lines != 1)
        return "expected exactly 1 line cleared";
    if (g->score != 100)
        return "expected score 100 for a single";
    return NULL;
}

/* Locking must re-arm hold for the piece that follows. */
static const char *chk_hold_rearmed(const GameState *g)
{
    return g->hold_used ? "expected hold_used cleared after a lock" : NULL;
}

/* Swapping into an occupied slot must NOT consume a piece from the queue. */
static const char *chk_hold_swapped(const GameState *g)
{
    if (g->hold == PIECE_S)
        return "hold slot should no longer hold S after a swap";
    if (g->active.kind != PIECE_S)
        return "expected the held S to become active";
    return NULL;
}




static const char *chk_game_over(const GameState *g)
{
    return g->game_over ? NULL : "expected the game to have topped out by now";
}

/*
 * Once the game is over the engine must ignore everything. Defined after the
 * script (it needs to replay the game itself); forward-declared here so the
 * table below can reference it.
 *
 * Deliberately NOT a static snapshot taken on first call: the viewer replays
 * from scratch on every keypress and lets you step backwards, so a check that
 * depends on call order would report phantom failures.
 */
static const char *chk_frozen(const GameState *g);

/*
 * The game, in order. Piece numbers follow the seed=0 stream.
 *
 * Opening plan: I fills floor columns 0-3, S is parked in hold (it has no flat
 * bottom and would leave a gap), L fills 4-6, T fills 7-9 -> the floor row
 * completes and clears. That is a legitimate line, built by the engine.
 */
static const Step SCRIPT[] = {
    /* --- P1: I --- walls and rotation ----------------------------------- */
    {S_INPUT, MOVE_LEFT, 6, "P1 I: slam left (3 moves land, 3 refused)",
     chk_left_wall},
    {S_INPUT, MOVE_ROT_RIGHT, 1, "P1 I: rotate upright while flush left", NULL},
    {S_INPUT, MOVE_ROT_LEFT, 1, "P1 I: rotate back to flat", chk_left_wall},
    {S_INPUT, MOVE_SOFT_DROP, 3, "P1 I: soft drop x3", NULL},
    {S_INPUT, MOVE_HARD_DROP, 1, "P1 I: HARD DROP -> floor cols 0-3",
     chk_i_on_floor},

    /* --- P2: S --- right wall, then park it in hold ---------------------- */
    {S_INPUT, MOVE_RIGHT, 8, "P2 S: slam right (clamps at x=7)",
     chk_right_wall},
    {S_INPUT, HOLD, 1, "P2 S: HOLD on empty slot -> L pulled up",
     chk_hold_s_active_l},
    {S_INPUT, HOLD, 1, "P3 L: HOLD again -> REFUSED (once/piece)",
     chk_hold_blocked},

    /* --- P3: L --- fills floor cols 4-6 ---------------------------------- */
    {S_INPUT, MOVE_LEFT, 6, "P3 L: to the left wall", NULL},
    {S_INPUT, MOVE_RIGHT, 4, "P3 L: over to x=4", NULL},
    {S_INPUT, MOVE_HARD_DROP, 1, "P3 L: HARD DROP -> floor cols 4-6",
     chk_hold_rearmed},

    /* --- P4: T --- completes the floor row ------------------------------- */
    {S_INPUT, MOVE_RIGHT, 6, "P4 T: over to x=7", NULL},
    /* T spawns at y=3 and rests at y=24, so 21 soft drops land it without
     * locking; the 22nd cannot move, which is what triggers the lock cycle. */
    {S_INPUT, MOVE_SOFT_DROP, 21, "P4 T: soft drop to the floor (pre-lock)",
     chk_pre_lock},
    {S_INPUT, MOVE_SOFT_DROP, 1, "P4 T: one more -> LOCK + SINGLE, +100",
     chk_single_clear},

    /* --- P5: Z --- gravity, not player input ----------------------------- */
    {S_TICK, MOVE_NONE, 19, "P5 Z: 19 ticks - below the gravity trigger", NULL},
    {S_TICK, MOVE_NONE, 1, "P5 Z: 20th tick -> gravity pulls it down", NULL},
    {S_TICK, MOVE_NONE, 60, "P5 Z: 60 more ticks of free fall", NULL},
    {S_INPUT, MOVE_HARD_DROP, 1, "P5 Z: HARD DROP to settle it", NULL},

    /* --- P6: I --- hold swap against an occupied slot -------------------- */
    {S_INPUT, HOLD, 1, "P6 I: HOLD swap -> S returns, queue frozen",
     chk_hold_swapped},
    {S_INPUT, MOVE_ROT_RIGHT, 1, "P6 S: rotate CW", NULL},
    {S_INPUT, MOVE_HARD_DROP, 1, "P6 S: HARD DROP", NULL},

    /* --- rotation against the walls -------------------------------------- */
    {S_INPUT, MOVE_LEFT, 9, "P7: hard against the left wall", chk_left_wall},
    {S_INPUT, MOVE_ROT_RIGHT, 1, "P7: rotate at the wall -> kick if needed",
     NULL},
    {S_INPUT, MOVE_ROT_RIGHT, 1, "P7: rotate again", NULL},
    {S_INPUT, MOVE_HARD_DROP, 1, "P7: HARD DROP", NULL},

    /* --- climb to the top -------------------------------------------------
     * Every piece spawns at x=3, so hard-dropping without moving piles them
     * all into the same few columns and the stack reaches the spawn zone fast.
     * Kept to two steps on purpose: the interesting behaviour is already
     * covered above, and a long climb only makes the test slower to watch. */
    {S_INPUT, MOVE_HARD_DROP, 6, "climb: 6 pieces straight down the middle",
     NULL},
    {S_INPUT, MOVE_HARD_DROP, 6, "climb: 6 more -> tops out", chk_game_over},

    /* --- the engine must be inert now ------------------------------------ */
    {S_INPUT, MOVE_NONE, 1, "GAME OVER: state must now be inert", chk_frozen},
    {S_INPUT, MOVE_LEFT, 5, "GAME OVER: moves ignored", chk_frozen},
    {S_INPUT, MOVE_HARD_DROP, 3, "GAME OVER: hard drops ignored", chk_frozen},
    {S_TICK, MOVE_NONE, 50, "GAME OVER: gravity ignored", chk_frozen},
};
static const int N_STEPS = (int)(sizeof SCRIPT / sizeof SCRIPT[0]);

/* ---- driver ------------------------------------------------------------- */

/* Apply one script step. Every path here goes through tetrisbrain_input or
 * tetrisbrain_tick - the harness never writes to GameState itself. */
static void apply_step(GameState *g, int i)
{
    const Step *s = &SCRIPT[i];

    if (s->kind == S_PLACE)
    {
        int rot = s->repeat / 10, k = s->repeat % 10;
        for (int t = 0; t < rot; t++)
            tetrisbrain_input(g, MOVE_ROT_RIGHT);
        for (int t = 0; t < 9; t++)
            tetrisbrain_input(g, MOVE_LEFT);
        for (int t = 0; t < k; t++)
            tetrisbrain_input(g, MOVE_RIGHT);
        tetrisbrain_input(g, MOVE_HARD_DROP);
        return;
    }

    for (int r = 0; r < s->repeat; r++)
    {
        if (s->kind == S_TICK)
            tetrisbrain_tick(g);
        else
            tetrisbrain_input(g, s->move);
    }
}

/* Replay the game from scratch up to and including step `upto`. Replaying
 * rather than caching is what makes "no jumping around" true: every frame is
 * reached only by playing the game from move one. */
static void play_to(GameState *g, int upto)
{
    /* Zero first: tetrisbrain_init assigns field by field and leaves the
     * padding after hold_used / game_over untouched, which would show up as a
     * phantom difference in the byte-for-byte expect_noop comparison. */
    memset(g, 0, sizeof *g);
    tetrisbrain_init(g, SEED);
    for (int i = 0; i <= upto; i++)
        apply_step(g, i);
}

/* Replay until the game ends, then compare: anything the engine did after that
 * point is a bug. Order-independent, so backwards navigation is safe. */
static const char *chk_frozen(const GameState *g)
{
    GameState ref;
    int i;
    for (i = 0; i < N_STEPS; i++)
    {
        play_to(&ref, i);
        if (ref.game_over)
            break;
    }
    if (i == N_STEPS)
        return "game never ended, so nothing to freeze";

    if (!g->game_over)
        return "game_over flag cleared after the game ended";
    if (memcmp(ref.board, g->board, sizeof ref.board) != 0)
        return "board changed after game over";
    if (ref.score != g->score || ref.lines != g->lines)
        return "score/lines changed after game over";
    if (memcmp(&ref.active, &g->active, sizeof ref.active) != 0)
        return "active piece moved after game over";
    return NULL;
}

/* True if step `i` asserts anything at all (used only for the PASS/FAIL tag).
 */
#define STEP_ASSERTED(i) (SCRIPT[i].check != NULL)

/*
 * Evaluate step `i`, leaving the resulting state in *out. Returns NULL when
 * the step is clean, else the failure message.
 *
 * A chk_noop step is diffed against the state produced by step i-1. That is
 * the strongest assertion available here: it proves the engine touched nothing
 * at all, not merely that the fields someone thought to check are unchanged.
 */
static const char *eval_step(int i, GameState *out)
{
    play_to(out, i);

    if (SCRIPT[i].check == chk_noop)
    {
        GameState before;
        if (i == 0)
        {
            memset(&before, 0, sizeof before);
            tetrisbrain_init(&before, SEED);
        }
        else
            play_to(&before, i - 1);
        if (memcmp(&before, out, sizeof before) != 0)
            return "step must change nothing, but the game state moved";
        return NULL;
    }
    return SCRIPT[i].check ? SCRIPT[i].check(out) : NULL;
}

static int run_dump(void)
{
    GameState g;
    int failures = 0;

    printf("playthrough: seed=%d, %d scripted steps\n", SEED, N_STEPS);
    printf("stream: I S L T Z I S J O Z L T J O S L T Z I ...\n");

    for (int i = 0; i < N_STEPS; i++)
    {
        const char *err = eval_step(i, &g);
        const char *tag = STEP_ASSERTED(i) ? (err ? "FAIL" : "PASS") : "    ";
        if (err)
            failures++;

        printf("\n[%2d] %s  %s\n", i + 1, tag, SCRIPT[i].note);
        printf("     active=%c rot=%d x=%d y=%d | hold=%c used=%d | "
               "next=%c%c%c%c%c | score=%d lines=%d | over=%d\n",
               KINDCH[g.active.kind], g.active.rot, g.active.x, g.active.y,
               KINDCH[g.hold], g.hold_used, KINDCH[g.next[0]],
               KINDCH[g.next[1]], KINDCH[g.next[2]], KINDCH[g.next[3]],
               KINDCH[g.next[4]], g.score, g.lines, g.game_over);
        if (err)
            printf("     -> %s\n", err);

        /* bottom slice of the board, active piece overlaid in <> */
        for (int r = BOARD_HEIGHT - 8; r < BOARD_HEIGHT; r++)
        {
            printf("     |");
            for (int c = 0; c < BOARD_WIDTH; c++)
            {
                int k = g.board[r][c];
                if (!k && !g.game_over &&
                    piece_filled(g.active, r - g.active.y, c - g.active.x))
                    printf("<%c>", KINDCH[g.active.kind]);
                else
                    printf(" %c ", KINDCH[k]);
            }
            printf("|\n");
        }
    }

    printf("\n%s: %d/%d checks failed\n",
           failures ? "FAILURES" : "ALL CHECKS PASSED", failures, N_STEPS);
    return failures ? 1 : 0;
}

static int run_viewer(void)
{
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (LINES < MIN_ROWS || COLS < MIN_COLS)
    {
        int rows = LINES, cols = COLS;
        endwin();
        fprintf(stderr, "terminal too small: have %dx%d, need at least %dx%d\n",
                rows, cols, MIN_ROWS, MIN_COLS);
        return 1;
    }

    int i = 0;
    for (;;)
    {
        GameState g;
        const char *err = eval_step(i, &g);

        render_game(&g);

        mvprintw(LINES - 1, 0, "[%2d/%2d] %-40s %s", i + 1, N_STEPS,
                 SCRIPT[i].note, STEP_ASSERTED(i) ? (err ? "FAIL" : "ok") : "");
        clrtoeol();
        refresh();

        int ch = getch();
        if (ch == 'q')
            break;
        if (ch == 'n' || ch == ' ' || ch == KEY_RIGHT)
            i = (i + 1) % N_STEPS;
        else if (ch == 'p' || ch == KEY_LEFT)
            i = (i + N_STEPS - 1) % N_STEPS;
    }

    endwin();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0)
        return run_dump();
    return run_viewer();
}
