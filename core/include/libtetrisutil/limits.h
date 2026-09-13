#ifndef LIBTETRISUTIL_LIMITS_H
#define LIBTETRISUTIL_LIMITS_H

#include <sys/un.h>

/*
 * limits.h - every capacity in tetriSH, in one place.
 *
 * These used to be split by whoever happened to use them: the server table
 * sizes in tetrisd.h, the wire-facing ones in gamestate.h. That grouping hid
 * the thing that actually matters about them - how they relate to each other -
 * and forced headers to include each other for nothing more than a number
 * (sessionstate.h needed the board header purely to learn how long a name is).
 *
 * Named libtetrisutil/limits.h rather than limits.h: with -Iinclude, a bare
 * include/limits.h would be found by #include <limits.h> and shadow the C
 * standard header, which tetrisd.c relies on for PATH_MAX.
 *
 * Board DIMENSIONS deliberately stay in gamestate.h. They are the shape of the
 * board[][] member declared beside them, not a capacity, and separating a type
 * from its own dimensions makes both harder to read.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

/* ---- server capacity ---------------------------------------------------- */
/*
 * MAX_SESSIONS bounds both the admin thread's poll set and the room module's
 * client registry, so the two must agree.
 *
 * These were 254 for as long as a room id was believed to travel as a single
 * byte. It does not: JOIN names its room in the request path (/room/<id>) as
 * decimal text, and every id in the daemon, in AdminMsg and in SessionState is
 * an int. The byte was only ever in one place - client_join()'s parameter -
 * and that is now an int too, so the cap is a capacity decision and nothing
 * else.
 *
 * 1024 is that decision. Each session costs the daemon one socketpair fd and
 * one forked process, so the real ceiling is RLIMIT_NOFILE and the per-user
 * process limit; tetrisd raises its own fd limit at startup (see main()) and
 * a table this size needs ~1029 descriptors for the poll set alone.
 *
 * NOTE the consequence of MAX_ROOM_MEMBERS == MAX_SESSIONS: a room can never
 * fill before the session table does, so room_add_member's full branch is
 * unreachable as configured, and REJECT_FULL only ever fires for the table.
 * Lower MAX_ROOM_MEMBERS if you want a real per-room cap - that is the whole
 * reason these three now sit three lines apart instead of in two files.
 */
#define MAX_SESSIONS 1024     /* concurrent client sessions        */
#define MAX_ROOMS 1024        /* concurrent rooms                  */
#define MAX_ROOM_MEMBERS 1024 /* players per room (raise to scale) */

/* ---- what travels to a client ------------------------------------------- */
/*
 * How many players can be described in one message to a client.
 *
 * Far below MAX_ROOM_MEMBERS on purpose: an array this size is copied into
 * frames the server sends continuously, so its size is paid over and over.
 * Eight covers any room anyone will actually sit in; a larger room simply
 * reports its first eight, and recipients iterate the accompanying count
 * rather than this constant.
 */
#define MAX_STANDINGS 8

/* Display name, including the terminating NUL. */
#define MAX_USER_NAME 16

/* ---- what a directive can hold ------------------------------------------ */
/*
 * A unix socket path, including the terminating NUL.
 *
 * Every ipc directive lands in a buffer this size rather than PATH_MAX, so
 * "too long to bind" is caught by the ordinary "too long for out" check in
 * rc_get() instead of by a second length rule each reader carries. sun_path is
 * The capacity is taken from this platform's sockaddr_un. Configuration is
 * validated where it is consumed, so Linux can use all 107 pathname bytes
 * without making a Linux-only valid configuration fail to load.
 */
#define MAX_IPC_PATH sizeof(((struct sockaddr_un *)0)->sun_path)

#endif /* LIBTETRISUTIL_LIMITS_H */
