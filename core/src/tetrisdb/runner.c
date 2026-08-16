#include "runner.h"
#include "libtetrisdb/jvm.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include <stddef.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** The lockfile inside a data directory. Called by db_runner_lock(). */
static int lock_path(const char *db_dir, char *path, size_t cap)
{
    int n = snprintf(path, cap, "%s/.runner.lock", db_dir);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int db_runner_lock(const char *db_dir, char *path, size_t cap)
{
    if (db_dir == NULL || db_dir[0] == '\0' || path == NULL)
    {
        fprintf(stderr, "tetrisdb: no data directory set\n");
        return -1;
    }
    if (db_mkdir_p(db_dir) != 0)
        return -1;
    if (lock_path(db_dir, path, cap) != 0)
    {
        fprintf(stderr, "tetrisdb: lock path too long for %s\n", db_dir);
        return -1;
    }

    int opened = open(path, O_RDWR | O_CREAT, 0640);
    if (opened < 0)
    {
        fprintf(stderr, "tetrisdb: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* db_runner_spawn() replaces fd 0, 1 and 2 in the child, so a low
     * descriptor here would become one of them. F_DUPFD does not set
     * FD_CLOEXEC, which is equally load-bearing: the JVM must inherit this. */
    int fd = opened;
    if (fd <= STDERR_FILENO)
    {
        fd = fcntl(opened, F_DUPFD, STDERR_FILENO + 1);
        close(opened);
        if (fd < 0)
        {
            fprintf(stderr, "tetrisdb: duplicate %s: %s\n", path,
                    strerror(errno));
            return -1;
        }
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        fprintf(stderr, "tetrisdb: %s is locked - a runner already owns %s\n",
                path, db_dir);
        close(fd);
        return -1;
    }
    return fd;
}

static int check_db_config(const db_runner_opts_t *opts, char *catalog,
                           size_t catalog_cap)
{

    if (opts == NULL || opts->dir[0] == '\0')
    {
        fprintf(stderr, "tetrisdb: no data directory set (see "
                        "db_runner_opts_t.dir)\n");
        return -1;
    }
    if (opts->ipc[0] == '\0')
    {
        fprintf(stderr, "tetrisdb: no socket path set (see "
                        "db_runner_opts_t.ipc)\n");
        return -1;
    }
    if (strlen(opts->ipc) >= sizeof(((struct sockaddr_un *)0)->sun_path))
    {
        fprintf(stderr, "tetrisdb: socket path too long: %s\n", opts->ipc);
        return -1;
    }
    if (db_jvm_check(opts->java, opts->jar) < 0)
        return -1;
    db_catalog_path(catalog, catalog_cap, opts->dir);
    if (access(catalog, R_OK) != 0)
    {
        fprintf(stderr,
                "tetrisdb: %s: %s - create the tables before starting the "
                "runner, it reads the catalog once\n",
                catalog, strerror(errno));
        return -1;
    }

    return 0;
}

pid_t db_runner_spawn(const db_runner_opts_t *opts, int err_fd)
{
    char catalog[PATH_MAX + 16];
    char sessions[32];

    if (check_db_config(opts, catalog, sizeof(catalog)) < 0)
        return -1;

    snprintf(sessions, sizeof(sessions), "--sessions=%d",
             opts->sessions > 0 ? opts->sessions : DB_DEFAULT_SESSIONS);

    char *argv[9];
    int n = 0;
    argv[n++] = (char *)opts->java;
    argv[n++] = (char *)"-cp";
    argv[n++] = (char *)opts->jar;
    argv[n++] = (char *)"simpledb.SocketRunner";
    argv[n++] = catalog;
    argv[n++] = (char *)opts->ipc;
    argv[n++] = sessions;
    if (!opts->recover)
        argv[n++] = (char *)"--no-recover";
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "tetrisdb: fork: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        /* Its own session, so the runner survives the terminal that started it
         * and never receives the launcher's job-control signals. */
        setsid();

        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0)
        {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            if (err_fd < 0)
                dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        if (err_fd >= 0)
            dup2(err_fd, STDERR_FILENO);

        execvp(opts->java, argv);
        _exit(127);
    }
    return pid;
}

static long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One connect attempt. Returns 1 if something is listening. */
static int reachable(const char *ipc)
{
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

int db_runner_wait(const char *ipc, pid_t pid, int timeout_ms)
{
    long long deadline =
        now_ms() + (timeout_ms > 0 ? timeout_ms : DB_RUNNER_DEFAULT_WAIT_MS);

    if (ipc == NULL || ipc[0] == '\0')
    {
        fprintf(stderr, "tetrisdb: no socket path to wait on\n");
        return -1;
    }

    for (;;)
    {
        if (reachable(ipc))
            return 0;

        /* Ask about the child before the clock: a JVM that refuses to start is
         * the common failure, and waiting out ten seconds to say so buries the
         * status that explains it. */
        if (pid > 0)
        {
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid)
            {
                if (WIFEXITED(status))
                    /* Name causes as possibilities, not as the diagnosis.
                     * Several unrelated failures land here with the same exit
                     * status, and a confident wrong answer sends the reader
                     * further from the real one than no answer would - the
                     * runner's own stderr has it. */
                    fprintf(
                        stderr,
                        "tetrisdb: the runner exited (status %d) instead of "
                        "listening on %s - its stderr says why. A jar built "
                        "by a newer JDK than the java on PATH looks like "
                        "this; so does a socket path whose directory does "
                        "not exist.\n",
                        WEXITSTATUS(status), ipc);
                else
                    fprintf(stderr,
                            "tetrisdb: the runner was killed by signal %d "
                            "instead of listening on %s\n",
                            WTERMSIG(status), ipc);
                return -1;
            }
        }

        if (now_ms() >= deadline)
        {
            fprintf(stderr, "tetrisdb: nothing is listening on %s\n", ipc);
            return -1;
        }
        usleep(50000);
    }
}
