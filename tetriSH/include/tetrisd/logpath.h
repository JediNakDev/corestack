#ifndef TETRISD_LOGPATH_H
#define TETRISD_LOGPATH_H

/**
 * @file logpath.h
 * @brief Opening the tetrislogd sender the way the rest of the server does.
 *
 * log_open() (libtetrisutil/logmsg.h) takes a socket path and does not go looking
 * for one - deliberately, because libtetrisutil owns no notion of a project root
 * (ADR 0003). Every server-side binary therefore has to answer the same
 * question before it can log: where is the socket tetrislogd bound?
 *
 * Answering it three times in three mains is how two of them end up
 * disagreeing, so it is answered once, here, exactly as
 * tetrisctl/control_plane.h answers it for ctl_ipc. Include this and call
 * log_open_configured() - one call, at the top of main, and the process is a
 * sender.
 *
 * Used by tetrisd and tetrisctl. bin/session wants it too: it is exec'd, so it
 * inherits no sender and its libtetrisauth records go nowhere until it makes
 * this call for itself.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"

#define LOG_SOCK_DEFAULT "var/run/tetrislogd.sock"

static inline void log_socket_path(char *out, size_t out_len)
{
    (void)rc_get("log_ipc", LOG_SOCK_DEFAULT, out, out_len);
}

static inline int log_open_configured(void)
{
    char sock[PATH_MAX];
    log_socket_path(sock, sizeof sock);
    return log_open(sock);
}

#endif /* TETRISD_LOGPATH_H */
