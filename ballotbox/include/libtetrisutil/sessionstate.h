#ifndef LIBTETRISUTIL_SESSIONSTATE_H
#define LIBTETRISUTIL_SESSIONSTATE_H

#include <stdbool.h>

#include "libtetrisutil/limits.h" /* MAX_USER_NAME, MAX_STANDINGS */

/* Where a client is in its lifecycle. */
typedef enum
{
    SESSION_NONE = 0,  /* no session yet                       */
    SESSION_IDLE,      /* handshake done, not in a room        */
    SESSION_WAITING,   /* in a room, waiting for game start    */
    SESSION_PLAYING,   /* game running                         */
    SESSION_GAME_OVER, /* topped out, still in the room        */
    SESSION_DISCONNECTED
} SessionPhase;

#define ROOM_NONE -1

/*
 * One player as the lobby sees them.
 *
 * Deliberately not a PlayerStanding: the waiting room has no scores to show
 * (nobody has played yet) and does need is_owner, which the scoreboard has no
 * use for. Carrying the score fields here would mean displaying zeros and
 * pretending they mean something.
 *
 * The two lists ARE the same players, so room.c fills both in a single pass
 * over its member table - see room_build_lists(). Building them separately is
 * how a name or an id would eventually differ between the lobby and the game.
 */
typedef struct
{
    int player_id; /* stable identity; matches SessionState.player_id */
    char
        name[MAX_USER_NAME]; /* NUL-terminated; "Player N" until real names */
    bool is_owner;             /* this row may START the round */
} RoomMember;

typedef struct
{
    SessionPhase phase;
    int room_id;   /* ROOM_NONE when not in a room */
    int player_id; /* unique within the room       */
    bool is_owner; /* may start the game / kick    */

    /*
     * Everyone currently in the room, in join order.
     *
     * Join order rather than ranked: in the waiting room every score is zero,
     * and a list that reshuffled as people scored would be disorienting to
     * read. The in-game scoreboard ranks; this does not.
     *
     * Rides UPD_SESSION, which is pushed whenever the membership or ownership
     * changes - exactly when this list changes - so the lobby needs no polling
     * and no separate message. Iterate to roster_count, never to MAX_STANDINGS.
     *
     * Note is_owner appears twice in this struct and both earn their place:
     * the one above is "am I the owner" and gates whether pressing s does
     * anything; the one on each row is "is this player the owner" and drives
     * the marker beside their name.
     */
    RoomMember roster[MAX_STANDINGS];
    int roster_count;
} SessionState;

#endif
