#ifndef TETRISD_ROOM_H
#define TETRISD_ROOM_H

/*
 * room.h - room table and message routing for the admin thread.
 *
 * Owns the admin-side view of every session (fd, pid, room, player, owner)
 * and every room. Only the admin thread calls these, so no locking is needed.
 * The module sends AdminMsg replies straight to the relevant session fds.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <sys/types.h> /* pid_t */
#include "tetrisd/adminmsg.h"

/* A newly forked session, announced by the listener. */
void client_add(int fd, pid_t pid);

/* One message arrived from the session on `fd`. Dispatches by msg->type to the
 * right room operation (join / leave / start / garbage / gameover). */
void client_handle(int fd, const AdminMsg *msg);

/* Publish room-wide state accumulated while the admin thread processed its
 * current batch of ready session messages. */
void room_flush_updates(void);

/* The session on `fd` is gone (exited/disconnected): drop it from its room,
 * reassign ownership if it was the owner, and notify its roommates. */
void client_close(int fd);

/*
 * Next session whose socketpair filled up, or -1 when there are none.
 *
 * The admin's ends are non-blocking so that one client which stops reading
 * cannot stall the thread serving every room. When a write to such a session
 * would block it is recorded here instead, because room.c must not close the
 * fd itself - the admin owns the poll set. Drain this each pass and
 * drop_session() what it returns.
 */
int client_take_wedged(void);

/* ---- read-only views, for the control plane ----------------------------- */
/*
 * These exist so tetrisctl can be answered without g_clients/g_rooms leaving
 * this file. Handing out a pointer to the tables would make "only the admin
 * thread touches them" a promise the compiler stops helping to keep; copying
 * out a snapshot keeps that invariant local to room.c.
 *
 * Called from the admin thread like everything else here - the ctl thread
 * parses requests but never reads room state.
 */

/* One room, as an administrator sees it. */
typedef struct
{
    int id;
    int phase; /* SessionPhase: WAITING or PLAYING */
    int members;
    int owner_player; /* player_id of the owner, -1 if none */
} RoomInfo;

/* One connected session, as an administrator sees it. */
typedef struct
{
    int room;   /* ROOM_NONE when not in a room */
    int player; /* player_id, unique only within the room */
    pid_t pid;
    int is_owner;
    int score;
    int lines;
    char name[MAX_USER_NAME];
} PlayerInfo;

/* How many session slots / rooms are currently in use. */
int client_count(void);
int room_count(void);

/* Copy out at most `max` entries. Returns how many were written. */
int room_snapshot(RoomInfo *out, int max);
int player_snapshot(PlayerInfo *out, int max);

/*
 * The session fd of one player, or -1 if that room or player is unknown.
 *
 * Room-scoped because player_id is only unique within a room - room 1 and
 * room 3 both have a player 0, so a bare player id cannot name a target.
 */
int client_fd_by_player(int room_id, int player_id);

/* The process id behind a session fd, or -1 if the fd is not a live session. */
pid_t client_pid_of(int fd);

#endif /* TETRISD_ROOM_H */
