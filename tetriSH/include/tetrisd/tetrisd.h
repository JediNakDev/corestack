#ifndef TETRISD_H
#define TETRISD_H

/*
 * tetrisd.h - server-wide declarations shared by tetrisd.c and room.c.
 *
 * The capacity constants that used to live here (MAX_SESSIONS, MAX_ROOMS,
 * MAX_ROOM_MEMBERS) moved to libtetrisutil/limits.h, alongside the ones the client
 * also needs. Keeping them together is what makes their relationships legible -
 * see the note there about MAX_ROOM_MEMBERS and MAX_SESSIONS being equal.
 *
 * Included here so the server files keep compiling unchanged.
 */

#include "libtetrisutil/limits.h"

#endif /* TETRISD_H */
