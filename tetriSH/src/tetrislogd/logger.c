#include "logger.h"

#include "libtetrisdb/pipe/db.h"
#include "libtetrisdb/schema.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t sig_stop;
static volatile sig_atomic_t sig_reopen;

void logd_stop(void)
{
    sig_stop = 1;
}
void logd_reopen(void)
{
    sig_reopen = 1;
}

static int bind_to_socket(const char *path)
{
    struct sockaddr_un addr;
    struct stat st;

    if (strlen(path) >= sizeof(addr.sun_path))
    {
        fprintf(stderr, "tetrislogd: socket path too long (max %zu): %s\n",
                sizeof(addr.sun_path) - 1, path);
        return -1;
    }

    if (lstat(path, &st) == 0)
    {
        if (!S_ISSOCK(st.st_mode))
        {
            fprintf(stderr, "tetrislogd: refusing to replace non-socket %s\n",
                    path);
            return -1;
        }
        if (unlink(path) < 0)
        {
            fprintf(stderr, "tetrislogd: cannot remove stale socket %s: %s\n",
                    path, strerror(errno));
            return -1;
        }
    }
    /* Error NO ENTry */
    else if (errno != ENOENT)
    {
        fprintf(stderr, "tetrislogd: cannot stat %s: %s\n", path,
                strerror(errno));
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        fprintf(stderr, "tetrislogd: socket: %s\n", strerror(errno));
        return -1;
    }
    /* close-on-exec flag: if later run exec() close this fd */
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    int bufsz = LOGD_RCVBUF;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    /* Set timer for periodic summary */
    struct timeval tick = {LOGD_TICK_SECS, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tick, sizeof(tick));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "tetrislogd: bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    /* bind() applies the umask, which would typically leave the socket
     * unwritable by other users. Every local tetriSH process must be able to
     * report, so widen it explicitly; the socket only ever accepts fixed-size
     * records and grants no read access to the log itself. */
    if (chmod(path, 0666) < 0)
        fprintf(stderr, "tetrislogd: chmod %s: %s (senders may be denied)\n",
                path, strerror(errno));

    return fd;
}

/* Open (or reopen) the log file in append mode. */
static FILE *open_log_file(const char *path)
{
    if (strcmp(path, "-") == 0)
        return stdout;

    /* O_APPEND makes every write atomic with respect to other appenders, so a
     * rotation script or a second daemon cannot overwrite our records. 0640:
     * logs carry usernames and addresses, so keep them off world-readable. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0)
    {
        fprintf(stderr, "tetrislogd: open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    FILE *f = fdopen(fd, "a");
    if (f == NULL)
    {
        fprintf(stderr, "tetrislogd: fdopen %s: %s\n", path, strerror(errno));
        close(fd);
        return NULL;
    }
    return f;
}

static void close_log_file(FILE *f)
{
    if (f != NULL && f != stdout)
        fclose(f);
}

/* Seed id for database return max(id) + 1 */
static long db_seed_id(const char *body)
{
    const char *p = body;
    int after_rule = 0;

    while (*p != '\0')
    {
        const char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

        if (after_rule)
        {
            char *end;
            long v = strtol(p, &end, 10);
            if (end != p && v >= 0)
                return v + 1;
        }
        else if (len >= 3 && strncmp(p, "---", 3) == 0)
        {
            after_rule = 1;
        }

        if (eol == NULL)
            break;
        p = eol + 1;
    }
    return 1;
}

static void open_db(logd_mirror_t *mirror, const logd_opts_t *opts)
{
    char body[1024];

    mirror->db = NULL;
    mirror->next_id = 1;

    if (!opts->db_enable)
        return;

    if (db_ensure_table(opts->db.dir, LOGD_DB_TABLE, LOGD_DB_SCHEMA) < 0)
    {
        fprintf(stderr, "tetrislogd: database mirroring disabled\n");
        return;
    }

    mirror->db = db_start(&opts->db, "select max(id) from " LOGD_DB_TABLE ";",
                          body, sizeof(body));
    if (mirror->db == NULL)
    {
        fprintf(stderr, "tetrislogd: database mirroring disabled\n");
        return;
    }
    mirror->next_id = db_seed_id(body);
}

static const char *log_level_to_str(log_level_t level)
{
    switch (level)
    {
    case LOG_INFO:
        return "INFO ";
    case LOG_WARN:
        return "WARN ";
    case LOG_ERROR:
        return "ERROR";
    case LOG_DEBUG:
        return "DEBUG";
    }
    return "?????";
}

static void remove_level_word_padding(log_level_t level, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", log_level_to_str(level));
    for (size_t i = strlen(buf); i > 0 && buf[i - 1] == ' '; i--)
        buf[i - 1] = '\0';
}

static void mirror_log_to_db(logd_mirror_t *mirror, const log_msg_t *m,
                             time_t now)
{
    if (mirror->db == NULL)
        return;

    char status[16];
    remove_level_word_padding(m->level, status, sizeof(status));

    char text[LOGD_DB_STRING_MAX + 1];
    snprintf(text, sizeof(text), "%s", m->msg);

    /* Worst case every character is a quote and doubles. */
    char q_status[sizeof(status) * 2 + 3];
    char q_text[sizeof(text) * 2 + 3];
    db_quote(q_status, sizeof(q_status), status);
    db_quote(q_text, sizeof(q_text), text);

    char sql[LOGD_DB_STRING_MAX * 2 + 256];
    snprintf(sql, sizeof(sql),
             "insert into " LOGD_DB_TABLE " values (%ld, %d, %ld, %s, %s);",
             mirror->next_id, (int)m->pid, (long)now, q_status, q_text);

    (void)db_submit(mirror->db, sql);
    mirror->next_id++;
}

/* Format the local time of now as "YYYY-MM-DD HH:MM:SS". */
static void stamp(time_t now, char *buf, size_t cap)
{
    struct tm tm;

    if (localtime_r(&now, &tm) == NULL ||
        strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm) == 0)
        snprintf(buf, cap, "0000-00-00 00:00:00"); /* clock unusable */
}

static int emit(FILE *log, const logd_opts_t *opts, logd_mirror_t *mirror,
                const log_msg_t *m)
{
    time_t now = time(NULL);
    char ts[32];
    stamp(now, ts, sizeof(ts));

    if (fprintf(log, "[%s] [%s] pid=%d: %s\n", ts, log_level_to_str(m->level),
                (int)m->pid, m->msg) < 0 ||
        fflush(log) != 0)
    {
        fprintf(stderr, "tetrislogd: writing %s: %s\n", opts->log_path,
                strerror(errno));
        return -1;
    }

    mirror_log_to_db(mirror, m, now);

    if (opts->echo)
        fprintf(stderr, "[%s] [%s] pid=%d: %s\n", ts,
                log_level_to_str(m->level), (int)m->pid, m->msg);
    return 0;
}

static int emit_self(FILE *log, const logd_opts_t *opts, logd_mirror_t *mirror,
                     log_level_t level, const char *text)
{
    log_msg_t m;

    memset(&m, 0, sizeof(m));
    m.pid = getpid();
    m.level = level;
    snprintf(m.msg, sizeof(m.msg), "%s", text);
    return emit(log, opts, mirror, &m);
}

/* Sanitizes an untrusted string */
static void sanitise(char *s)
{
    for (; *s != '\0'; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) /* < 0x20 are empty space and 0x7f is DEL */
            *s = '.';
    }
}

/* Read 1 log from socket, then emit it. */
static int serve_one(int socket_fd, FILE *log, const logd_opts_t *opts,
                     logd_mirror_t *mirror, logd_stats_t *st, int flags)
{
    union
    {
        log_msg_t m;
        char raw[sizeof(log_msg_t) + 1];
    } buf;

    ssize_t n =
        recvfrom(socket_fd, buf.raw, sizeof(buf.raw), flags, NULL, NULL);
    if (n < 0)
    {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; /* signal arrived, or nothing queued */
        fprintf(stderr, "tetrislogd: recvfrom: %s\n", strerror(errno));
        return -1;
    }

    if (n != (ssize_t)sizeof(log_msg_t))
    {
        st->malformed++;
        return 1;
    }

    /* every drop this sender has counted but hasn't yet handed to tetrislogd,
       piggy back log */
    if (buf.m.dropped > LOGD_DROP_CLAIM_MAX)
        st->malformed++;
    else
        st->dropped += buf.m.dropped;

    /* Add \0 at the end */
    if (memchr(buf.m.msg, '\0', sizeof(buf.m.msg)) == NULL)
    {
        buf.m.msg[sizeof(buf.m.msg) - 1] = '\0';
        st->truncated++;
    }
    sanitise(buf.m.msg);

    if (log_level_rank(buf.m.level) > log_level_rank(LOG_ERROR))
        st->malformed++; /* unknown level: still logged, as "?????" */
    else if (log_level_rank(buf.m.level) < log_level_rank(opts->min_level))
    {
        st->filtered++;
        return 1;
    }

    if (emit(log, opts, mirror, &buf.m) < 0)
        return -1;
    st->received++;
    return 1;
}

/* === The periodic stats summary === */

static logd_stats_t db_snapshot(const logd_mirror_t *mirror,
                                const logd_stats_t *st)
{
    logd_stats_t s = *st;
    s.db_dropped = get_db_dropped(mirror->db);
    s.db_errors = get_db_errors(mirror->db);
    return s;
}

static int summarise(FILE *log, const logd_opts_t *opts, logd_mirror_t *mirror,
                     const logd_stats_t *st, logd_summary_window_t *w,
                     time_t now)
{
    if (opts->summary_secs <= 0)
        return 0;
    /* Signed difference, so a clock stepped backwards */
    if (now - w->opened < (time_t)opts->summary_secs)
    {
        if (now < w->opened)
            w->opened = now; /* clock went backwards: restart the window */
        return 0;
    }

    logd_stats_t cur = db_snapshot(mirror, st);
    logd_stats_t d = {
        .received = cur.received - w->at_open.received,
        .filtered = cur.filtered - w->at_open.filtered,
        .malformed = cur.malformed - w->at_open.malformed,
        .truncated = cur.truncated - w->at_open.truncated,
        .dropped = cur.dropped - w->at_open.dropped,
        .db_dropped = cur.db_dropped - w->at_open.db_dropped,
        .db_errors = cur.db_errors - w->at_open.db_errors,
    };
    int elapsed = (int)(now - w->opened);
    w->opened = now;
    w->at_open = cur;

    if (d.received == 0 && d.filtered == 0 && d.malformed == 0 &&
        d.truncated == 0 && d.dropped == 0 && d.db_dropped == 0 &&
        d.db_errors == 0)
        return 0; /* nothing happened this window: stay silent */

    log_level_t level =
        (d.malformed || d.truncated || d.dropped || d.db_dropped || d.db_errors)
            ? LOG_WARN
            : LOG_INFO;

    char line[LOG_MSG_MAX];
    snprintf(line, sizeof(line),
             "received=%lu filtered=%lu malformed=%lu truncated=%lu "
             "dropped=%lu db_dropped=%lu db_errors=%lu in last %ds",
             d.received, d.filtered, d.malformed, d.truncated, d.dropped,
             d.db_dropped, d.db_errors, elapsed);
    return emit_self(log, opts, mirror, level, line);
}

int logd_run(const logd_opts_t *opts, logd_stats_t *stats)
{
    logd_stats_t local = {0};
    logd_mirror_t mirror;
    logd_summary_window_t window;
    int rc = -1;

    if (stats != NULL)
        *stats = local;

    int socket_fd = bind_to_socket(opts->socket_path);
    if (socket_fd < 0)
        return -1;

    FILE *log = open_log_file(opts->log_path);
    if (log == NULL)
    {
        close(socket_fd);
        unlink(opts->socket_path);
        return -1;
    }

    open_db(&mirror, opts);

    char banner[LOG_MSG_MAX];
    char level[16];
    remove_level_word_padding(opts->min_level, level, sizeof(level));
    snprintf(banner, sizeof(banner),
             "tetrislogd started: socket=%s level>=%s summary=%ds db=%s",
             opts->socket_path, level, opts->summary_secs,
             mirror.db != NULL ? opts->db.dir : "off");
    emit_self(log, opts, &mirror, LOG_INFO, banner);

    window.opened = time(NULL);
    window.at_open = db_snapshot(&mirror, &local);

    while (!sig_stop)
    {
        if (sig_reopen)
        {
            sig_reopen = 0;

            FILE *fresh = open_log_file(opts->log_path);
            if (fresh != NULL)
            {
                close_log_file(log);
                log = fresh;
                emit_self(log, opts, &mirror, LOG_INFO,
                          "log file reopened (SIGHUP)");
            }
        }

        if (serve_one(socket_fd, log, opts, &mirror, &local, 0) < 0)
            goto out;

        if (summarise(log, opts, &mirror, &local, &window, time(NULL)) < 0)
            goto out;
    }

    /* If SIGTERM, drain what is queued (bounded) before closing the file. */
    rc = 0;
    for (long i = 0; i < LOGD_DRAIN_MAX; i++)
    {
        int r = serve_one(socket_fd, log, opts, &mirror, &local, MSG_DONTWAIT);
        if (r == 1)
            continue;
        if (r < 0)
            rc = -1; /* the log file broke: report a failed run */
        break;
    }

out:
    local.db_dropped = get_db_dropped(mirror.db);
    local.db_errors = get_db_errors(mirror.db);

    snprintf(banner, sizeof(banner),
             "tetrislogd stopping: received=%lu filtered=%lu malformed=%lu "
             "truncated=%lu dropped=%lu db_dropped=%lu db_errors=%lu",
             local.received, local.filtered, local.malformed, local.truncated,
             local.dropped, local.db_dropped, local.db_errors);
    emit_self(log, opts, &mirror, LOG_INFO, banner);

    db_stop(mirror.db, &local.db_dropped, &local.db_errors);
    close_log_file(log);
    close(socket_fd);
    unlink(opts->socket_path);

    if (stats != NULL)
        *stats = local;
    return rc;
}
