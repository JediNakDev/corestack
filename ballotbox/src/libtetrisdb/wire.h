#ifndef LIBTETRISDB_WIRE_H
#define LIBTETRISDB_WIRE_H

/**
 * @file wire.h
 * @brief The line protocol, over a plain fd, shared by both runners.
 */

#include "libtetrisdb/status.h"

#include <stddef.h>

/** Longest response body kept from a statement. PipeRunner's failures are a
 * line or two and the query path's one row is shorter, so a short cap costs
 * nothing and bounds a runaway runner's output. */
#define DB_BODY_MAX 1024
#define DB_NO_DEADLINE (-1)

/** Now, on that monotonic clock. Called wherever a deadline is built. */
long long db_now_ms(void);

typedef struct
{
    int fd;
    char buf[4096]; /**< Holds bytes already read from the file descriptor. */
    size_t len; /**< Valid bytes are currently in the buffer. */
    size_t pos; /**< Next unread byte in buf. */
} db_wire_t;

enum
{
    DB_WIRE_LINE = 1, /**< A line was read. */
    DB_WIRE_EOF = 0,  /**< The peer closed cleanly. */
    DB_WIRE_IO = -1,  /**< The fd broke. */
    DB_WIRE_LATE = -2 /**< The deadline passed first. */
};

/**
 * Reads one '\n'-terminated line.
 *
 * @param w         The reader.
 * @param out       Receives the line, NUL-terminated, newline stripped. A line
 *                  longer than cap is split: the caller sees the first cap-1
 *                  bytes as a line and the rest as the next one, which cannot
 *                  corrupt marker detection because markers are short and
 *                  anchored to the start of a line.
 * @param cap       Capacity of out.
 * @param deadline  Absolute monotonic ms, or DB_NO_DEADLINE.
 * @returns One of the DB_WIRE_* values.
 */
int db_wire_line(db_wire_t *w, char *out, size_t cap, long long deadline);

/** Writes every byte, retrying short writes and waiting for room until
 * deadline. Returns 0, DB_WIRE_IO if the fd broke, or DB_WIRE_LATE. */
int db_wire_write(int fd, const char *data, size_t len, long long deadline);

/**
 * Reads one statement's whole response, up to and including its "<<END ...>>"
 * marker.
 *
 * There is no framing beyond the marker, so a caller that stops reading early
 * leaves the next statement's response misaligned - which is why this always
 * reads to the marker, and why an abandoned exchange must end the connection
 * rather than continue on it.
 *
 * @param w         The reader.
 * @param body      Receives the lines above the marker, NUL-terminated and
 *                  truncated to cap; may be NULL.
 * @param cap       Capacity of body.
 * @param deadline  Absolute monotonic ms, or DB_NO_DEADLINE.
 * @returns The marker's meaning, or DB_IO / DB_TIMEOUT if the response never
 *          arrived.
 */
db_status_t db_wire_response(db_wire_t *w, char *body, size_t cap,
                               long long deadline);

#endif /* LIBTETRISDB_WIRE_H */
