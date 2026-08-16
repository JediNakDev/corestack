#ifndef TETRISD_SESSION_H
#define TETRISD_SESSION_H

/*
 * session.h - one client's game session.
 *
 * Runs in its own process, forked by tetrisd's listener thread. It owns this
 * client's board and lifecycle state, talks to the client over htttp, and
 * talks to the admin thread for room commands and garbage.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisutil/sessionstate.h"
#include "libtetrisutil/gamestate.h"

/* Everything one session owns. */
typedef struct
{
    SessionState session; /* phase, room, owner                        */
    GameState game;       /* this player's board                       */
    int client_fd;        /* htttp socket to the client                */
    int admin_fd;         /* socketpair end to the admin thread        */
    char user_name[MAX_USER_NAME];
    long long ts_start;
} Session;

/*
 * Entry point for a session process. Both fds are inherited across fork/exec
 * (passed as argv): client_fd is the client socket, admin_fd is this session's
 * end of the socketpair to the admin thread. Runs until the client leaves.
 */
void session_main(int client_fd, int admin_fd);

#endif /* TETRISD_SESSION_H */
