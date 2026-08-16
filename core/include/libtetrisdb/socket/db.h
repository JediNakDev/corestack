#ifndef LIBTETRISDB_SOCKET_DB_H
#define LIBTETRISDB_SOCKET_DB_H

#include "libtetrisdb/status.h"

#include <limits.h>
#include <stddef.h>

typedef struct db_socket db_socket_t;

typedef struct
{
    char sock[PATH_MAX]; /**< socket path */
    int timeout_ms;      /**< Whole-exchange deadline; 0 means the default. */
}db_socket_opts_t;

void db_socket_opts_load(db_socket_opts_t *opts);

db_socket_t *db_socket_open(const db_socket_opts_t *opts);

db_status_t db_socket_exec(db_socket_t *c, const char *sql, char *body,
                             size_t cap);

void db_socket_close(db_socket_t *c);

/**
 * The number of row.

 * @param body  A reply body fromdb_socket_exec().
 * @returns The number of row.
 */
int db_row_count(const char *body);

/**
 * The fields of one row, as slices into body.

 * @param body  A reply body fromdb_socket_exec().
 * @param row   0-based row index.
 * @param f     Receives field starts.
 * @param len   Receives field lengths.
 * @param max   Entries available in f and len.
 * @returns The number of fields the row actually has.
 */
int db_row_fields(const char *body, int row, const char *f[], size_t len[],
                   int max);

/**
 * The id for the next row, read from a `select max(id) from <t>;` reply.
 *
 * @param body  The reply body from the max(id) select.
 * @returns max(id) + 1
 */
long long db_next_id(const char *body);

#endif /* LIBTETRISDB_SOCKET_DB_H */
