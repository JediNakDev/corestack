# Comment format

## File

```c
/**
 * @file db_logger.c
 * @brief Buffers log records and flushes them to Postgres.
 *
 * Used by the request handlers via log_write(); flushed by the background
 * writer thread in worker.c.
 */
```

## Header function

```c
/**
 * Appends a log record to the pending buffer.
 *
 * Called by request handlers on every completed request. The record is not
 * durable until the writer thread flushes it.
 *
 * @param rec  Record to copy; caller keeps ownership.
 * @returns 0 on success, -1 if the buffer is full (record dropped).
 */
int log_write(const log_rec_t *rec);
```

The "called by X" line is the usage context. One sentence, and only when it's not obvious from the name.

## Static function

```c
/** Serializes one record to wire format. Called by flush_batch() per row. */
static int encode_rec(const log_rec_t *rec, buf_t *out);
```

```c
/** Opens the config file. Called once from db_logger_init(). */
static FILE *open_cfg(const char *path);
```

## Variables

```c
static conn_t *g_conn;        /**< Postgres handle; opened at init, used by flush_batch(). */
static size_t  pending;       /**< Records buffered since last flush; triggers flush at BATCH_MAX. */
static time_t  last_flush;    /**< Checked by the writer thread to force a flush on idle. */

#define BATCH_MAX 256         /**< Flush threshold; tuned for one round trip per batch. */
```

Pattern for variables: **what it is** + **when/where it's used**. Two clauses, one line.
