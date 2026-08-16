#include "logger.h"

#include "libtetrisutil/rc.h"

#include <limits.h>

#define DEFAULT_DB_DIR "var/db_log"
#define DEFAULT_DB_JAR "db/dist/simpledb.jar"
#define DEFAULT_DB_JAVA "java"
#define DEFAULT_DB_QUEUE_CAP 256

#define DEFAULT_SUMMARY_SECS 30
#define MAX_SUMMARY_SECS 3600

int config(logd_opts_t *opts)
{
    if (opts == NULL)
        return -1;

    if (rc_get("log_path", NULL, opts->log_path, sizeof opts->log_path) !=
            RC_VALUE_FOUND ||
        rc_get("log_ipc", NULL, opts->socket_path, sizeof opts->socket_path) !=
            RC_VALUE_FOUND)
        return -1;
    rc_get("log_db_dir", DEFAULT_DB_DIR, opts->db.dir, sizeof opts->db.dir);
    rc_get("log_db_jar", DEFAULT_DB_JAR, opts->db.jar, sizeof opts->db.jar);
    rc_get("log_db_java", DEFAULT_DB_JAVA, opts->db.java, sizeof opts->db.java);
    (void)rc_get_int("log_summary_secs", DEFAULT_SUMMARY_SECS, 0,
                     MAX_SUMMARY_SECS, &opts->summary_secs);
    (void)rc_get_bool("db", 0, &opts->db_enable);

    int queue_cap;
    (void)rc_get_int("log_db_queue", DEFAULT_DB_QUEUE_CAP, 1, INT_MAX,
                     &queue_cap);
    opts->db.queue_cap = (size_t)queue_cap;

    char level[16];
    rc_get("log_level", "info", level, sizeof level);
    if (log_level_parse(level, &opts->min_level) < 0)
        log_level_parse("info", &opts->min_level);

    return 0;
}
