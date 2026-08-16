#ifndef LIBTETRISDB_STATUS_H
#define LIBTETRISDB_STATUS_H

typedef enum
{
    DB_OK = 0,  /**< <<END ok>>: the statement succeeded. */
    DB_RETRY,   /**< <<END retry>>: deadlock victim, resubmit it. */
    DB_ERROR,   /**< <<END error>>: the statement is wrong; do not retry. */
    DB_TIMEOUT, /**< The deadline passed with the exchange unfinished. */
    DB_IO       /**< The runner died, or the socket broke. */
} db_status_t;

#endif /* LIBTETRISDB_STATUS_H */
