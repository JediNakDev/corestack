#ifndef LIBTETRISUTIL_GAMESTATE_H
#define LIBTETRISUTIL_GAMESTATE_H

/*
 * gamestate.h - the shared tetriSH data model.
 *
 * Plain-old-data only: no logic, no I/O, no link dependency. Lives in
 * libtetrisutil because every component needs these types - libtetrisbrain owns
 * the rules, but tetrisd serialises the state and tetrisu renders it, and all
 * of them must agree on the exact layout.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdbool.h>

#include "libtetrisutil/limits.h" /* MAX_STANDINGS, MAX_USER_NAME */

/* Playfield dimensions. Classic Tetris guideline.
 * The array is taller than the visible field: the top BOARD_BUFFER_HEIGHT
 * row are a spawn zone where pieces appear before falling into the main
 * field. Rendering of that zone (hidden, dimmed, marked) is the caller's
 * choice; the engine treats all BOARD_HEIGHT rows as playable. */
#define BOARD_WIDTH 10
#define BOARD_MAIN_HEIGHT 20  /* main playfield rows */
#define BOARD_BUFFER_HEIGHT 6 /* spawn rows on top */
#define BOARD_HEIGHT (BOARD_MAIN_HEIGHT + BOARD_BUFFER_HEIGHT)

/* The seven one-sided tetrominoes, plus an empty sentinel and garbage. */
typedef enum
{
    PIECE_NONE = 0,
    PIECE_I,
    PIECE_O,
    PIECE_T,
    PIECE_S,
    PIECE_Z,
    PIECE_J,
    PIECE_L,

    /* One past the last spawnable piece. The generator picks from
     * PIECE_I..PIECE_L, so its range must be derived from THIS and never from
     * PIECE_COUNT - anything added below would otherwise turn up in the queue
     * as a playable piece. New tetrominoes go above this line. */
    PIECE_SPAWNABLE,

    /* A junk cell pushed up from below by an opponent's line clear. Never
     * spawns and has no shape: it only ever exists as a locked cell in
     * board[][]. It is a PieceKind so that "occupied" stays one concept for
     * collision and line clearing; only the colour differs. */
    PIECE_GARBAGE,

    PIECE_COUNT /* number of distinct cell values */
} PieceKind;

/* One board cell: PIECE_NONE = empty, else the kind that locked here. */
typedef int Cell;

/* A live piece: kind, rotation index 0..3, and top-left origin (x,y). */
typedef struct
{
    PieceKind kind;
    int rot; /* rotation state 0..3 */
    int x;   /* column of piece origin (may be negative) */
    int y;   /* row of piece origin (may be negative at spawn) */
    // x and y are coordinate of the top left of the frame of the piece.
} Piece;

/* Player inputs the engine understands. */
typedef enum
{
    MOVE_NONE = 0,
    MOVE_LEFT,
    MOVE_RIGHT,
    MOVE_ROT_LEFT,
    MOVE_ROT_RIGHT,
    MOVE_SOFT_DROP,
    MOVE_HARD_DROP,
    HOLD,
    //,PAUSE //if we want
} Move;

/*
 * One row of the room's scoreboard: how a single player is doing right now.
 *
 * `name` is what the UI shows. The server fills it today by formatting
 * player_id ("Player 0"), because no client ever sends a name - but it is a
 * string rather than a derived label so that adding real usernames later is a
 * change to who WRITES this field, not a change to the wire format, the
 * renderer, or anything in between.
 *
 * `player_id` stays alongside it as the stable identity: names may later be
 * duplicated, blank or chosen by the player, so anything that needs to match a
 * row to a session (highlighting "you", ranking, tie-breaks) must key off the
 * id and never off the text.
 */
typedef struct
{
    int player_id;              /* matches SessionState.player_id     */
    char name[MAX_USER_NAME]; /* NUL-terminated display name        */
    int score;
    int lines;      /* total lines cleared                */
    bool game_over; /* finished; the UI can grey the row  */
} PlayerStanding;

/* Full authoritative game state. Passed by pointer to every engine call. */
typedef struct
{
    Cell board[BOARD_HEIGHT][BOARD_WIDTH];
    Piece active; /* currently falling piece */

    PieceKind next[5]; /* next piece to spawn */

    PieceKind hold; /* piece in hold */
    bool hold_used; /* true for already used hold. reset when a piece is locked
                     */

    int score;
    int lines; /* total lines cleared */
    int level; /* drives gravity speed */

    int tick_count;   /* accumulated ticks toward next gravity step */
    int tick_trigger; /* accumulated ticks the piece has rested */

    unsigned seed;
    unsigned piece_generated_counter;

    bool game_over;

    /*
     * Garbage rows earned and not yet sent. The engine adds to this on every
     * multi-line clear but cannot send anything, so the session drains it to
     * the admin, which routes it to an opponent. Drain by setting it to 0;
     * whatever is left stays pending.
     */
    int garbage_out;

    /*
     * The room's scoreboard, as of the last update from the server.
     *
     * NOT owned by libtetrisbrain. The engine knows about exactly one board;
     * it can neither fill these in nor validate them. tetrisd's admin thread
     * aggregates them from every session in the room and pushes them down, and
     * the session copies them in here so they ride the existing UPD_GAME frame
     * instead of needing a second delivery path to the renderer.
     *
     * The engine must therefore leave this array ALONE - including in
     * tetrisbrain_init, which resets every other field in this struct.
     *
     * Always iterate to standing_count, never to MAX_STANDINGS: a room may
     * hold more players than fit, and the remaining entries are stale.
     */
    PlayerStanding standings[MAX_STANDINGS];
    int standing_count; /* valid entries in standings[] */

    /*
     * Which of those rows is us, by player_id; -1 when not in a room.
     *
     * The renderer is handed a GameState and nothing else, so without this it
     * cannot tell the local player apart from the opponents - it could show
     * the scoreboard but not say who you are on it. Copied here by the session
     * from its SessionState rather than passed as a second argument, so
     * render_game's signature stays "draw this state" and every existing
     * caller is unaffected.
     */
    int my_player_id;
} GameState;

#endif /* LIBTETRISUTIL_GAMESTATE_H */
