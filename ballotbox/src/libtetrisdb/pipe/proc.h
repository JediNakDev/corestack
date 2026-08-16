#ifndef LIBTETRISDB_PIPE_PROC_H
#define LIBTETRISDB_PIPE_PROC_H

/**
 * @file proc.h
 * @brief The PipeRunner child process
 */

#include "libtetrisdb/pipe/db.h"
#include "../wire.h"

#include <stdio.h>
#include <sys/types.h>

/** One PipeRunner child and the two pipes to it. */
typedef struct
{
    pid_t pid;      /**< Child, or -1 when not running. */
    int in_fd;      /**< Statements are written here, to the child's stdin. */
    db_wire_t out; /**< Responses are read here, from the child's stdout. */
    int timeout_ms; /**< Resolved at spawn from db_opts_t.timeout_ms; reused
                       by every later exec and by close's reap. */
} db_proc_t;

/*
 * Fork and exec "java -cp <jar> simpledb.PipeRunner <dir>/catalog.txt", then
 * read the child's startup output until its "<<READY>>" line.
 */
int db_proc_spawn(db_proc_t *p, const db_opts_t *opts);

/*
 * Send one statement and consume its whole response, up to and including the
 * "<<END ...>>" marker line.
 */
int db_proc_exec(db_proc_t *p, const char *sql, char *body, size_t body_cap);

/* Close the child's stdin so it flushes and exits, then reap it. Waits for
 * the child; safe to call on an already-closed proc. */
void db_proc_close(db_proc_t *p);

/* Kill and reap the child without the clean flush. Only for a child that is
 * already unusable (failed handshake), never for normal shutdown. */
void db_proc_kill(db_proc_t *p);

#endif /* LIBTETRISDB_PIPE_PROC_H */
