#ifndef LIBTETRISUTIL_LOGMSG_H
#define LIBTETRISUTIL_LOGMSG_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "libtetrisutil/rc.h"

#define LOG_MSG_MAX 256

typedef enum
{
    LOG_INFO = 0,
    LOG_WARN = 1,
    LOG_ERROR = 2,
    LOG_DEBUG = 3
} log_level_t;

typedef struct
{
    pid_t pid; /* sender's getpid() */
    log_level_t level;
    unsigned long dropped; /* every drop this sender has counted but hasn't yet
                              handed to tetrislogd, piggy backing log */
    char msg[LOG_MSG_MAX];
} log_msg_t;

/* === Level helpers (no I/O, safe anywhere) === */

/* Severity rank for filtering: DEBUG < INFO < WARN < ERROR. An out-of-range
 * level ranks highest, so a corrupt record is never silently filtered out. */
int log_level_rank(log_level_t level);

/* Parse a level name, case-insensitive ("info", "WARN", "error", "debug"). */
int log_level_parse(const char *name, log_level_t *out);

/* === Sender side === */

int log_open(const char *socket_path);

int log_send(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* va_list form of log_send(), for callers with their own varargs wrapper. */
int log_vsend(log_level_t level, const char *fmt, va_list ap);

unsigned long get_log_dropped(void);

void log_close(void);

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

#endif /* LIBTETRISUTIL_LOGMSG_H */
