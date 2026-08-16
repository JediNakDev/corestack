/*
 * render.c - ncurses board renderer (pure GUI, no networking).
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <ncurses.h>

#include "tetrisu/render.h"
#include "libtetrisbrain/piece.h"
#include "libtetrisbrain/tetrisbrain.h" /* tetrisbrain_ghost (pure query) */

/* Cell width lives in render.h - callers size their terminal check off it. */
#define CELL_W RENDER_CELL_W

/*
 * Two colour pairs per piece.
 *
 * SOLID - the piece colour as BACKGROUND, drawn as two blank spaces. A filled
 *         block of colour: what a real, committed piece looks like.
 * PALE  - the same colour as FOREGROUND on black, drawn as ACS_CKBOARD (the
 *         terminal's stipple glyph). Reads as a washed-out, see-through
 *         version of the same block, so it is obviously the same piece and
 *         obviously not solid.
 *
 * Pale is deliberately NOT "solid block + A_DIM": dimming a block means
 * dimming its background colour, and most terminals ignore that outright, so
 * the two states would look identical on exactly the setups that matter.
 * Switching to a foreground glyph makes the difference structural, not a
 * shading hint the terminal is free to drop.
 */
enum
{
    CP_I = 1,
    CP_O,
    CP_T,
    CP_S,
    CP_Z,
    CP_J,
    CP_L,
    CP_FRAME,
    CP_BUFFER,
    CP_GARBAGECOL,
    CP_PALE_I,
    CP_PALE_O,
    CP_PALE_T,
    CP_PALE_S,
    CP_PALE_Z,
    CP_PALE_J,
    CP_PALE_L,
    CP_GARBAGE /* must stay after the pale block: see CP_PALE_OFFSET */
};

/* Distance from a solid pair to its pale twin, so one lookup serves both. */
#define CP_PALE_OFFSET (CP_PALE_I - CP_I)

/*
 * Pale uses the piece's EXACT colour - no second palette, no blending.
 *
 * A solid block is the colour as background, so every pixel of the cell is
 * inked. A pale block is the same colour as foreground drawn with ACS_CKBOARD,
 * the terminal's checkerboard glyph: roughly half the cell is the piece colour
 * and half is the black background showing through. Same hue, half the
 * coverage - which is what "not really there" should look like.
 *
 * Doing this by COVERAGE rather than by choosing a lighter or darker colour is
 * deliberate. A blended colour has to know its background - a lighter tint
 * reads as translucent over a white page, a darker one over a black page - so
 * it can be wrong in a way that is easy to get backwards. Half coverage of the
 * true colour reads correctly over either, needs no palette beyond the
 * original eight, and uses a glyph from the ancient ACS set that works on any
 * terminal without a UTF-8 locale or the wide-character ncurses build.
 */

/*
 * Set up the colour pairs once (start_color must run after initscr).
 *
 * tetrisui_init() already does this for the real client; it is repeated here so
 * the standalone harnesses (tests/test_render_*.c, test_gameui.c), which call
 * initscr() themselves and never touch libtetrisui, still get a palette.
 *
 * use_default_colors() has to be re-asserted after start_color(), because
 * start_color() resets pair 0 to an explicit white-on-black - and pair 0 is
 * what every unstyled character on screen is drawn in.
 */
static void init_colors(void)
{
    static int done = 0;
    if (done)
        return;
    done = 1;
    if (!has_colors())
        return;
    start_color();
    use_default_colors();
    init_pair(CP_I, COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_O, COLOR_BLACK, COLOR_YELLOW);
    init_pair(CP_T, COLOR_BLACK, COLOR_MAGENTA);
    init_pair(CP_S, COLOR_BLACK, COLOR_GREEN);
    init_pair(CP_Z, COLOR_BLACK, COLOR_RED);
    init_pair(CP_J, COLOR_BLACK, COLOR_BLUE);
    init_pair(CP_L, COLOR_BLACK, COLOR_WHITE);
    /*
     * -1 is "the terminal's own background", not black.
     *
     * Empty cells are drawn in pair 0, which use_default_colors() keeps on the
     * terminal default. Hard-coding COLOR_BLACK here would make the walls and
     * the buffer zone disagree with the board they enclose on any terminal
     * whose background is not pure black.
     */
    init_pair(CP_FRAME, COLOR_WHITE, -1);
    init_pair(CP_BUFFER, COLOR_WHITE, -1);
    init_pair(CP_GARBAGECOL, COLOR_RED, -1);

    /* Pale twins: same colour, foreground instead of background. Note these
     * mirror the solid pairs above exactly - swap the two arguments and you
     * get one from the other. */
    init_pair(CP_PALE_I, COLOR_CYAN, -1);
    init_pair(CP_PALE_O, COLOR_YELLOW, -1);
    init_pair(CP_PALE_T, COLOR_MAGENTA, -1);
    init_pair(CP_PALE_S, COLOR_GREEN, -1);
    init_pair(CP_PALE_Z, COLOR_RED, -1);
    init_pair(CP_PALE_J, COLOR_BLUE, -1);
    init_pair(CP_PALE_L, COLOR_WHITE, -1);

    /* Garbage: white, stippled rather than solid so it reads as rubble. */
    init_pair(CP_GARBAGE, COLOR_WHITE, -1);
}

static int piece_color(PieceKind k)
{
    switch (k)
    {
    case PIECE_I:
        return CP_I;
    case PIECE_O:
        return CP_O;
    case PIECE_T:
        return CP_T;
    case PIECE_S:
        return CP_S;
    case PIECE_Z:
        return CP_Z;
    case PIECE_J:
        return CP_J;
    case PIECE_L:
        return CP_L;
    default:
        return 0;
    }
}

/*
 * A pale block of piece k at screen (y, x): the SAME colour as the solid
 * version, just less of it. The stipple glyph inks only part of each cell, so
 * an I still reads as cyan and a Z still reads as red - it looks translucent
 * rather than faded.
 *
 * No A_DIM here on purpose: A_DIM pulls the colour toward grey, which would
 * throw away the one thing the player needs from a pale block - which piece
 * it is. Paleness comes from coverage, not from draining the hue.
 */
static void draw_pale_cell(int y, int x, PieceKind k)
{
    int cp = piece_color(k);
    if (!cp)
        return;
    attron(COLOR_PAIR(cp + CP_PALE_OFFSET));
    for (int i = 0; i < CELL_W; i++)
        mvaddch(y, x + i, ACS_CKBOARD); /* half colour, half background */
    attroff(COLOR_PAIR(cp + CP_PALE_OFFSET));
}

/* Draw one board cell at screen (y, x). PIECE_NONE = empty. `dim` marks the
 * spawn buffer so the player can tell it apart: only the empty-cell grid is
 * dimmed, never a piece - a block sitting in the buffer still has to be
 * clearly readable, that is the whole point of showing the buffer at all. */
static void draw_cell(int y, int x, PieceKind k, int dim)
{
    /* piece_color() has no pair for garbage; without this it would fall
     * through to the empty branch and a blocking row would look open. */
    if (k == PIECE_GARBAGE)
    {
        attron(COLOR_PAIR(CP_GARBAGE));
        for (int i = 0; i < CELL_W; i++)
            mvaddch(y, x + i, ACS_CKBOARD);
        attroff(COLOR_PAIR(CP_GARBAGE));
        return;
    }

    int cp = piece_color(k);
    if (cp)
    {
        attron(COLOR_PAIR(cp));
        for (int i = 0; i < CELL_W; i++)
            mvaddch(y, x + i, ' '); /* coloured spaces = a solid block */
        attroff(COLOR_PAIR(cp));
    }
    else
    {
        /* Empty cell: blanks with one dot. ACS_BULLET rather than '.' because
         * a period sits on the baseline, at the BOTTOM of its character cell,
         * whereas the bullet is centred vertically - so it reads as marking
         * the cell rather than sitting under it.
         *
         * Horizontally there is no true middle to aim for: CELL_W is even, so
         * the cell's midpoint falls on the seam between its two characters and
         * no single glyph can occupy it. Left column chosen over right; both
         * are equally off-centre, just mirrored. */
        if (dim)
            attron(A_DIM);
        for (int i = 0; i < CELL_W; i++)
            mvaddch(y, x + i, ' ');
        mvaddch(y, x, ACS_BULLET);
        if (dim)
            attroff(A_DIM);
    }
}

/* What to draw in one board cell: which piece, and whether it is real. */
typedef struct
{
    PieceKind kind;
    int pale; /* 1 = landing preview, not an actual block */
} CellView;

/*
 * What covers board cell (r, c), in strict precedence order:
 *
 *   1. the active piece   - solid; checked first so it always wins
 *   2. a locked cell      - solid
 *   3. the ghost          - pale
 *   4. nothing            - empty
 *
 * The active piece beating the ghost is what makes a near-landed piece look
 * right: where the two overlap the cell is solid, and only the part of the
 * ghost still below the piece stays pale. A ghost cell can never collide with
 * a locked cell - the ghost sits where the piece can legally rest, so those
 * cells are empty by construction - but the order is written defensively.
 *
 * `ghost` is NULL when there is nothing to preview.
 */
static CellView cell_at(const GameState *g, const Piece *ghost, int r, int c)
{
    /* piece_filled takes box-local coordinates and safely returns false for
     * anything outside the piece's box. */
    if (piece_filled(g->active, r - g->active.y, c - g->active.x))
        return (CellView){g->active.kind, 0};

    if (g->board[r][c] != PIECE_NONE)
        return (CellView){(PieceKind)g->board[r][c], 0};

    if (ghost && piece_filled(*ghost, r - ghost->y, c - ghost->x))
        return (CellView){ghost->kind, 1};

    return (CellView){PIECE_NONE, 0};
}

/* Draw a small preview of a piece's spawn shape at screen (y, x).
 *
 * `disabled` draws the piece pale - in its own colour, just not solid - which
 * is the "you already used hold this turn" state. Keeping the piece's colour
 * matters: the player still has to read at a glance WHICH piece is parked. */
static void draw_preview(int y, int x, PieceKind k, int disabled)
{
    if (k == PIECE_NONE)
        return;
    Piece p = {.kind = k, .rot = 0, .x = 0, .y = 0};
    int n = piece_size(p);
    for (int r = 0; r < n; r++)
    {
        for (int c = 0; c < n; c++)
        {
            if (!piece_filled(p, r, c))
                continue;
            int sy = y + r, sx = x + c * CELL_W;
            if (disabled)
                draw_pale_cell(sy, sx, k);
            else
                draw_cell(sy, sx, k, 0);
        }
    }
}

void render_game(const GameState *g)
{
    init_colors(); /* idempotent */
    erase();

    const int oy = 1; /* board origin on screen */
    const int ox = 2;

    /*
     * Landing preview. Derived fresh every frame rather than stored: it
     * changes on every move, rotate and gravity tick, so keeping a copy in
     * sync would cost more than the handful of collision checks it takes to
     * just ask. Suppressed once the piece has reached its landing row - at
     * that point the ghost sits exactly under the piece and adds nothing.
     */
    Piece ghost = tetrisbrain_ghost(g);
    int show_ghost =
        g->active.kind != PIECE_NONE && !g->game_over && ghost.y != g->active.y;

    /* board: every row, buffer zone dimmed */
    for (int r = 0; r < BOARD_HEIGHT; r++)
    {
        int dim = (r < BOARD_BUFFER_HEIGHT);
        for (int c = 0; c < BOARD_WIDTH; c++)
        {
            CellView v = cell_at(g, show_ghost ? &ghost : NULL, r, c);
            if (v.pale)
                draw_pale_cell(oy + r, ox + c * CELL_W, v.kind);
            else
                draw_cell(oy + r, ox + c * CELL_W, v.kind, dim);
        }
    }

    /*
     * Frame the PLAYFIELD, not the array. The top BOARD_BUFFER_HEIGHT rows are
     * a spawn zone, not space the player can use - so they get no side walls
     * and no lid above them. Boxing all BOARD_HEIGHT rows in, as this used to,
     * reads as a 26-row board and invites players to plan into rows that are
     * really just the spawn area. Walls begin at the first main-field row and
     * the only horizontal rule is the floor.
     */
    attron(COLOR_PAIR(CP_FRAME));
    for (int r = BOARD_BUFFER_HEIGHT; r < BOARD_HEIGHT; r++)
    {
        mvaddstr(oy + r, ox - 1, "|");
        mvaddstr(oy + r, ox + BOARD_WIDTH * CELL_W, "|");
    }

    mvhline(oy + BOARD_HEIGHT, ox - 1, '-', 5 * CELL_W + 1 );

    
    attroff(COLOR_PAIR(CP_FRAME));
    attron(COLOR_PAIR(CP_GARBAGECOL));
    mvhline(oy + BOARD_HEIGHT, ox + 5* CELL_W , '-', CELL_W );
    attroff(COLOR_PAIR(CP_GARBAGECOL));

    attron(COLOR_PAIR(CP_FRAME));
    //mvhline(oy + BOARD_HEIGHT, ox - 1, '-', BOARD_WIDTH * CELL_W + 2);
    mvhline(oy + BOARD_HEIGHT, ox + 6* CELL_W , '-', 4 * CELL_W + 1);
    
    attroff(COLOR_PAIR(CP_FRAME));

    /* side panels */
    int px = ox + BOARD_WIDTH * CELL_W + 4;

    mvprintw(oy + 0, px, "SCORE %d", g->score);
    mvprintw(oy + 1, px, "LINES %d", g->lines);
    mvprintw(oy + 2, px, "LEVEL %d", g->level);

    /* Hold greys out once used, and stays that way until a piece locks. */
    mvprintw(oy + 4, px, "HOLD");
    draw_preview(oy + 5, px, g->hold, g->hold_used);

    mvprintw(oy + 10, px, "NEXT");
    int ny = oy + 11;
    for (int i = 0; i < (int)(sizeof g->next / sizeof g->next[0]); i++)
    {
        draw_preview(ny, px, g->next[i], 0);
        ny += 3; /* space each preview vertically */
    }

    /*
     * Right-hand column: who you are, then the room's scoreboard.
     *
     * Its own column rather than beneath NEXT, which already runs to the
     * bottom of the board. The server sends the rows pre-ranked, so this only
     * prints them - if the order ever looks wrong, the bug is in room.c's
     * standing_ranks_before and not here.
     */
    int sx = px + RENDER_STATS_COLS;

    /* Identity first. The scoreboard shows everyone by name, so without this
     * there is nothing on screen saying which of them is you. */
    const char *me = NULL;
    for (int i = 0; i < g->standing_count && i < MAX_STANDINGS; i++)
        if (g->standings[i].player_id == g->my_player_id)
            me = g->standings[i].name;

    if (g->my_player_id >= 0)
        mvprintw(oy + 0, sx, "YOU  %d %-8.8s", g->my_player_id, me ? me : "");
    else
        mvprintw(oy + 0, sx, "YOU  -");

    /* Iterating to standing_count and not MAX_STANDINGS matters: a bigger room
     * sends only its top rows, and the rest of the array is stale. */
    mvprintw(oy + 2, sx, "ROOM");
    mvprintw(oy + 3, sx, "%-2s %-8s %5s %3s", "ID", "PLAYER", "SCORE", "LN");

    for (int i = 0; i < g->standing_count && i < MAX_STANDINGS; i++)
    {
        const PlayerStanding *st = &g->standings[i];
        int mine = (st->player_id == g->my_player_id);

        /* Your own row stands out; a finished player is dimmed rather than
         * dropped, because their final score is what the rest are chasing. */
        if (mine)
            attron(A_BOLD);
        if (st->game_over)
            attron(A_DIM);

        mvprintw(oy + 4 + i, sx, "%-2d %-8.8s %5d %3d", st->player_id, st->name,
                 st->score, st->lines);

        if (st->game_over)
            attroff(A_DIM);
        if (mine)
            attroff(A_BOLD);
    }

    if (g->game_over)
        mvprintw(oy + BOARD_HEIGHT + 2, ox, "GAME OVER - press q");

    refresh();
}
