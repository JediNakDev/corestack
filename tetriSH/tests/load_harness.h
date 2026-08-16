#ifndef TETRISH_LOAD_HARNESS_H
#define TETRISH_LOAD_HARNESS_H

/**
 * @file load_harness.h
 * @brief Throwaway daemon fixture shared by the whole-server stress suites.
 *
 * Brings up a real tetrisd on a free port under a private TETRISH_ROOT, so a
 * suite can drive it with real clients over TLS and then leave nothing behind.
 * Used by test_load.c (deadlock/leak/crash) and test_saturation.c (latency);
 * it holds only what is common to both, so neither suite's notion of a
 * scenario, a metric or a pass/fail rule leaks into the other.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** Everything one throwaway daemon owns; zeroed and filled by start_daemon().
 */
typedef struct
{
    char tmp[PATH_MAX];
    char rc_path[PATH_MAX];
    char ctl_path[PATH_MAX];
    char bin_link[PATH_MAX];
    char auth_link[PATH_MAX];
    char daemon_path[PATH_MAX];
    char ca_path[PATH_MAX];
    char log_path[PATH_MAX];
    pid_t daemon;
} TestEnv;

/** Monotonic milliseconds. The only clock these suites measure with. */
static inline long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static inline void pause_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/**
 * Finds a free loopback TCP port.
 *
 * Called by start_daemon(). Binds, reads the assigned port back and closes:
 * inherently racy, but it keeps concurrent suites off each other's ports far
 * better than a hardcoded number would.
 *
 * @returns the port, or -1 if no socket could be bound.
 */
static inline int reserve_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = 0};
    socklen_t len = sizeof addr;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0)
    {
        close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

/** Waits for the daemon's ctl socket to appear. Called by start_daemon(). */
static inline int wait_for_socket(const char *path, int timeout_ms)
{
    struct stat st;
    for (int waited = 0; waited < timeout_ms; waited += 20)
    {
        if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode))
            return 0;
        pause_ms(20);
    }
    return -1;
}

/** Signals the daemon and reaps it. Safe to call twice. */
static inline void stop_daemon(TestEnv *env)
{
    if (env->daemon > 0)
    {
        kill(env->daemon, SIGTERM);
        waitpid(env->daemon, NULL, 0);
        env->daemon = -1;
    }
}

/** Removes the private root the daemon ran under. Called after stop_daemon().
 */
static inline void clean_env(TestEnv *env)
{
    unlink(env->ctl_path);
    unlink(env->rc_path);
    unlink(env->log_path);
    unlink(env->bin_link);
    unlink(env->auth_link);
    rmdir(env->tmp);
}

/**
 * Starts a tetrisd on a free port under a private TETRISH_ROOT.
 *
 * The root is a fresh temp directory holding a generated .tetrishrc and
 * symlinks back to the repo's bin/ and auth/, so the daemon finds its session
 * binary and certificates without the test writing anything into the repo.
 *
 * @param env  Receives the daemon pid and every path to clean up.
 * @returns the listening port, or -1 (nothing left running) on failure.
 */
static inline int start_daemon(TestEnv *env)
{
    char repo[PATH_MAX];
    char repo_bin[PATH_MAX], repo_auth[PATH_MAX];
    int port = reserve_port();

    memset(env, 0, sizeof *env);
    env->daemon = -1;
    snprintf(env->tmp, sizeof env->tmp, "/tmp/tetrish-load-XXXXXX");
    if (port < 0 || getcwd(repo, sizeof repo) == NULL ||
        mkdtemp(env->tmp) == NULL)
        return -1;

    snprintf(env->rc_path, sizeof env->rc_path, "%s/.tetrishrc", env->tmp);
    snprintf(env->ctl_path, sizeof env->ctl_path, "%s/tetrisd.ctl", env->tmp);
    snprintf(env->bin_link, sizeof env->bin_link, "%s/bin", env->tmp);
    snprintf(env->auth_link, sizeof env->auth_link, "%s/auth", env->tmp);
    snprintf(env->daemon_path, sizeof env->daemon_path, "%s/bin/tetrisd", repo);
    snprintf(env->ca_path, sizeof env->ca_path, "%s/auth/cacsertificate.crt",
             repo);
    snprintf(env->log_path, sizeof env->log_path, "%s/tetrisd.log", env->tmp);
    snprintf(repo_bin, sizeof repo_bin, "%s/bin", repo);
    snprintf(repo_auth, sizeof repo_auth, "%s/auth", repo);

    FILE *rc = fopen(env->rc_path, "w");
    if (rc == NULL || symlink(repo_bin, env->bin_link) != 0 ||
        symlink(repo_auth, env->auth_link) != 0)
    {
        if (rc != NULL)
            fclose(rc);
        clean_env(env);
        return -1;
    }
    /* All six directives rc_config() demands, or tetrisd refuses to start and
     * the fixture sees only a ctl socket that never appears. */
    fprintf(rc,
            "listen_port = %d\nctl_ipc = %s\nlog_ipc = %s/no-log.sock\n"
            "cert_path = auth/server_signed.crt\n"
            "key_path = auth/private_key.pem\n"
            "ca_path = auth/cacsertificate.crt\n"
            "log_path = %s\n",
            port, env->ctl_path, env->tmp, env->log_path);
    fclose(rc);

    env->daemon = fork();
    if (env->daemon == 0)
    {
        if (getenv("TETRISH_TEST_VERBOSE") == NULL)
        {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd >= 0)
            {
                dup2(nullfd, STDOUT_FILENO);
                dup2(nullfd, STDERR_FILENO);
            }
        }
        setenv("TETRISH_ROOT", env->tmp, 1);
        execl(env->daemon_path, "tetrisd", (char *)NULL);
        _exit(127);
    }
    if (env->daemon < 0 || wait_for_socket(env->ctl_path, 3000) != 0)
    {
        stop_daemon(env);
        clean_env(env);
        return -1;
    }
    return port;
}

/**
 * Redirects stdout to /dev/null, returning the saved descriptor.
 *
 * Called around the connect loop: the client library narrates every handshake,
 * and 254 of those bury the suite's own output.
 *
 * @returns the saved stdout to hand back to restore_stdout(), or -1.
 */
static inline int silence_stdout(void)
{
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    int nullfd = open("/dev/null", O_WRONLY);
    if (saved_stdout < 0 || nullfd < 0 || dup2(nullfd, STDOUT_FILENO) < 0)
    {
        if (saved_stdout >= 0)
            close(saved_stdout);
        if (nullfd >= 0)
            close(nullfd);
        return -1;
    }
    close(nullfd);
    return saved_stdout;
}

/** Puts stdout back. Accepts -1 (nothing was silenced) and does nothing. */
static inline int restore_stdout(int saved_stdout)
{
    if (saved_stdout < 0)
        return 0;
    fflush(stdout);
    int result = dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    return result < 0 ? -1 : 0;
}

#endif /* TETRISH_LOAD_HARNESS_H */
