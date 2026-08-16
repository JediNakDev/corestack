/**
 * @file pipe/proc.c
 * @brief The PipeRunner child process
 */

#include "proc.h"

#include "libtetrisdb/jvm.h"
#include "libtetrisdb/schema.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DB_DEFAULT_JAR "db/dist/simpledb.jar"
#define DB_DEFAULT_JAVA "java"
#define DB_DEFAULT_QCAP 256
#define DB_READY "<<READY>>"

/* Bounds readiness, one statement's round trip, and shutdown's reap, each
 * freshly, when db_opts_t.timeout_ms leaves it at the default. */
#define DB_PIPE_DEFAULT_TIMEOUT_MS 2000

/* Lines consumed while waiting for the handshake before giving up. Loading a
 * catalog prints a handful; anything past this means we are not talking to a
 * PipeRunner at all (a wrong jar, a JVM error) and would otherwise hang. */
#define DB_HANDSHAKE_MAX 256

int db_proc_spawn(db_proc_t *p, const db_opts_t *opts)
{
    int to_child[2], from_child[2];
    char catalog[PATH_MAX + 16];

    memset(p, 0, sizeof(*p));
    p->pid = -1;
    p->in_fd = p->out.fd = -1;
    p->timeout_ms =
        opts->timeout_ms > 0 ? opts->timeout_ms : DB_PIPE_DEFAULT_TIMEOUT_MS;

    if (opts->dir[0] == '\0')
    {
        fprintf(stderr,
                "tetrisdb: no data directory set (see db_opts_t.dir)\n");
        return -1;
    }

    if (db_jvm_check(opts->java, opts->jar) < 0)
        return -1;

    db_catalog_path(catalog, sizeof(catalog), opts->dir);

    if (pipe(to_child) < 0)
    {
        fprintf(stderr, "tetrisdb: pipe: %s\n", strerror(errno));
        return -1;
    }
    if (pipe(from_child) < 0)
    {
        fprintf(stderr, "tetrisdb: pipe: %s\n", strerror(errno));
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "tetrisdb: fork: %s\n", strerror(errno));
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return -1;
    }

    if (pid == 0)
    {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        /* PipeRunner folds its own stderr into each response body */

        char cp[PATH_MAX + 16];
        snprintf(cp, sizeof(cp), "%s", opts->jar);
        execlp(opts->java, opts->java, "-cp", cp, "simpledb.PipeRunner",
               catalog, (char *)NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    /* mark it so auto close when this process successfully calls exec()*/
    (void)fcntl(to_child[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(from_child[0], F_SETFD, FD_CLOEXEC);

    p->pid = pid;
    p->in_fd = to_child[1];
    p->out.fd = from_child[0];

    /* Wait for the handshake. Everything the catalog loader prints on the way
     * there is startup noise, not a response, so it is skipped. One deadline
     * spans every line: it bounds the sum of the waits, not each one, so a
     * stalled child cannot dribble out noise forever and stay under it. */
    long long deadline = db_now_ms() + p->timeout_ms;
    char line[512];
    for (int i = 0; i < DB_HANDSHAKE_MAX; i++)
    {
        int r = db_wire_line(&p->out, line, sizeof(line), deadline);
        if (r != DB_WIRE_LINE)
            break;
        if (strncmp(line, DB_READY, sizeof(DB_READY) - 1) == 0)
            return 0;
    }

    fprintf(stderr, "tetrisdb: %s never reported ready (jar=%s catalog=%s)\n",
            opts->java, opts->jar, catalog);
    db_proc_kill(p);
    return -1;
}

int db_proc_exec(db_proc_t *p, const char *sql, char *body, size_t body_cap)
{
    if (body != NULL && body_cap > 0)
        body[0] = '\0';

    long long deadline = db_now_ms() + p->timeout_ms;

    if (db_wire_write(p->in_fd, sql, strlen(sql), deadline) != 0 ||
        db_wire_write(p->in_fd, "\n", 1, deadline) != 0)
        return -1;

    switch (db_wire_response(&p->out, body, body_cap, deadline))
    {
    case DB_OK:
        return 1;
    case DB_IO:
    case DB_TIMEOUT: /* the child died, or stalled past its deadline */
        return -1;
    default:
        return 0;
    }
}

/* Reap pid, tolerating an interrupted wait. */
static void reap(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
}

/* Reap pid, but not forever: a child that will not take EOF as its cue to
 * exit gets killed once deadline passes, then reaped for real. */
static void reap_by(pid_t pid, long long deadline)
{
    for (;;)
    {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return;
        if (r < 0 && errno != EINTR)
            return;
        if (db_now_ms() >= deadline)
        {
            kill(pid, SIGKILL);
            reap(pid);
            return;
        }
        struct timespec pause_for = {.tv_sec = 0, .tv_nsec = 2 * 1000000};
        while (nanosleep(&pause_for, &pause_for) < 0 && errno == EINTR)
            ;
    }
}

void db_proc_close(db_proc_t *p)
{
    if (p->pid < 0)
        return;
    if (p->in_fd >= 0)
    {
        close(p->in_fd);
        p->in_fd = -1;
    }
    reap_by(p->pid, db_now_ms() + p->timeout_ms);
    if (p->out.fd >= 0)
    {
        close(p->out.fd);
        p->out.fd = -1;
    }
    p->pid = -1;
}

void db_proc_kill(db_proc_t *p)
{
    if (p->pid < 0)
        return;

    kill(p->pid, SIGKILL);
    reap(p->pid);
    if (p->in_fd >= 0)
        close(p->in_fd);
    if (p->out.fd >= 0)
        close(p->out.fd);
    p->pid = -1;
    p->in_fd = p->out.fd = -1;
}
