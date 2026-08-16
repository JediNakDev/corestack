#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libtetrisauth/auth.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include "runner.h"

#define STOP_WAIT_MS 10000

static void report(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void report(log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_list log_ap;

    va_start(ap, fmt);
    va_copy(log_ap, ap);
    fprintf(stderr, "tetrisdb: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    (void)log_vsend(level, fmt, log_ap);
    va_end(log_ap);
    va_end(ap);
}

/* The lock has to exist before db_ensure_table() creates the table, so its
 * directory is the one small piece of step 1 that may need creating. */
static int mkdir_parent(const char *path)
{
    char buf[PATH_MAX];

    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf)
    {
        report(LOG_ERROR, "path is too long: %s", path);
        return -1;
    }

    char *slash = strrchr(buf, '/');
    if (slash == NULL || slash == buf)
        return 0; /* the working directory, or "/" - both already exist */
    *slash = '\0';
    return db_mkdir_p(buf);
}

/**
 * Takes the runner lock and clears any stale pid from it.
 *
 * The fd hygiene the JVM inheritance depends on is db_runner_lock()'s, so it
 * is enforced by the module that imposes it rather than restated here.
 */
static int acquire_lock(const char *db_dir, char *path, size_t pathlen)
{
    int fd = db_runner_lock(db_dir, path, pathlen);
    if (fd < 0)
        return -1;

    /* Once the lock is ours, any old pid is necessarily stale. Clear it before
     * preflight so the file names an owner exactly when one exists. A duplicate
     * start never reaches this line and therefore cannot erase the live pid. */
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0 || fsync(fd) != 0)
    {
        report(LOG_ERROR, "clear %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int record_pid(int lock_fd, const char *lock_path, pid_t pid)
{
    char text[64];
    int len = snprintf(text, sizeof text, "%ld\n", (long)pid);

    if (ftruncate(lock_fd, 0) != 0 || lseek(lock_fd, 0, SEEK_SET) < 0)
    {
        report(LOG_ERROR, "write %s: %s", lock_path, strerror(errno));
        return -1;
    }
    size_t written = 0;
    while (written < (size_t)len)
    {
        ssize_t n = write(lock_fd, text + written, (size_t)len - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            report(LOG_ERROR, "write %s: %s", lock_path, strerror(errno));
            return -1;
        }
        if (n == 0)
        {
            report(LOG_ERROR, "write %s: wrote zero bytes", lock_path);
            return -1;
        }
        written += (size_t)n;
    }
    if (fsync(lock_fd) != 0)
    {
        report(LOG_ERROR, "write %s: %s", lock_path, strerror(errno));
        return -1;
    }
    return 0;
}

static int clear_pid(int lock_fd, const char *lock_path)
{
    if (ftruncate(lock_fd, 0) != 0 || lseek(lock_fd, 0, SEEK_SET) < 0 ||
        fsync(lock_fd) != 0)
    {
        report(LOG_ERROR, "clear %s: %s", lock_path, strerror(errno));
        return -1;
    }
    return 0;
}

static void stop_failed_child(pid_t pid)
{
    if (pid <= 0)
        return;
    (void)kill(pid, SIGTERM);

    struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
    for (int i = 0; i < 100; i++)
    {
        pid_t done = waitpid(pid, NULL, WNOHANG);
        if (done == pid || (done < 0 && errno == ECHILD))
            return;
        nanosleep(&pause, NULL);
    }

    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

/*
 * Loads and validates the rc file. Called by start() and check().
 *
 * A missing file is a refusal rather than a silent run on defaults, so it is
 * probed for here: rc_get() answers RC_NO_FILE for every key, which each
 * reader below is right to treat as "use the default" and this verb is not.
 * Both verbs stop here, before the lock, the semaphore or the socket.
 */
static int load_config(db_runner_opts_t *opts)
{
    memset(opts, 0, sizeof *opts);
    rc_get("db_dir", DB_DEFAULT_DIR, opts->dir, sizeof opts->dir);
    rc_get("db_ipc", DB_DEFAULT_IPC, opts->ipc, sizeof opts->ipc);
    rc_get("db_jar", DB_DEFAULT_JAR, opts->jar, sizeof opts->jar);
    rc_get("db_java", DB_DEFAULT_JAVA, opts->java, sizeof opts->java);
    rc_get("db_err_path", DB_DEFAULT_ERR_PATH, opts->err_path,
           sizeof opts->err_path);
    rc_get_int("db_sessions", DB_DEFAULT_SESSIONS, 1, MAX_SESSIONS,
               &opts->sessions);
    opts->recover = 1;
    return 0;
}

static int provision(const db_runner_opts_t *opts)
{
    if (db_ensure_table(opts->dir, TETRISAUTH_DB_TABLE, TETRISAUTH_DB_SCHEMA) !=
        0)
    {
        report(LOG_ERROR, "could not provision database tables in %s",
               opts->dir);
        return -1;
    }

    return 0;
}

static int start(void)
{
    db_runner_opts_t opts;
    if (load_config(&opts) < 0)
        return 1;

    (void)log_open_configured();

    char lock_path[PATH_MAX];
    int lock_fd = acquire_lock(opts.dir, lock_path, sizeof lock_path);
    if (lock_fd < 0)
        goto fail;

    if (provision(&opts) != 0)
        goto fail_locked;

    if (auth_secret_provision(".") != 0)
    {
        report(LOG_ERROR, "could not provision the auth secret");
        goto fail_locked;
    }

    if (mkdir_parent(opts.ipc) != 0)
        goto fail_locked;

    if (mkdir_parent(opts.err_path) != 0)
        goto fail_locked;

    int err_fd = open(opts.err_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (err_fd < 0)
    {
        report(LOG_ERROR, "open %s: %s", opts.err_path, strerror(errno));
        goto fail_locked;
    }

    pid_t pid = db_runner_spawn(&opts, err_fd);
    close(err_fd);
    if (pid < 0)
    {
        report(LOG_ERROR, "runner did not start; see %s", opts.err_path);
        goto fail_locked;
    }

    if (record_pid(lock_fd, lock_path, pid) != 0)
    {
        stop_failed_child(pid);
        (void)clear_pid(lock_fd, lock_path);
        goto fail_locked;
    }

    if (db_runner_wait(opts.ipc, pid, 0) != 0)
    {
        stop_failed_child(pid);
        report(LOG_ERROR, "runner failed readiness; see %s", opts.err_path);
        goto fail_locked;
    }

    report(LOG_INFO, "started runner pid=%ld socket=%s sessions=%d", (long)pid,
           opts.ipc, opts.sessions);
    close(lock_fd); /* the JVM's inherited descriptor keeps the lock held */
    log_close();
    return 0;

fail_locked:
    close(lock_fd);
fail:
    log_close();
    return 1;
}

static int check(void)
{
    db_runner_opts_t opts;
    if (load_config(&opts) < 0)
        return 1;
    return db_runner_wait(opts.ipc, -1, 1) == 0 ? 0 : 1;
}

static int read_lock_pid(int fd, const char *lock_path, pid_t *pid)
{
    char buf[64];
    ssize_t n = pread(fd, buf, sizeof buf, 0);
    if (n < 0)
    {
        report(LOG_ERROR, "read %s: %s", lock_path, strerror(errno));
        return -1;
    }
    if (n == 0)
        return 1;

    if ((size_t)n == sizeof buf)
        return -1;
    buf[n] = '\0';
    char *end;
    errno = 0;
    long value = strtol(buf, &end, 10);
    if (errno != 0 || value <= 1 || value > INT_MAX ||
        (*end != '\0' && (*end != '\n' || end[1] != '\0')))
    {
        report(LOG_ERROR, "%s is malformed - expected one positive runner pid",
               lock_path);
        return -1;
    }

    *pid = (pid_t)value;
    return 0;
}

static int stop(void)
{
    db_runner_opts_t opts;
    if (load_config(&opts) < 0)
        return 1;

    char lock_path[PATH_MAX];
    if (snprintf(lock_path, sizeof lock_path, "%s/.runner.lock", opts.dir) >=
        (int)sizeof lock_path)
        return 1;

    int fd = open(lock_path, O_RDWR);
    if (fd < 0)
    {
        if (errno == ENOENT)
        {
            report(LOG_INFO, "already stopped");
            return 0;
        }
        report(LOG_ERROR, "open %s: %s", lock_path, strerror(errno));
        return 1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
    {
        int cleared = clear_pid(fd, lock_path);
        close(fd);
        report(LOG_INFO, "already stopped");
        return cleared == 0 ? 0 : 1;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN)
    {
        report(LOG_ERROR, "inspect lock %s: %s", lock_path, strerror(errno));
        close(fd);
        return 1;
    }

    pid_t pid;
    if (read_lock_pid(fd, lock_path, &pid) != 0)
    {
        close(fd);
        return 1;
    }
    if (kill(pid, SIGTERM) != 0)
    {
        report(LOG_ERROR, "signal runner pid %ld: %s", (long)pid,
               strerror(errno));
        close(fd);
        return 1;
    }

    struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
    for (int waited_ms = 0; waited_ms < STOP_WAIT_MS; waited_ms += 50)
    {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            int cleared = clear_pid(fd, lock_path);
            close(fd);
            if (cleared != 0)
                return 1;
            report(LOG_INFO, "stopped runner pid=%ld socket=%s", (long)pid,
                   opts.ipc);
            return 0;
        }
        nanosleep(&pause, NULL);
    }

    close(fd);
    report(LOG_ERROR, "timeout waiting for runner pid %ld", (long)pid);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: tetrisdb {start|check|stop}\n");
        return 2;
    }

    int db_enable;
    (void)rc_get_bool("db", 0, &db_enable);

    if (db_enable == 0)
    {
        fprintf(stderr, "tetrisdb: db flag is disabled\n");
        return 2;
    }

    if (strcmp(argv[1], "start") == 0)
        return start();
    if (strcmp(argv[1], "check") == 0)
        return check();
    if (strcmp(argv[1], "stop") == 0)
        return stop();

    fprintf(stderr, "tetrisdb: unknown command '%s'\n", argv[1]);
    return 2;
}
