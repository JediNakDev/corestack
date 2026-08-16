/* Integration tests for the public `bin/tetrisdb` CLI seam.
 *
 * Most cases use this test binary as a scripted java executable. `-version`
 * succeeds, while the SocketRunner-shaped invocation binds the configured
 * socket only after checking every pre-exec prerequisite an end user depends
 * on. The tests then drive check and stop against that process. The final
 * section starts the real jar and visibly skips when java or the jar is
 * unavailable.
 *
 * Run from the repo root: make bin/test_tetrisdb && ./bin/test_tetrisdb */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libtetrisutil/logmsg.h"

#define TEST_JAR "db/dist/simpledb.jar"
#define REG_SEM "/tetrish_register"
#define WATCHDOG_SECS 20

static int tests_run;
static int tests_failed;
static int tests_skipped;
static char test_binary[PATH_MAX];
static char launcher[PATH_MAX];
static char repo_root[PATH_MAX];
static volatile sig_atomic_t runner_stop;
static volatile sig_atomic_t runner_fd = -1;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static void run(const char *name, int (*fn)(void))
{
    tests_run++;
    if (fn() != 0)
    {
        tests_failed++;
        test_output_fail(name);
    }
    else
        test_output_pass(name);
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_text(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    int rc = write_all(fd, text, strlen(text));
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

static int slurp(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static void on_term(int sig)
{
    (void)sig;
    runner_stop = 1;
    if (runner_fd >= 0)
        close((int)runner_fd);
}

static int fake_runner_preflight(const char *catalog, const char *ipc)
{
    char text[1024];
    if (slurp(catalog, text, sizeof text) != 0 ||
        strstr(text, "user (id int, name string, salt string, digest string, "
                     "iters int, created_at int)") == NULL)
        return 20;

    char db_dir[PATH_MAX];
    snprintf(db_dir, sizeof db_dir, "%s", catalog);
    char *slash = strrchr(db_dir, '/');
    if (slash == NULL)
        return 21;
    *slash = '\0';

    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof lock_path, "%s/.runner.lock", db_dir);
    int lock_fd = open(lock_path, O_RDWR);
    if (lock_fd < 0)
        return 22;
    errno = 0;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0 ||
        (errno != EWOULDBLOCK && errno != EAGAIN))
    {
        close(lock_fd);
        return 23;
    }
    close(lock_fd);

    struct stat st;
    if (stat("auth/jwt_secret", &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0777) != 0600 || st.st_size != 32)
        return 24;

    if (lstat(ipc, &st) == 0 || errno != ENOENT)
        return 27;
    return 0;
}

static int run_fake_java(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "-version") == 0)
        return 0;
    if (argc < 7 || strcmp(argv[1], "-cp") != 0 ||
        strcmp(argv[3], "simpledb.SocketRunner") != 0)
        return 19;

    if (getenv("TETRISH_FAKE_RUNNER_FAIL") != NULL)
        return 42;

    int rc = fake_runner_preflight(argv[4], argv[5]);
    if (rc != 0)
        return rc;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", argv[5]);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, 8) != 0)
        return 28;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    runner_stop = 0;
    runner_fd = fd;
    while (!runner_stop)
    {
        int client = accept(fd, NULL, NULL);
        if (client >= 0)
            close(client);
        else if (errno != EINTR)
            break;
    }
    runner_fd = -1;
    close(fd);
    unlink(argv[5]);
    return 0;
}

static int make_fixture(char *root, size_t cap)
{
    snprintf(root, cap, "/tmp/tetrish-tetrisdb-XXXXXX");
    if (mkdtemp(root) == NULL)
        return -1;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/auth", root);
    if (mkdir(path, 0700) != 0)
        return -1;
    snprintf(path, sizeof path, "%s/var", root);
    if (mkdir(path, 0700) != 0)
        return -1;
    snprintf(path, sizeof path, "%s/var/log", root);
    if (mkdir(path, 0700) != 0)
        return -1;
    snprintf(path, sizeof path, "%s/var/run", root);
    if (mkdir(path, 0700) != 0)
        return -1;

    char jar[PATH_MAX];
    snprintf(jar, sizeof jar, "%s/fake.jar", root);
    if (write_text(jar, "fake\n", 0600) != 0)
        return -1;

    char rc[4096];
    snprintf(rc, sizeof rc,
             "db = on\n"
             "db_dir = var/db\n"
             "db_ipc = var/run/tetrisdb.sock\n"
             "db_sessions = 4\n"
             "db_jar = %s\n"
             "db_java = %s\n"
             "db_timeout = 2000\n"
             "auth_max_attempts = 5\n"
             "auth_token_ttl = 604800\n"
             "auth_pbkdf2_iters = 1000\n",
             jar, test_binary);
    snprintf(path, sizeof path, "%s/.tetrishrc", root);
    return write_text(path, rc, 0600);
}

static int run_cli(const char *root, const char *verb, int fail_runner,
                   char *output, size_t output_cap)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (chdir(root) != 0)
            _exit(126);
        if (fail_runner)
            setenv("TETRISH_FAKE_RUNNER_FAIL", "1", 1);
        else
            unsetenv("TETRISH_FAKE_RUNNER_FAIL");
        execl(launcher, launcher, verb, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    alarm(WATCHDOG_SECS);
    size_t used = 0;
    while (used + 1 < output_cap)
    {
        ssize_t n = read(pipefd[0], output + used, output_cap - used - 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    close(pipefd[0]);
    output[used] = '\0';

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    alarm(0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static int run_start(const char *root, int fail_runner, char *output,
                     size_t output_cap)
{
    return run_cli(root, "start", fail_runner, output, output_cap);
}

static pid_t fixture_pid(const char *root)
{
    char path[PATH_MAX];
    char text[64];
    snprintf(path, sizeof path, "%s/var/db/.runner.lock", root);
    if (slurp(path, text, sizeof text) != 0)
        return -1;
    return (pid_t)strtol(text, NULL, 10);
}

static void stop_fixture_runner(const char *root)
{
    pid_t pid = fixture_pid(root);
    if (pid > 0)
        kill(pid, SIGTERM);

    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof lock_path, "%s/var/db/.runner.lock", root);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
    for (int i = 0; i < 1000; i++)
    {
        int fd = open(lock_path, O_RDWR);
        if (fd >= 0)
        {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            {
                close(fd);
                return;
            }
            close(fd);
        }
        nanosleep(&pause, NULL);
    }
    if (pid > 0)
        kill(pid, SIGKILL);
}

static void cleanup_fixture(const char *root)
{
    static const char *const files[] = {
        ".tetrishrc",
        "fake.jar",
        "log",
        "auth/jwt_secret",
        "var/db/catalog.txt",
        "var/db/user.dat",
        "var/db/.runner.lock",
        "var/log/tetrisdb.err",
        "var/run/tetrisdb.sock",
    };
    char path[PATH_MAX];

    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
    {
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        (void)unlink(path);
    }
    snprintf(path, sizeof path, "%s/var/db", root);
    (void)rmdir(path);
    snprintf(path, sizeof path, "%s/var/log", root);
    (void)rmdir(path);
    snprintf(path, sizeof path, "%s/var/run", root);
    (void)rmdir(path);
    snprintf(path, sizeof path, "%s/var", root);
    (void)rmdir(path);
    snprintf(path, sizeof path, "%s/auth", root);
    (void)rmdir(path);
    (void)rmdir(root);
}

static int socket_reachable(const char *path)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    int reached = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    close(fd);
    return reached;
}

static int test_start_logs_to_configured_socket(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char rc_path[PATH_MAX];
    snprintf(rc_path, sizeof rc_path, "%s/.tetrishrc", root);
    int rc_fd = open(rc_path, O_WRONLY | O_APPEND);
    CHECK(rc_fd >= 0, "open fixture rc");
    CHECK(write_all(rc_fd, "log_ipc = var/run/custom-log.sock\n",
                    strlen("log_ipc = var/run/custom-log.sock\n")) == 0,
          "configure custom log socket");
    CHECK(close(rc_fd) == 0, "close fixture rc");

    char socket_path[PATH_MAX];
    snprintf(socket_path, sizeof socket_path, "%s/var/run/custom-log.sock",
             root);
    int log_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(log_fd >= 0, "create log socket");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path);
    CHECK(bind(log_fd, (struct sockaddr *)&addr, sizeof addr) == 0,
          "bind configured log socket");

    char output[4096];
    CHECK(run_start(root, 0, output, sizeof output) == 0, output);

    log_msg_t message;
    ssize_t received = recv(log_fd, &message, sizeof message, MSG_DONTWAIT);
    CHECK(received == (ssize_t)sizeof message,
          "startup record did not reach configured log_ipc");
    stop_fixture_runner(root);
    close(log_fd);
    unlink(socket_path);
    cleanup_fixture(root);
    return 0;
}

static int test_start_succeeds_after_ordered_preflight(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char output[4096];
    int status = run_start(root, 0, output, sizeof output);
    CHECK(status == 0, output);
    CHECK(strstr(output, "started") != NULL, "success was not reported");
    CHECK(fixture_pid(root) > 0, "runner pid was not recorded");

    char catalog[PATH_MAX];
    snprintf(catalog, sizeof catalog, "%s/var/db/catalog.txt", root);
    char text[1024];
    CHECK(slurp(catalog, text, sizeof text) == 0, "catalog was not created");
    CHECK(strstr(text, "user (") != NULL, "user table was not provisioned");

    stop_fixture_runner(root);
    cleanup_fixture(root);
    return 0;
}

static int test_start_check_stop_check_and_restart(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char output[4096];
    CHECK(run_start(root, 0, output, sizeof output) == 0, output);

    CHECK(run_cli(root, "check", 0, output, sizeof output) == 0, output);

    CHECK(run_cli(root, "stop", 0, output, sizeof output) == 0, output);
    CHECK(strstr(output, "stopped") != NULL, "stop was not reported");

    char socket_path[PATH_MAX];
    snprintf(socket_path, sizeof socket_path, "%s/var/run/tetrisdb.sock", root);
    CHECK(access(socket_path, F_OK) != 0,
          "shutdown hook did not remove the socket");

    CHECK(run_cli(root, "check", 0, output, sizeof output) != 0,
          "check reported stopped auth as usable");

    CHECK(run_cli(root, "stop", 0, output, sizeof output) == 0, output);
    CHECK(strstr(output, "already stopped") != NULL,
          "a second stop was not reported as already stopped");

    CHECK(run_start(root, 0, output, sizeof output) == 0, output);
    CHECK(socket_reachable(socket_path), "runner did not restart after stop");
    CHECK(run_cli(root, "stop", 0, output, sizeof output) == 0, output);

    cleanup_fixture(root);
    return 0;
}

static int test_duplicate_start_preserves_live_runner(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char output[4096];
    CHECK(run_start(root, 0, output, sizeof output) == 0, output);
    pid_t first = fixture_pid(root);
    CHECK(first > 0, "first runner pid was not recorded");

    char socket_path[PATH_MAX];
    snprintf(socket_path, sizeof socket_path, "%s/var/run/tetrisdb.sock", root);
    CHECK(socket_reachable(socket_path), "first runner is not reachable");

    int status = run_start(root, 0, output, sizeof output);
    CHECK(status != 0, "duplicate start succeeded");
    CHECK(strstr(output, "locked") != NULL,
          "duplicate refusal did not name lock");
    CHECK(fixture_pid(root) == first,
          "duplicate start replaced the recorded pid");
    CHECK(socket_reachable(socket_path),
          "duplicate start unlinked the live runner's socket");

    stop_fixture_runner(root);
    cleanup_fixture(root);
    return 0;
}

static int test_missing_jar_refuses_before_fork(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.tetrishrc", root);
    int rc_fd = open(path, O_WRONLY | O_APPEND);
    CHECK(rc_fd >= 0, "open fixture config");
    const char *missing = "db_jar = /definitely/missing/simpledb.jar\n";
    CHECK(write_all(rc_fd, missing, strlen(missing)) == 0,
          "append missing jar config");
    close(rc_fd);

    char output[4096];
    int status = run_start(root, 0, output, sizeof output);
    CHECK(status != 0, "missing jar started a runner");
    CHECK(strstr(output, "cannot read /definitely/missing/simpledb.jar") !=
              NULL,
          "missing jar refusal was not reported");
    snprintf(path, sizeof path, "%s/var/db/user.dat", root);
    CHECK(access(path, F_OK) == 0,
          "launch prerequisite was checked before ordered provisioning");
    snprintf(path, sizeof path, "%s/auth/jwt_secret", root);
    CHECK(access(path, F_OK) == 0,
          "launch prerequisite was checked before secret provisioning");
    snprintf(path, sizeof path, "%s/var/run/tetrisdb.sock", root);
    CHECK(access(path, F_OK) != 0, "missing jar left a socket behind");
    CHECK(fixture_pid(root) <= 0, "missing jar recorded a child pid");

    cleanup_fixture(root);
    return 0;
}

static int test_runner_stderr_uses_configured_path(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.tetrishrc", root);
    int rc_fd = open(path, O_WRONLY | O_APPEND);
    CHECK(rc_fd >= 0, "open fixture config");
    const char *setting = "db_err_path = custom/runner.err\n";
    CHECK(write_all(rc_fd, setting, strlen(setting)) == 0,
          "configure runner stderr path");
    CHECK(close(rc_fd) == 0, "close fixture config");

    char output[4096];
    CHECK(run_start(root, 1, output, sizeof output) != 0,
          "failing runner unexpectedly started");
    CHECK(strstr(output, "see custom/runner.err") != NULL,
          "failure report did not name configured stderr path");

    snprintf(path, sizeof path, "%s/custom/runner.err", root);
    CHECK(access(path, F_OK) == 0, "configured stderr file was not created");
    snprintf(path, sizeof path, "%s/var/log/tetrisdb.err", root);
    CHECK(access(path, F_OK) != 0, "default stderr file was still used");

    snprintf(path, sizeof path, "%s/custom/runner.err", root);
    unlink(path);
    snprintf(path, sizeof path, "%s/custom", root);
    rmdir(path);
    cleanup_fixture(root);
    return 0;
}

static int test_loose_secret_refuses_before_socket_unlink(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/auth/jwt_secret", root);
    CHECK(write_text(path, "0123456789abcdef0123456789abcdef", 0600) == 0,
          "create secret fixture");
    CHECK(chmod(path, 0644) == 0, "loosen secret permissions");
    snprintf(path, sizeof path, "%s/var/run/tetrisdb.sock", root);
    CHECK(write_text(path, "stale\n", 0600) == 0, "create stale socket marker");

    char output[4096];
    int status = run_start(root, 0, output, sizeof output);
    CHECK(status != 0, "loose secret started a runner");
    CHECK(strstr(output, "group- or other-accessible") != NULL,
          "secret refusal did not name the permission defect");

    snprintf(path, sizeof path, "%s/auth/jwt_secret", root);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644,
          "start repaired a secret it must refuse");
    snprintf(path, sizeof path, "%s/var/db/user.dat", root);
    CHECK(access(path, F_OK) == 0,
          "secret check ran before table provisioning");
    snprintf(path, sizeof path, "%s/var/run/tetrisdb.sock", root);
    CHECK(access(path, F_OK) == 0, "secret refusal unlinked the socket marker");
    CHECK(fixture_pid(root) <= 0, "secret refusal recorded a child pid");
    cleanup_fixture(root);
    return 0;
}

/*
 * An unheld lock naming a pid means the runner died without clearing it, which
 * is what a crash or a SIGKILL leaves behind.
 *
 * TWO SEPARATE PROPERTIES, and only the first is about safety: that pid is
 * never signalled, because without the lock the text has no authority and the
 * number may since have been recycled onto an unrelated process. This test's
 * own pid is written in deliberately, so signalling it kills the suite.
 *
 * The second is that this is "already stopped" rather than a failure. It was
 * an error exit until the Phase 4 review, which broke the `tetrisdb stop &&
 * tetrisdb start` restart docs/libtetrisauth.md documents, at exactly the
 * moment an operator reaches for it - after a crash - and left the stale pid
 * in place so every later stop failed the same way.
 */
/* The whole point of the above, end to end: a runner that was SIGKILLed leaves
 * a directory that `stop && start` can still restart. */
/*
 * start creates the directories it owns.
 *
 * The socket path and the runner's stderr file are the launcher's, the same
 * way the lockfile's directory is - and a missing directory under the socket
 * surfaced as a Java bind failure that db_runner_wait() then reported as the
 * JDK-version trap, sending the reader somewhere the problem was not.
 */
static int test_start_creates_the_directories_it_owns(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char run_dir[PATH_MAX], log_dir[PATH_MAX];
    snprintf(run_dir, sizeof run_dir, "%s/var/run", root);
    snprintf(log_dir, sizeof log_dir, "%s/var/log", root);
    CHECK(rmdir(run_dir) == 0, "remove the socket directory");
    CHECK(rmdir(log_dir) == 0, "remove the runner stderr directory");

    char output[4096];
    CHECK(run_start(root, 0, output, sizeof output) == 0, output);
    CHECK(access(run_dir, X_OK) == 0,
          "start did not create the socket directory");
    CHECK(access(log_dir, X_OK) == 0,
          "start did not create the stderr directory");

    stop_fixture_runner(root);
    cleanup_fixture(root);
    return 0;
}

/**
 * Has the caller opted out of everything that needs a real runner?
 *
 * `make test-ci` sets TETRISH_NO_RUNNER because the GitHub macOS runner has no
 * usable java and cannot build the jar. Checked BEFORE java_runs(), so the
 * probe - a fork and an exec of a java that may be a stub - never happens
 * either. It also outranks TETRISH_REQUIRE_RUNNER: one says "these must run",
 * the other says "these cannot", and a deliberate opt-out is not a regression.
 */
static int runner_disabled(void)
{
    return getenv("TETRISH_NO_RUNNER") != NULL;
}

static int java_runs(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0)
    {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0)
        {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execlp("java", "java", "-version", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int real_jar_exists(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", repo_root, TEST_JAR);
    return access(path, R_OK) == 0;
}

static int test_real_runner_starts_through_cli(void)
{
    char root[PATH_MAX];
    CHECK(make_fixture(root, sizeof root) == 0, "create isolated fixture");

    char jar[PATH_MAX];
    snprintf(jar, sizeof jar, "%s/%s", repo_root, TEST_JAR);
    char rc[4096];
    snprintf(rc, sizeof rc,
             "db = on\n"
             "db_dir = var/db\n"
             "db_ipc = var/run/tetrisdb.sock\n"
             "db_sessions = 4\n"
             "db_jar = %s\n"
             "db_java = java\n"
             "db_timeout = 2000\n"
             "auth_max_attempts = 5\n"
             "auth_token_ttl = 604800\n"
             "auth_pbkdf2_iters = 1000\n",
             jar);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.tetrishrc", root);
    CHECK(write_text(path, rc, 0600) == 0, "write real-runner config");

    char output[4096];
    CHECK(run_start(root, 0, output, sizeof output) == 0, output);
    snprintf(path, sizeof path, "%s/var/run/tetrisdb.sock", root);
    CHECK(socket_reachable(path), "real runner socket is not reachable");
    pid_t first = fixture_pid(root);
    CHECK(first > 0, "real runner pid was not recorded");

    CHECK(run_start(root, 0, output, sizeof output) != 0,
          "real runner did not inherit the directory lock");
    CHECK(fixture_pid(root) == first,
          "duplicate real start replaced the recorded pid");
    CHECK(socket_reachable(path),
          "duplicate real start unlinked the live runner socket");

    CHECK(run_cli(root, "check", 0, output, sizeof output) == 0, output);
    CHECK(run_cli(root, "stop", 0, output, sizeof output) == 0, output);
    CHECK(access(path, F_OK) != 0,
          "real runner shutdown hook did not remove its socket");
    CHECK(run_cli(root, "check", 0, output, sizeof output) != 0,
          "check accepted a stopped real runner");

    CHECK(run_start(root, 0, output, sizeof output) == 0, output);
    CHECK(socket_reachable(path), "real runner did not restart after stop");
    CHECK(run_cli(root, "stop", 0, output, sizeof output) == 0, output);
    cleanup_fixture(root);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 &&
        (strcmp(argv[1], "-version") == 0 || strcmp(argv[1], "-cp") == 0))
        return run_fake_java(argc, argv);

    test_output_begin("test_tetrisdb");
    if (realpath(argv[0], test_binary) == NULL)
    {
        test_output_fail("resolve test binary");
        test_output_summary(1, 1, 0);
        return 1;
    }
    if (getcwd(repo_root, sizeof repo_root) == NULL)
    {
        test_output_fail("get repository root");
        test_output_summary(1, 1, 0);
        return 1;
    }
    snprintf(launcher, sizeof launcher, "%s/bin/tetrisdb", repo_root);

    (void)sem_unlink(REG_SEM);
    run("start provisions everything before the runner binds",
        test_start_succeeds_after_ordered_preflight);
    run("start logs to the rc-configured socket",
        test_start_logs_to_configured_socket);
    run("start, check, stop, check, and restart through the CLI",
        test_start_check_stop_check_and_restart);
    run("a duplicate start preserves the live runner",
        test_duplicate_start_preserves_live_runner);
    run("a missing jar refuses before the child is forked",
        test_missing_jar_refuses_before_fork);
    run("runner stderr uses the rc-configured path",
        test_runner_stderr_uses_configured_path);
    run("a loose secret refuses before socket unlink and spawn",
        test_loose_secret_refuses_before_socket_unlink);
    run("start creates the socket and stderr directories it owns",
        test_start_creates_the_directories_it_owns);

    if (!runner_disabled() && real_jar_exists() && java_runs())
    {
        run("runner: the real jar starts through bin/tetrisdb start",
            test_real_runner_starts_through_cli);
    }
    else
    {
        if (!runner_disabled() && getenv("TETRISH_REQUIRE_RUNNER") != NULL)
        {
            test_output_failure_detail(
                "TETRISH_REQUIRE_RUNNER is set and the tetrisdb runner test "
                "could not run",
                __FILE__, __LINE__);
            tests_run++;
            tests_failed++;
            test_output_fail("runner integration test is available");
        }
        else
        {
            test_output_skip("runner integration test",
                             runner_disabled()    ? "TETRISH_NO_RUNNER is set"
                             : !real_jar_exists() ? "no " TEST_JAR
                                                    " - run `ant dist` in db/"
                                                  : "java did not run");
            tests_skipped++;
        }
    }
    (void)sem_unlink(REG_SEM);

    test_output_summary(tests_run + tests_skipped, tests_failed, tests_skipped);
    return tests_failed == 0 ? 0 : 1;
}
