/*
 * test_render_fixtures.c - renderer isolation harness.
 *
 * Unlike tests/test_gameui.c (which plays a real game through libtetrisbrain),
 * this feeds render_game() hand-built GameState structs. Nothing computes the
 * state, so anything that looks wrong on screen is a render.c bug and nothing
 * else. Step through the scenarios and eyeball each one.
 *
 * Two sections, same keys:
 *
 *   EDGE CASES  - independent one-off states (walls, colours, panels). Each
 *                 starts from a blank board.
 *   STORYBOARD  - one continuous game. Every frame is the state that FOLLOWS
 *                 the previous one, covering soft drop, rotate, hold on an
 *                 empty slot, hold as a swap, hard drop, a 4-line clear and a
 *                 top out. This catches what single snapshots cannot: the next
 *                 queue advancing, hold_used setting and clearing, and rows
 *                 shifting down after a clear.
 *
 * Frames are replayed from the start on every keypress, so stepping backwards
 * shows exactly the same state as stepping forwards.
 *
 * Build (renderer + piece table only - no brain logic, no net, no crypto):
 *   make gui
 * Run:
 *   ./bin/test_render_fixtures     (n/space next, p prev, q quit)
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <ncurses.h>
#include <stdio.h>
#include <string.h>

#include "tetrisu/render.h"
#include "libtetrisutil/gamestate.h"
#include "libtetrisbrain/piece.h" /* shape table only - no rules, no state */

/* Below this ncurses silently clips and every fixture looks broken for the
 * wrong reason, so bail out loudly instead. Rows, worst case (game over):
 * board origin 1 + 26 board rows + bottom frame + gap + banner = row 29, then
 * the controls line needs one clear row under it. Cols: board origin 2 +
 * 10 cells * 2 wide + 4 gap + the widest caption. */
#define MIN_ROWS (BOARD_HEIGHT + 5)
#define MIN_COLS RENDER_MIN_COLS

#define NEXT_LEN                                                               \
    ((int)(sizeof((GameState *)0)->next / sizeof((GameState *)0)->next[0]))

/* Line-clear scoring, mirroring scoring.c's table. Duplicated on purpose: if
 * this harness linked the brain it would stop isolating the renderer. */
static const int PTS[] = {0, 100, 300, 500, 800};

/* ---- fixture helpers ---------------------------------------------------- */

static PieceKind kind_of(char ch)
{
    switch (ch)
    {
    case 'I':
        return PIECE_I;
    case 'O':
        return PIECE_O;
    case 'T':
        return PIECE_T;
    case 'S':
        return PIECE_S;
    case 'Z':
        return PIECE_Z;
    case 'J':
        return PIECE_J;
    case 'L':
        return PIECE_L;
    case '#':
        return PIECE_GARBAGE;
    default:
        return PIECE_NONE; /* '.' or anything else */
    }
}

/* Start from a known-clean state: empty board, no piece, no hold/next. */
static void blank(GameState *g)
{
    memset(g, 0, sizeof *g);
    g->active.kind = PIECE_NONE;
    g->hold = PIECE_NONE;
    for (int i = 0; i < (int)(sizeof g->next / sizeof g->next[0]); i++)
        g->next[i] = PIECE_NONE;
    /* memset would leave this 0, which reads as "I am player 0" - the panel
     * would name an identity for a board that is not in a room at all. */
    g->my_player_id = -1;
}

/* Paint `n` rows of ASCII art onto the bottom of the board. rows[0] is the
 * highest of the group, rows[n-1] sits on the floor. Each string is one
 * character per column, '.' = empty, 'I'/'O'/'T'/'S'/'Z'/'J'/'L' = locked,
 * '#' = garbage. */
static void fill_bottom(GameState *g, const char *const *rows, int n)
{
    for (int i = 0; i < n; i++)
    {
        int r = BOARD_HEIGHT - n + i;
        for (int c = 0; c < BOARD_WIDTH && rows[i][c]; c++)
            g->board[r][c] = (Cell)kind_of(rows[i][c]);
    }
}

/*
 * 10. Garbage from an opponent: three junk rows under a live stack, all
 *     sharing the hole at column 5.
 *
 *     The one to check the garbage colour against. It sits directly beneath
 *     locked pieces of every hue and underneath the falling J's ghost, so all
 *     three treatments - solid, stipple-in-piece-colour, stipple-in-white -
 *     are on screen at once. Garbage must not read as any of: a locked piece,
 *     the ghost, or an empty cell.
 */
static void fx_garbage(GameState *g)
{
    static const char *const rows[] = {
        "..I.......", "..I..T....", "IOOZZTTTLL",
        "#####.####", "#####.####", "#####.####",
    };
    blank(g);
    fill_bottom(g, rows, 6);
    g->active = (Piece){.kind = PIECE_J, .rot = 0, .x = 4, .y = 6};
    g->hold = PIECE_S;
    g->next[0] = PIECE_Z;
    g->score = 4200;
    g->lines = 18;
    g->level = 2;
}

/* ---- state transitions (for the storyboard) ----------------------------- */
/*
 * Small reimplementations of what the brain does on lock/clear/hold. Yes, this
 * duplicates libtetrisbrain - deliberately. The whole value of this harness is
 * that a wrong-looking frame implicates render.c and nothing else, which stops
 * being true the moment it links the engine under test.
 */

/* Put a piece at the top of the buffer zone, the way a real spawn would. */
static void spawn(GameState *g, PieceKind k)
{
    g->active.kind = k;
    g->active.rot = 0;
    g->active.x = 3;
    g->active.y = 2;
}

/* Pull next[0] into play and push `incoming` onto the tail of the queue. */
static void advance_queue(GameState *g, PieceKind incoming)
{
    spawn(g, g->next[0]);
    for (int i = 0; i < NEXT_LEN - 1; i++)
        g->next[i] = g->next[i + 1];
    g->next[NEXT_LEN - 1] = incoming;
}

/* Stamp the active piece into the board and retire it. Clearing hold_used here
 * mirrors the real rule: hold unlocks again once a piece has landed. */
static void lock_piece(GameState *g)
{
    int n = piece_size(g->active);
    for (int r = 0; r < n; r++)
    {
        for (int c = 0; c < n; c++)
        {
            if (!piece_filled(g->active, r, c))
                continue;
            int br = g->active.y + r;
            int bc = g->active.x + c;
            if (br >= 0 && br < BOARD_HEIGHT && bc >= 0 && bc < BOARD_WIDTH)
                g->board[br][bc] = (Cell)g->active.kind;
        }
    }
    g->active.kind = PIECE_NONE;
    g->hold_used = false;
}

/* Remove every complete row, shifting the rest down. Returns the count. */
static int clear_full_rows(GameState *g)
{
    int cleared = 0;
    for (int r = BOARD_HEIGHT - 1; r >= 0; r--)
    {
        int full = 1;
        for (int c = 0; c < BOARD_WIDTH; c++)
        {
            if (g->board[r][c] == PIECE_NONE)
            {
                full = 0;
                break;
            }
        }
        if (!full)
            continue;

        for (int rr = r; rr > 0; rr--)
            memcpy(g->board[rr], g->board[rr - 1], sizeof g->board[rr]);
        memset(g->board[0], 0, sizeof g->board[0]);
        cleared++;
        r++; /* this index now holds the row from above; recheck it */
    }
    return cleared;
}

/* Swap the active piece with the hold slot. An empty slot pulls from the queue
 * instead - that is the case worth eyeballing, since both panels change. */
static void do_hold(GameState *g, PieceKind incoming)
{
    PieceKind was_active = g->active.kind;
    if (g->hold == PIECE_NONE)
    {
        g->hold = was_active;
        advance_queue(g, incoming);
    }
    else
    {
        PieceKind from_hold = g->hold;
        g->hold = was_active;
        spawn(g, from_hold);
    }
    g->hold_used = true;
}

/* ---- SECTION 1: independent edge cases ---------------------------------- */

/* 1. Nothing at all. Checks the frame, the empty-cell glyph, and that the
 *    buffer zone (top BOARD_BUFFER_HEIGHT rows) is distinguishable. */
static void fx_empty(GameState *g)
{
    blank(g);
}

/* 2. An I piece sitting in the spawn buffer. Checks that the active-piece
 *    overlay draws over the buffer zone, not under it. */
static void fx_spawn_in_buffer(GameState *g)
{
    blank(g);
    g->active = (Piece){.kind = PIECE_I, .rot = 0, .x = 3, .y = 1};
}

/* 3. Piece flush against the left wall (x = 0). Checks for off-by-one bleed
 *    into the frame column. */
static void fx_left_wall(GameState *g)
{
    blank(g);
    g->active = (Piece){.kind = PIECE_J, .rot = 0, .x = 0, .y = 10};
}

/* 4. Piece flush against the right wall. J has a 3-wide box, so the rightmost
 *    legal origin is BOARD_WIDTH - 3. */
static void fx_right_wall(GameState *g)
{
    blank(g);
    g->active =
        (Piece){.kind = PIECE_L, .rot = 0, .x = BOARD_WIDTH - 3, .y = 10};
}

/* 5. Every piece colour locked into the stack at once, plus holes. This is the
 *    one to check the colour pairs against - all seven must be distinct. */
static void fx_all_colors(GameState *g)
{
    static const char *const rows[] = {
        "I.O.T.S.Z.",
        "IJOLTZSJZL",
        "IJ.LT.SJ.L",
        "IJOLTZSJZL",
    };
    blank(g);
    fill_bottom(g, rows, 4);
    g->score = 12300;
    g->lines = 41;
    g->level = 5;
}

/* 6. Both side panels populated: a held piece and a full 5-deep next queue.
 *    Checks panel spacing - the NEXT previews are 3 rows apart, and an I piece
 *    is 4 rows tall, so this is where overlap would show. */
static void fx_panels_full(GameState *g)
{
    blank(g);
    g->active = (Piece){.kind = PIECE_T, .rot = 0, .x = 4, .y = 8};
    g->hold = PIECE_I;
    g->hold_used = true;
    g->next[0] = PIECE_O;
    g->next[1] = PIECE_I;
    g->next[2] = PIECE_S;
    g->next[3] = PIECE_L;
    g->next[4] = PIECE_Z;
    g->score = 900;
    g->lines = 7;
    g->level = 1;
}

/* 7. Stack grown up into the buffer zone. Checks that locked cells in the
 *    buffer rows still render, and that the dim treatment (if any) does not
 *    swallow them. */
static void fx_near_topout(GameState *g)
{
    static const char *const rows[] = {
        "ZZ......ZZ", "ZZZ....ZZZ", "ZZZZ..ZZZZ", "ZZZZZZZZZ.", "ZZZZZZZZZ.",
        "ZZZZZZZZZ.", "ZZZZZZZZZ.", "ZZZZZZZZZ.", "ZZZZZZZZZ.", "ZZZZZZZZZ.",
    };
    blank(g);
    /* 10 rows tall, but pushed up so the top of it lands inside the buffer. */
    fill_bottom(g, rows, 10);
    for (int r = BOARD_BUFFER_HEIGHT - 2; r < BOARD_HEIGHT - 10; r++)
        for (int c = 0; c < BOARD_WIDTH - 1; c++)
            g->board[r][c] = PIECE_Z;
    g->score = 88800;
    g->lines = 96;
    g->level = 9;
}

/* 8. Game over banner. Drawn below the board, so it also confirms there is
 *    vertical room left on the terminal. */
static void fx_game_over(GameState *g)
{
    static const char *const rows[] = {
        "TTTTTTTTT.",
        "TTTTTTTTT.",
    };
    blank(g);
    fill_bottom(g, rows, 2);
    g->game_over = true;
    g->score = 4200;
    g->lines = 13;
    g->level = 2;
}

/*
 * 9. The room scoreboard, with every state a row can be in.
 *
 * Rows arrive pre-ranked from the server, so they are listed here in the order
 * the renderer should print them - if the panel reorders anything, that is a
 * render.c bug. Covers: the local player highlighted at id 2, deliberately
 * mid-table so "bold" is visibly not the same thing as "top"; a finished
 * opponent dimmed but still listed, because their score is what the rest are
 * chasing; and a name longer than the column, so truncation is visible rather
 * than something that quietly overruns into the next field.
 */
static void fx_standings(GameState *g)
{
    static const char *const rows[] = {
        "..OO......",
        "..OOTT....",
    };
    blank(g);
    fill_bottom(g, rows, 2);

    g->score = 2400;
    g->lines = 12;
    g->level = 2;
    g->active = (Piece){.kind = PIECE_S, .rot = 0, .x = 4, .y = 9};
    g->hold = PIECE_L;
    g->next[0] = PIECE_I;
    g->next[1] = PIECE_T;
    g->next[2] = PIECE_O;
    g->next[3] = PIECE_J;
    g->next[4] = PIECE_Z;

    g->my_player_id = 2;
    g->standing_count = 4;
    g->standings[0] = (PlayerStanding){0, "Player 0", 8800, 31, false};
    g->standings[1] = (PlayerStanding){1, "Player 1", 5100, 22, true};
    g->standings[2] = (PlayerStanding){2, "Player 2", 2400, 12, false};
    g->standings[3] = (PlayerStanding){3, "LongNameHere", 400, 3, false};
}

/* ---- SECTION 2: one continuous game -------------------------------------- */
/*
 * Each step mutates the state left by the one before it. Only st_setup calls
 * blank(), which is what makes it the start of the sequence.
 */

/*
 * A Tetris setup: four rows complete except column 9, so a vertical I dropped
 * into that well clears all four at once. Two ragged rows underneath survive
 * the clear and should visibly fall to the floor afterwards.
 */
static void st_setup(GameState *g)
{
    static const char *const rows[] = {
        "...JJ.....", "..JJJ.....", "LLOOTTZZS.",
        "OOTTZZSSJ.", "TTZZSSJJL.", "ZZSSJJLLO.",
    };
    blank(g);
    fill_bottom(g, rows, 6);
    g->next[0] = PIECE_I;
    g->next[1] = PIECE_O;
    g->next[2] = PIECE_L;
    g->next[3] = PIECE_S;
    g->next[4] = PIECE_Z;
    g->score = 2400;
    g->lines = 12;
    g->level = 2;
    spawn(g, PIECE_T);
}

/* Soft drop is pure descent: y grows, nothing else moves. */
static void st_softdrop(GameState *g)
{
    g->active.y += 6;
}

/* Rotation bumps rot only - x/y are untouched, so the box stays put. */
static void st_rotate_cw(GameState *g)
{
    g->active.rot = 1;
}

/* Slid hard against the left wall. */
static void st_move_wall(GameState *g)
{
    g->active.x = 0;
}

/* First hold of the game: the slot is empty, so T is stored and the queue
 * feeds the next piece. HOLD fills in and NEXT shifts up by one. */
static void st_hold_empty(GameState *g)
{
    do_hold(g, PIECE_J);
}

/* I rotated upright. Its filled column is index 2 of the 4x4 box, so x = 7
 * lines it up with the well at column 9. */
static void st_line_up(GameState *g)
{
    g->active.rot = 1;
    g->active.x = 7;
}

/* Soft dropping down the well. */
static void st_descend(GameState *g)
{
    g->active.y = 14;
}

/* Hard drop: straight to the floor. Shown before the lock resolves, so the I
 * is still the active overlay sitting in the well. */
static void st_harddrop(GameState *g)
{
    g->active.y = 22;
}

/* The lock resolves it: four complete rows vanish, the ragged rows fall to the
 * floor, +800, and hold unlocks. */
static void st_lock_tetris(GameState *g)
{
    lock_piece(g);
    int n = clear_full_rows(g);
    g->score += PTS[n];
    g->lines += n;
    advance_queue(g, PIECE_T);
}

/* Second hold, with a piece already in the slot: a straight swap, and the
 * queue does NOT move. Contrast with st_hold_empty. */
static void st_hold_swap(GameState *g)
{
    do_hold(g, PIECE_NONE);
}

/* Position the T on the leftover stack, box bottom row resting on the floor. */
static void st_place_t(GameState *g)
{
    g->active.rot = 0;
    g->active.x = 5;
    g->active.y = 24;
}

/* Locks with no complete row: score unchanged, stack grows. */
static void st_lock_noclear(GameState *g)
{
    lock_piece(g);
    int n = clear_full_rows(g);
    g->score += PTS[n];
    g->lines += n;
    advance_queue(g, PIECE_S);
}

/* The stack finally breaches the buffer zone and the game ends. */
static void st_topout(GameState *g)
{
    for (int r = BOARD_BUFFER_HEIGHT - 1; r < BOARD_HEIGHT; r++)
        for (int c = 0; c < BOARD_WIDTH; c++)
            if (c != 6)
                g->board[r][c] = (Cell)(PIECE_I + ((r + c) % 7));
    g->active.kind = PIECE_NONE;
    g->game_over = true;
}

/* ---- driver ------------------------------------------------------------- */

typedef struct
{
    const char *name;
    void (*build)(GameState *);
} Fixture;

static const Fixture FIXTURES[] = {
    /* --- edge cases: each one calls blank() and stands alone --- */
    {"edge: empty board", fx_empty},
    {"edge: spawn in buffer zone", fx_spawn_in_buffer},
    {"edge: piece at left wall", fx_left_wall},
    {"edge: piece at right wall", fx_right_wall},
    {"edge: all 7 colours + holes", fx_all_colors},
    {"edge: hold + full next queue", fx_panels_full},
    {"edge: stack into buffer", fx_near_topout},
    {"edge: game over banner", fx_game_over},
    {"edge: room scoreboard", fx_standings},
    {"edge: garbage rows", fx_garbage},

    /* --- storyboard: each follows on from the one above --- */
    {"game: T spawns, well open", st_setup},
    {"game: soft drop x6", st_softdrop},
    {"game: rotate CW", st_rotate_cw},
    {"game: move to left wall", st_move_wall},
    {"game: HOLD empty - T stored", st_hold_empty},
    {"game: rotate I upright, x=7", st_line_up},
    {"game: soft drop down the well", st_descend},
    {"game: HARD DROP - I hits floor", st_harddrop},
    {"game: lock -> TETRIS x4, +800", st_lock_tetris},
    {"game: HOLD swap - O out, T in", st_hold_swap},
    {"game: position T on the stack", st_place_t},
    {"game: lock - no clear", st_lock_noclear},
    {"game: top out - GAME OVER", st_topout},
};
static const int N_FIXTURES = (int)(sizeof FIXTURES / sizeof FIXTURES[0]);

/*
 * Rebuild frame `upto` from scratch by replaying every step before it. The
 * storyboard steps are mutations, so state N only exists as the sum of 0..N.
 * Replaying rather than caching keeps backward navigation showing exactly what
 * forward navigation showed.
 */
static void build_frame(GameState *g, int upto)
{
    blank(g);
    for (int i = 0; i <= upto; i++)
        FIXTURES[i].build(g);
}

int main(void)
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
        fprintf(stderr,
                "terminal too small: have %dx%d, need at least %dx%d "
                "(rows x cols)\n",
                rows, cols, MIN_ROWS, MIN_COLS);
        return 1;
    }

    int i = 0;
    for (;;)
    {
        GameState g;
        build_frame(&g, i);

        render_game(&g); /* erases and refreshes on its own */

        /* Caption goes after render_game, which starts with erase(). One line
         * at the bottom, so it never collides with the GAME OVER banner. */
        mvprintw(LINES - 1, 0, "[%2d/%2d] %-34s n/p/q", i + 1, N_FIXTURES,
                 FIXTURES[i].name);
        clrtoeol();
        refresh();

        int ch = getch();
        if (ch == 'q')
            break;
        if (ch == 'n' || ch == ' ' || ch == KEY_RIGHT)
            i = (i + 1) % N_FIXTURES;
        else if (ch == 'p' || ch == KEY_LEFT)
            i = (i + N_FIXTURES - 1) % N_FIXTURES;
    }

    endwin();
    return 0;
}
