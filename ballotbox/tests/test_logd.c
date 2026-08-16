/* End-to-end tests for tetrislogd: the real binary, a real Unix datagram
 * socket, a real log file. Each test forks bin/tetrislogd as a child, sends
 * records through the public libtetrisutil sender (exactly as tetrisd would),
 * stops the daemon with SIGTERM, then inspects the file it wrote.
 *
 * Run from the repo root: make test */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libtetrisutil/logmsg.h"

#define LOGD_BIN    "bin/tetrislogd"
#define TEST_ROOT   "var/logd_test"
#define TEST_RC_PATH TEST_ROOT "/.tetrishrc"
#define SOCK_PATH   "var/run/test_logd.sock"
#define LOG_PATH    "var/log/test_logd.log"

static int tests_run = 0, tests_failed = 0;

/* The daemon a test currently has running. Tracked so a failing test (which
 * returns early, before its stop_logd) cannot leak a daemon into the next
 * test - or outlive the suite. */
static pid_t g_logd = -1;

#define CHECK(cond, msg) do {                                              \
    if (!(cond)) {                                                         \
        fprintf(stderr, "    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
        return -1;                                                         \
    }                                                                      \
} while (0)

/* Sleep for ms milliseconds; used only to pace polling loops. */
static void nap(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Wait up to ~2s for path to exist. Returns 0 if it appeared. */
static int wait_for_path(const char *path)
{
    struct stat st;
    for (int i = 0; i < 200; i++) {
        if (lstat(path, &st) == 0)
            return 0;
        nap(10);
    }
    return -1;
}

/* Wait up to ~2s for the log file to contain at least want occurrences of
 * needle, so tests never race the daemon's flush. Returns the count seen. */
static int slurp(const char *path, char *buf, size_t cap);

static int wait_for_count(const char *needle, int want)
{
    char buf[8192];
    int seen = 0;

    for (int i = 0; i < 200; i++) {
        if (slurp(LOG_PATH, buf, sizeof(buf)) == 0) {
            seen = 0;
            for (const char *p = buf; (p = strstr(p, needle)) != NULL; p++)
                seen++;
            if (seen >= want)
                return seen;
        }
        nap(10);
    }
    return seen;
}

/* Read a whole file into buf (NUL-terminated). Returns 0 on success. */
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

static int write_config(const char *socket_path, const char *level)
{
    (void)mkdir(TEST_ROOT, 0700);
    FILE *f = fopen(TEST_RC_PATH, "w");
    if (f == NULL)
        return -1;
    int rc = fprintf(f,
                     "log_ipc = %s\n"
                     "log_path = %s\n"
                     "log_level = %s\n"
                     "log_summary_secs = 0\n"
                     "db = false\n",
                     socket_path, LOG_PATH, level);
    return fclose(f) == 0 && rc > 0 ? 0 : -1;
}

/* Start the daemon with the given extra argument pair (may be NULL) and wait
 * until its socket is bound. Returns the child pid, or -1. */
static pid_t start_logd(const char *extra_flag, const char *extra_value)
{
    unlink(SOCK_PATH);
    unlink(LOG_PATH);

    const char *level = "debug";
    if (extra_flag != NULL && strcmp(extra_flag, "-l") == 0 &&
        extra_value != NULL)
        level = extra_value;
    if (write_config(SOCK_PATH, level) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("TETRISH_ROOT", TEST_ROOT, 1);
        execl(LOGD_BIN, LOGD_BIN, (char *)NULL);
        perror("execv " LOGD_BIN);
        _exit(127);
    }

    if (wait_for_path(SOCK_PATH) < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    g_logd = pid;
    return pid;
}

/* SIGTERM the daemon and reap it. Returns its exit status, or -1. */
static int stop_logd(pid_t pid)
{
    int status = 0;
    if (kill(pid, SIGTERM) < 0)
        return -1;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    g_logd = -1;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

/* Send a raw buffer to the daemon, bypassing libtetrisutil, to exercise the
 * malformed-datagram path. Returns 0 if the datagram was accepted. */
static int send_raw(const void *data, size_t len)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);

    ssize_t n = sendto(fd, data, len, 0, (struct sockaddr *)&addr,
                       sizeof(addr));
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

/* Records of every level arrive, in order, formatted with the sender's pid. */
static int test_basic_delivery(void)
{
    pid_t pid = start_logd(NULL, NULL);
    CHECK(pid > 0, "daemon did not start");

    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");
    CHECK(log_send(LOG_INFO, "session %d opened", 7) == 0, "send INFO");
    CHECK(log_send(LOG_WARN, "slow tick") == 0, "send WARN");
    CHECK(log_send(LOG_ERROR, "board desync") == 0, "send ERROR");
    CHECK(log_send(LOG_DEBUG, "piece=%c", 'T') == 0, "send DEBUG");
    CHECK(wait_for_count("pid=", 5) >= 5, "records not written");
    CHECK(get_log_dropped() == 0, "sender dropped records");
    log_close();

    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "[INFO ] pid=") != NULL, "INFO line missing");
    CHECK(strstr(buf, "session 7 opened") != NULL, "INFO text missing");
    CHECK(strstr(buf, "[WARN ] pid=") != NULL, "WARN line missing");
    CHECK(strstr(buf, "[ERROR] pid=") != NULL, "ERROR line missing");
    CHECK(strstr(buf, "[DEBUG] pid=") != NULL, "DEBUG line missing");
    CHECK(strstr(buf, "piece=T") != NULL, "format args lost");

    char expect[64];
    snprintf(expect, sizeof(expect), "pid=%d: slow tick", (int)getpid());
    CHECK(strstr(buf, expect) != NULL, "sender pid not recorded");

    /* Timestamp prefix shape: "[YYYY-MM-DD HH:MM:SS] " */
    CHECK(buf[0] == '[' && buf[5] == '-' && buf[8] == '-' && buf[11] == ' ' &&
              buf[14] == ':' && buf[17] == ':' && buf[20] == ']',
          "timestamp prefix malformed");
    return 0;
}

/* -l warn keeps WARN/ERROR and discards INFO/DEBUG. */
static int test_level_filter(void)
{
    pid_t pid = start_logd("-l", "warn");
    CHECK(pid > 0, "daemon did not start");

    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");
    log_send(LOG_DEBUG, "chatter-debug");
    log_send(LOG_INFO, "chatter-info");
    log_send(LOG_WARN, "kept-warn");
    log_send(LOG_ERROR, "kept-error");
    CHECK(wait_for_count("kept-error", 1) >= 1, "ERROR record not written");
    log_close();

    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "kept-warn") != NULL, "WARN was filtered out");
    CHECK(strstr(buf, "kept-error") != NULL, "ERROR was filtered out");
    CHECK(strstr(buf, "chatter-debug") == NULL, "DEBUG leaked past filter");
    CHECK(strstr(buf, "chatter-info") == NULL, "INFO leaked past filter");
    /* filtered=2 in the shutdown banner proves they were counted, not lost. */
    CHECK(strstr(buf, "filtered=2") != NULL, "filtered count wrong");
    return 0;
}

/* Hostile input: wrong-sized datagrams, an unterminated msg, and control
 * characters that would otherwise forge log lines. */
static int test_malformed_and_injection(void)
{
    pid_t pid = start_logd(NULL, NULL);
    CHECK(pid > 0, "daemon did not start");

    /* Too short, and too long. */
    CHECK(send_raw("junk", 4) == 0, "short datagram not sent");
    char big[sizeof(log_msg_t) + 32];
    memset(big, 'x', sizeof(big));
    CHECK(send_raw(big, sizeof(big)) == 0, "oversized datagram not sent");

    /* A msg with no NUL and an embedded newline + escape sequence: the daemon
     * must terminate it and neuter the control bytes. */
    log_msg_t m;
    memset(&m, 'A', sizeof(m));
    m.pid = 4242;
    m.level = LOG_ERROR;
    m.dropped = 0;
    memcpy(m.msg, "forged\n[2000-01-01 00:00:00] [INFO ] pid=1: fake\x1b[31m",
           strlen("forged\n[2000-01-01 00:00:00] [INFO ] pid=1: fake\x1b[31m"));
    CHECK(send_raw(&m, sizeof(m)) == 0, "hostile record not sent");

    CHECK(wait_for_count("pid=4242", 1) >= 1, "hostile record not written");

    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "forged.") != NULL, "newline was not sanitised");
    CHECK(strstr(buf, "\x1b") == NULL, "escape byte survived");
    /* The forged text may appear as payload, but never as a line of its own:
     * that is exactly what sanitising the newline buys. */
    CHECK(strstr(buf, "\n[2000-01-01 00:00:00]") == NULL,
          "sender forged a whole log line");
    CHECK(strstr(buf, "malformed=2") != NULL, "malformed count wrong");
    CHECK(strstr(buf, "truncated=1") != NULL, "truncated count wrong");
    return 0;
}

/* SIGHUP after the file is moved aside: the daemon must write to a fresh
 * file, which is how rotation works. */
static int test_rotation(void)
{
    pid_t pid = start_logd(NULL, NULL);
    CHECK(pid > 0, "daemon did not start");

    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");
    log_send(LOG_INFO, "before-rotate");
    CHECK(wait_for_count("before-rotate", 1) >= 1, "pre-rotate record missing");

    CHECK(rename(LOG_PATH, LOG_PATH ".1") == 0, "rename failed");
    CHECK(kill(pid, SIGHUP) == 0, "SIGHUP failed");
    /* SIGHUP interrupts the daemon's recvfrom, so the reopen (which announces
     * itself in the new file) happens before any further record is handled. */
    CHECK(wait_for_count("reopened", 1) >= 1, "reopen record missing");
    log_send(LOG_INFO, "after-rotate");
    CHECK(wait_for_count("after-rotate", 1) >= 1, "post-rotate record missing");
    log_close();

    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read new log");
    CHECK(strstr(buf, "after-rotate") != NULL, "post-rotate record missing");
    CHECK(strstr(buf, "before-rotate") == NULL, "new file has old records");
    CHECK(slurp(LOG_PATH ".1", buf, sizeof(buf)) == 0, "cannot read old log");
    CHECK(strstr(buf, "before-rotate") != NULL, "old file lost its records");
    unlink(LOG_PATH ".1");
    return 0;
}

/* A sender started before the daemon, and one that outlives a daemon restart,
 * must both recover without the caller noticing. */
static int test_sender_survives_restart(void)
{
    /* No daemon yet: opening fails, sending is safe and counted as a drop. */
    unlink(SOCK_PATH);
    log_open(SOCK_PATH);
    CHECK(log_send(LOG_INFO, "into the void") == -1, "send should have failed");
    CHECK(get_log_dropped() >= 1, "drop not counted");

    pid_t pid = start_logd(NULL, NULL);
    CHECK(pid > 0, "daemon did not start");

    /* Same sender, no re-open: the transparent reconnect must pick up the
     * newly bound socket. */
    CHECK(log_send(LOG_INFO, "first-run") == 0, "send after daemon start");
    CHECK(wait_for_count("first-run", 1) >= 1, "record lost after daemon start");
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    /* Daemon restarted: its socket is a new inode, so the sender's connected
     * fd is stale and must be replaced under the hood. */
    pid = start_logd(NULL, NULL);
    CHECK(pid > 0, "daemon did not restart");
    CHECK(log_send(LOG_WARN, "second-run") == 0, "send after daemon restart");
    CHECK(wait_for_count("second-run", 1) >= 1, "record lost after restart");
    log_close();
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "second-run") != NULL, "post-restart record missing");
    return 0;
}

/* A burst larger than any plausible receive queue must not block the sender:
 * records may be dropped, but the call always returns. */
static int test_burst_never_blocks(void)
{
    pid_t pid = start_logd(NULL, NULL);
    CHECK(pid > 0, "daemon did not start");

    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");
    /* Stop the daemon reading, so the queue fills for certain. */
    CHECK(kill(pid, SIGSTOP) == 0, "SIGSTOP failed");
    for (int i = 0; i < 20000; i++)
        log_send(LOG_DEBUG, "burst %d", i);
    CHECK(get_log_dropped() > 0, "queue never filled, test is not exercising drops");
    CHECK(kill(pid, SIGCONT) == 0, "SIGCONT failed");
    /* Whatever the kernel accepted must still reach the file. */
    CHECK(wait_for_count("burst ", 1) >= 1, "queued records were lost");
    log_close();

    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "burst ") != NULL, "no burst record survived");
    return 0;
}

/* The daemon must not clobber a regular file mistyped as its socket path. */
static int test_refuses_non_socket(void)
{
    const char *decoy = "var/run/test_logd_decoy";
    int fd = open(decoy, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "cannot create decoy file");
    CHECK(write(fd, "precious", 8) == 8, "cannot write decoy");
    close(fd);

    CHECK(write_config(decoy, "debug") == 0, "cannot write config");

    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        setenv("TETRISH_ROOT", TEST_ROOT, 1);
        execl(LOGD_BIN, LOGD_BIN, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "daemon should have failed to start");

    char buf[64];
    CHECK(slurp(decoy, buf, sizeof(buf)) == 0, "decoy file gone");
    CHECK(strcmp(buf, "precious") == 0, "decoy file was clobbered");
    unlink(decoy);
    return 0;
}

static void run(const char *name, int (*fn)(void))
{
    tests_run++;
    printf("  %-32s", name);
    fflush(stdout);
    if (fn() == 0) {
        printf("ok\n");
    } else {
        tests_failed++;
        printf("  -> FAILED\n");
    }
    if (g_logd > 0) {
        kill(g_logd, SIGKILL);
        waitpid(g_logd, NULL, 0);
        g_logd = -1;
    }
}

int main(void)
{
    struct stat st;
    if (stat(LOGD_BIN, &st) != 0) {
        fprintf(stderr, "test_logd: %s not built (run make)\n", LOGD_BIN);
        return 1;
    }

    printf("tetrislogd end-to-end tests\n");
    run("basic delivery", test_basic_delivery);
    run("level filter", test_level_filter);
    run("malformed + log injection", test_malformed_and_injection);
    run("SIGHUP rotation", test_rotation);
    run("sender survives restart", test_sender_survives_restart);
    run("burst never blocks sender", test_burst_never_blocks);
    run("refuses non-socket path", test_refuses_non_socket);

    unlink(SOCK_PATH);
    unlink(LOG_PATH);

    printf("%d/%d passed\n", tests_run - tests_failed, tests_run);
    return tests_failed == 0 ? 0 : 1;
}
