#ifndef LIBTETRISDB_PIPE_DB_H
#define LIBTETRISDB_PIPE_DB_H

#include <limits.h>
#include <stddef.h>

typedef struct db db_t;

typedef struct
{
    char dir[PATH_MAX];  /**< Directory holding catalog.txt and *.dat. */
    char jar[PATH_MAX];  /**< simpledb.jar for the classpath. */
    char java[PATH_MAX]; /**< java binary, resolved through PATH. */
    size_t queue_cap; /**< Pending statements before dropping; 0 = default. */
    int timeout_ms; /**< Bounds readiness, one statement's round trip, and
                       shutdown's reap, each freshly; 0 = default. */
} db_opts_t;

/* Spawns the PipeRunner child, waits for its startup handshake, and starts the worker thread. */
db_t *db_start(const db_opts_t *opts, const char *probe, char *body,
                 size_t body_cap);

/* Queues one SQL statement for execution. */
int db_submit(db_t *db, const char *sql);

unsigned long get_db_dropped(db_t *db);

unsigned long get_db_errors(db_t *db);

/* Drains the queue, shuts the child down cleanly. */
void db_stop(db_t *db, unsigned long *dropped, unsigned long *errors);

#endif /* LIBTETRISDB_PIPE_DB_H */
