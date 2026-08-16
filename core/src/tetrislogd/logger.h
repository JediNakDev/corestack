#ifndef TETRISLOGD_H
#define TETRISLOGD_H

#include <limits.h>

#include "libtetrisutil/limits.h"

#include "libtetrisutil/logmsg.h"
#include "libtetrisdb/pipe/db.h"

/* Socket receive buffer. */
#define LOGD_RCVBUF (256 * 1024)

/* Period between summary */
#define LOGD_TICK_SECS 1

/* Largest drop count one record may claim = 2^24 */
#define LOGD_DROP_CLAIM_MAX (1u << 24)

/* Upper bound on the shutdown drain */
#define LOGD_DRAIN_MAX 100000

#define LOGD_DB_TABLE "log"
#define LOGD_DB_SCHEMA "id int, pid int, ts int, status string, msg string"
#define LOGD_DB_STRING_MAX 128

typedef struct
{
    char socket_path[MAX_IPC_PATH];
    char log_path[PATH_MAX];
    log_level_t min_level;
    int echo;
    int summary_secs;
    int db_enable;
    db_opts_t db;
} logd_opts_t;

typedef struct
{
    unsigned long received;
    unsigned long filtered;  /* dropped by min_level */
    unsigned long malformed; /* wrong datagram size, or unknown level */
    unsigned long truncated; /* msg was not NUL-terminated by the sender */
    unsigned long dropped;
    unsigned long db_dropped; /* records the database queue could not take */
    unsigned long db_errors;  /* INSERTs SimpleDB rejected */
} logd_stats_t;

typedef struct
{
    db_t *db;
    long next_id;
} logd_mirror_t;

typedef struct
{
    time_t opened;        /**< Start of the current window. */
    logd_stats_t at_open; /**< Stats when the window opened. */
} logd_summary_window_t;

int config(logd_opts_t *opts) __attribute__((warn_unused_result));

int logd_run(const logd_opts_t *opts, logd_stats_t *stats);

void logd_stop(void);

void logd_reopen(void);

#endif /* TETRISLOGD_H */
