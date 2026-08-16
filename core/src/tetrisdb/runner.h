#ifndef LIBTETRISDB_SOCKET_RUNNER_H
#define LIBTETRISDB_SOCKET_RUNNER_H

#include <limits.h>

#include "libtetrisutil/limits.h"
#include <stddef.h>
#include <sys/types.h>
#include <sys/un.h>

typedef struct
{
    char dir[PATH_MAX];  /**< Data directory holding catalog.txt and *.dat. */
    char jar[PATH_MAX];  /**< simpledb.jar for the classpath. */
    char java[PATH_MAX]; /**< java binary, resolved through PATH. */
    char err_path[PATH_MAX]; /**< File receiving the runner's stderr. */
    char ipc[MAX_IPC_PATH]; /**< Unix socket to bind; the db_ipc rc key. */
    int sessions;           /**< Concurrent sessions served; 0 = the default. */
    int recover;            /**< Run the startup recovery pass; see below. */
} db_runner_opts_t;

/* Takes the directory lock that makes one runner per data directory. */
int db_runner_lock(const char *db_dir, char *path, size_t cap)
    __attribute__((warn_unused_result));

/* Forks, detaches and execs the runner. */
pid_t db_runner_spawn(const db_runner_opts_t *opts, int err_fd);

/* Waits until the runner answers on ipc. */
int db_runner_wait(const char *ipc, pid_t pid, int timeout_ms);

#endif /* LIBTETRISDB_SOCKET_RUNNER_H */
