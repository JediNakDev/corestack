/*
 * ctl_lifecycle.c - fork bin/dspawn2, watch for the daemon, signal tetrislogd.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"

#include "tetrisctl/ctl_client.h"
#include "tetrisctl/ctl_lifecycle.h"

#define DSPAWN "bin/dspawn2"
#define LOGD_SOCK_REL "var/run/tetrislogd.sock"
#define DB_SOCK_REL "var/run/tetrisdb.sock"

/* How long a daemon gets to become observable, and how often we look. */
#define CONFIRM_TIMEOUT_MS 3000
#define CONFIRM_POLL_MS 200

const char *ctl_start_step_names[CTL_START_STEPS] = {
    "spawning via dspawn2",
    "waiting for the daemon to answer",
};

const char *ctl_stop_logd_step_names[CTL_STOP_LOGD_STEPS] = {
    "corroborating the pidfile",
    "sending SIGTERM",
    "waiting for the socket to close",
};

const char *ctl_daemon_name(Daemon d)
{
    return d == DAEMON_TETRISD ? "tetrisd" : "tetrislogd";
}

int ctl_daemon_parse(const char *name, Daemon *out)
{
    if (strcmp(name, "tetrisd") == 0)
        *out = DAEMON_TETRISD;
    else if (strcmp(name, "tetrislogd") == 0)
        *out = DAEMON_TETRISLOGD;
    else
        return -1;
    return 0;
}

/* ---- paths --------------------------------------------------------------- */

int ctl_root(char *out, size_t cap)
{
    const char *root = getenv("TETRISD_ROOT");
    if (root == NULL || root[0] == '\0')
        root = ".";
    return snprintf(out, cap, "%s", root) < (int)cap ? 0 : -1;
}

int ctl_root_path(char *out, size_t cap, const char *rel)
{
    char root[PATH_MAX];
    if (ctl_root(root, sizeof root) != 0)
        return -1;
    if (rel[0] == '/')
        return snprintf(out, cap, "%s", rel) < (int)cap ? 0 : -1;
    return snprintf(out, cap, "%s/%s", root, rel) < (int)cap ? 0 : -1;
}

/* dspawn2 names its files after the program: var/run/<name>.pid. */
static int pidfile_path(Daemon d, char *out, size_t cap)
{
    char rel[64];
    snprintf(rel, sizeof rel, "var/run/%s.pid", ctl_daemon_name(d));
    return ctl_root_path(out, cap, rel);
}

static pid_t read_pidfile(Daemon d)
{
    char path[PATH_MAX];
    if (pidfile_path(d, path, sizeof path) != 0)
        return 0;

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;

    long pid = 0;
    int got = fscanf(f, "%ld", &pid);
    fclose(f);
    return (got == 1 && pid > 0) ? (pid_t)pid : 0;
}

/* ---- liveness ------------------------------------------------------------ */

/*
 * Is something bound to and reading tetrislogd's socket?
 *
 * connect() only, never send: a zero-length datagram would reach the log sink
 * as a message. On a leftover socket file whose owner died, connect fails with
 * ECONNREFUSED, which is exactly the case this has to catch.
 */
static bool logd_socket_alive(void)
{
    char path[PATH_MAX];
    if (ctl_root_path(path, sizeof path, LOGD_SOCK_REL) != 0)
        return false;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    bool ok = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    close(fd);
    return ok;
}

bool ctl_probe(Daemon d, const char *sock, pid_t *pid_out)
{
    log_send(LOG_DEBUG, "probing daemon=%s socket=%s", ctl_daemon_name(d),
             sock != NULL ? sock : "configured");

    if (d == DAEMON_TETRISD)
    {
        if (ctl_request(sock, "STATUS", "/", CTL_TIMEOUT_TUI_MS, NULL, 0) !=
            200)
        {
            (void)log_send(LOG_INFO,
                           "operation=ctl_probe phase=complete daemon=%s "
                           "status=0",
                           ctl_daemon_name(d));
            return false;
        }
        pid_t pid = read_pidfile(d);
        if (pid_out != NULL && pid > 0)
            *pid_out = pid;
        (void)log_send(LOG_INFO,
                       "operation=ctl_probe phase=complete daemon=%s status=1",
                       ctl_daemon_name(d));
        return true;
    }

    /* Neither signal alone is enough: the pidfile outlives the process, and
     * the socket does not name who owns it. */
    pid_t pid = read_pidfile(d);
    if (pid <= 0 || !logd_socket_alive())
    {
        (void)log_send(LOG_INFO,
                       "operation=ctl_probe phase=complete daemon=%s status=0",
                       ctl_daemon_name(d));
        return false;
    }
    if (kill(pid, 0) != 0 && errno != EPERM)
    {
        (void)log_send(LOG_INFO,
                       "operation=ctl_probe phase=complete daemon=%s status=0",
                       ctl_daemon_name(d));
        return false;
    }
    if (pid_out != NULL)
        *pid_out = pid;
    (void)log_send(LOG_INFO,
                   "operation=ctl_probe phase=complete daemon=%s status=1",
                   ctl_daemon_name(d));
    return true;
}

bool ctl_probe_db(void)
{
    char configured[PATH_MAX];
    char path[PATH_MAX];
    (void)rc_get("db_ipc", DB_SOCK_REL, configured, sizeof configured);
    if (ctl_root_path(path, sizeof path, configured) != 0)
        return false;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    bool ok = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    close(fd);
    return ok;
}

int ctl_db_command(const char *command, char *err, size_t err_cap)
{
    char root[PATH_MAX];
    int pfd[2];

    if (err_cap > 0)
        err[0] = '\0';
    if (ctl_root(root, sizeof root) != 0 || pipe(pfd) != 0)
    {
        snprintf(err, err_cap, "could not prepare tetrisdb %s", command);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pfd[0]);
        close(pfd[1]);
        snprintf(err, err_cap, "could not fork tetrisdb %s: %s", command,
                 strerror(errno));
        return -1;
    }
    if (pid == 0)
    {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        if (chdir(root) != 0)
            _exit(127);
        execl("bin/tetrisdb", "bin/tetrisdb", command, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    size_t used = 0;
    while (used + 1 < err_cap)
    {
        ssize_t n = read(pfd[0], err + used, err_cap - used - 1);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    if (err_cap > 0)
        err[used] = '\0';
    char discard[256];
    while (read(pfd[0], discard, sizeof discard) > 0)
        ;
    close(pfd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        if (err_cap > 0 && err[0] == '\0')
            snprintf(err, err_cap, "tetrisdb %s failed", command);
        return -1;
    }
    return 0;
}

static void nap_ms(int ms)
{
    usleep((useconds_t)ms * 1000);
}

/* ---- start --------------------------------------------------------------- */

/*
 * Fork dspawn2 with its diagnostics on a pipe.
 *
 * The redirect happens before anything else in the child because
 * already_running() writes to stderr in dspawn2's parent, before any fork of
 * its own - inherited onto a live ncurses screen, that text corrupts it.
 * Returns 0, having filled `msg` with whatever dspawn2 had to say.
 */
static int spawn_dspawn2(Daemon d, char *msg, size_t msg_cap)
{
    char root[PATH_MAX];
    char target[64];
    int pfd[2];

    msg[0] = '\0';
    if (ctl_root(root, sizeof root) != 0)
        return -1;
    snprintf(target, sizeof target, "bin/%s", ctl_daemon_name(d));

    if (pipe(pfd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        /* dspawn2 keeps the caller's cwd on purpose and resolves
         * var/run/<name>.pid against it, so it must start at the root. */
        if (chdir(root) != 0)
            _exit(127);
        execl(DSPAWN, DSPAWN, target, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    ssize_t n = read(pfd[0], msg, msg_cap - 1);
    msg[n > 0 ? (size_t)n : 0] = '\0';
    close(pfd[0]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    /* Not checked for success: dspawn2 exits after its first fork, before
     * execvp, so this says nothing about the daemon. Only 127 is meaningful,
     * and only because it is ours. */
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127)
        return -1;
    return 0;
}

int ctl_start(Daemon d, const char *sock, CtlStepFn step, void *ctx, char *err,
              size_t err_cap)
{
    log_send(LOG_DEBUG, "starting daemon=%s socket=%s", ctl_daemon_name(d),
             sock != NULL ? sock : "configured");
    char msg[512];

    if (err_cap > 0)
        err[0] = '\0';

    /*
     * Checked before spawning, not after.
     *
     * The confirm loop below would pass on its first poll against a daemon
     * that was already running, and report a spawn that dspawn2 in fact
     * refused. Asking first is the only way the answer is true.
     */
    if (ctl_probe(d, sock, NULL))
    {
        snprintf(err, err_cap, "%s is already running", ctl_daemon_name(d));
        (void)log_send(LOG_INFO,
                       "operation=ctl_start phase=complete daemon=%s status=%d",
                       ctl_daemon_name(d), CTL_START_ALREADY);
        return CTL_START_ALREADY;
    }

    if (spawn_dspawn2(d, msg, sizeof msg) != 0)
    {
        if (step)
            step(0, 0, ctx);
        snprintf(err, err_cap, "could not exec %s from the project root",
                 DSPAWN);
        (void)log_send(LOG_INFO,
                       "operation=ctl_start phase=complete daemon=%s status=%d",
                       ctl_daemon_name(d), CTL_START_FAILED);
        return CTL_START_FAILED;
    }
    if (step)
        step(0, 1, ctx);
    log_send(LOG_DEBUG, "daemon=%s spawned; waiting for readiness",
             ctl_daemon_name(d));

    for (int waited = 0; waited < CONFIRM_TIMEOUT_MS; waited += CONFIRM_POLL_MS)
    {
        nap_ms(CONFIRM_POLL_MS);
        if (ctl_probe(d, sock, NULL))
        {
            if (step)
                step(1, 1, ctx);
            (void)log_send(
                LOG_INFO,
                "operation=ctl_start phase=complete daemon=%s status=%d",
                ctl_daemon_name(d), CTL_START_OK);
            return CTL_START_OK;
        }
    }

    if (step)
        step(1, 0, ctx);

    /* dspawn2's own words first - it may have refused for a reason the
     * pre-check could not see, such as losing a race for the pidfile.
     * Otherwise point at where the daemon's complaint has been going. */
    if (msg[0] != '\0')
        snprintf(err, err_cap, "%s", msg);
    else
        snprintf(err, err_cap,
                 "%s did not become reachable within %ds. See "
                 "var/log/%s.err",
                 ctl_daemon_name(d), CONFIRM_TIMEOUT_MS / 1000,
                 ctl_daemon_name(d));
    (void)log_send(LOG_INFO,
                   "operation=ctl_start phase=complete daemon=%s status=%d",
                   ctl_daemon_name(d), CTL_START_FAILED);
    return CTL_START_FAILED;
}

/* ---- stop ---------------------------------------------------------------- */

int ctl_stop_logd(CtlStepFn step, void *ctx, char *err, size_t err_cap)
{
    log_send(LOG_DEBUG, "stopping daemon=tetrislogd signal=SIGTERM");
    if (err_cap > 0)
        err[0] = '\0';

    pid_t pid = 0;
    if (!ctl_probe(DAEMON_TETRISLOGD, NULL, &pid))
    {
        if (step)
            step(0, 0, ctx);
        pid_t claimed = read_pidfile(DAEMON_TETRISLOGD);
        if (claimed > 0)
            snprintf(err, err_cap,
                     "tetrislogd.pid names pid %ld but no logd socket is "
                     "live. Refusing to signal it; remove "
                     "var/run/tetrislogd.pid if it is stale.",
                     (long)claimed);
        else
            snprintf(err, err_cap, "tetrislogd does not appear to be running");
        (void)log_send(LOG_INFO,
                       "operation=ctl_stop_logd phase=complete status=-1");
        return -1;
    }
    if (step)
        step(0, 1, ctx);

    if (kill(pid, SIGTERM) != 0)
    {
        if (step)
            step(1, 0, ctx);
        snprintf(err, err_cap, "SIGTERM to pid %ld: %s", (long)pid,
                 strerror(errno));
        (void)log_send(LOG_INFO,
                       "operation=ctl_stop_logd phase=complete status=-1");
        return -1;
    }
    if (step)
        step(1, 1, ctx);
    (void)log_send(LOG_INFO,
                   "operation=ctl_stop_logd event=stop_requested pid=%ld "
                   "status=0",
                   (long)pid);
    log_send(LOG_DEBUG, "daemon=tetrislogd pid=%ld signalled; waiting for exit",
             (long)pid);

    /* tetrislogd unlinks its socket on the way out, so the file disappearing
     * is positive proof it got there - better than the pid going away, which
     * also happens if something else killed it. */
    char sock_path[PATH_MAX];
    ctl_root_path(sock_path, sizeof sock_path, LOGD_SOCK_REL);

    for (int waited = 0; waited < CONFIRM_TIMEOUT_MS; waited += CONFIRM_POLL_MS)
    {
        nap_ms(CONFIRM_POLL_MS);
        struct stat sb;
        if (stat(sock_path, &sb) != 0)
        {
            if (step)
                step(2, 1, ctx);
            char pid_path[PATH_MAX];
            if (pidfile_path(DAEMON_TETRISLOGD, pid_path, sizeof pid_path) == 0)
                unlink(pid_path); /* nobody else cleans it */
            (void)log_send(LOG_INFO,
                           "operation=ctl_stop_logd phase=complete status=0");
            return 0;
        }
    }

    if (step)
        step(2, 0, ctx);
    snprintf(err, err_cap, "signalled pid %ld but %s is still there after %ds",
             (long)pid, LOGD_SOCK_REL, CONFIRM_TIMEOUT_MS / 1000);
    (void)log_send(LOG_INFO,
                   "operation=ctl_stop_logd phase=complete status=-1");
    return -1;
}
