/* End-to-end tests for tetrislogd: the real binary, a real Unix datagram
 * socket, a real log file. Each test forks bin/tetrislogd as a child, sends
 * records through the public libtetrisutil sender (exactly as tetrisd would),
 * stops the daemon with SIGTERM, then inspects the file it wrote.
 *
 * The daemon takes its whole configuration from .tetrishrc, so a test that
 * wants a different socket, level or summary window writes one: rc_fixture()
 * plants a file in a scratch directory and points the child at it with
 * TETRISH_ROOT. The paths inside stay relative, resolved against the working
 * directory the child inherits - the repo root, same as the test's own.
 *
 * Run from the repo root: make test */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include "libhtttp/htttp.h"
#include "tetrisctl/control_plane.h"
#include "libtetrisbrain/tetrisbrain.h"
#include "tetrisd/room.h"

#define LOGD_BIN "bin/tetrislogd"
#define SOCK_PATH "var/run/test_logd.sock"
#define LOG_PATH "var/log/test_logd.log"

static int tests_run = 0, tests_failed = 0;

/* The daemon a test currently has running. Tracked so a failing test (which
 * returns early, before its stop_logd) cannot leak a daemon into the next
 * test - or outlive the suite. */
static pid_t g_logd = -1;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

/* Sleep for ms milliseconds; used only to pace polling loops. */
static void nap(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Wait up to ~2s for path to exist. Returns 0 if it appeared. */
static int wait_for_path(const char *path)
{
    struct stat st;
    for (int i = 0; i < 200; i++)
    {
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

    /* 5s, not 2: one check waits on a summary the daemon emits from its own
     * clock rather than on a record it already has in hand. Only the failure
     * path pays this - a match returns as soon as it is there. */
    for (int i = 0; i < 500; i++)
    {
        if (slurp(LOG_PATH, buf, sizeof(buf)) == 0)
        {
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

static int record_has(const char *buf, const char *level, const char *message)
{
    const char *line = buf;
    while (*line != '\0')
    {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *level_at = strstr(line, level);
        const char *message_at = strstr(line, message);
        if (level_at && message_at && level_at < line + len &&
            message_at < line + len)
            return 1;
        line = end ? end + 1 : line + len;
    }
    return 0;
}

/* The scratch directory holding the .tetrishrc the running daemon was started
 * with. One at a time: a test that restarts the daemon gets a fresh one, and
 * the old one is removed rather than left in /tmp. */
static char g_rc_dir[64];

static void rc_fixture_free(void)
{
    if (g_rc_dir[0] == '\0')
        return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.tetrishrc", g_rc_dir);
    unlink(path);
    rmdir(g_rc_dir);
    g_rc_dir[0] = '\0';
}

/*
 * Write the .tetrishrc the next daemon will read: log_ipc, log_path, and
 * whatever else the caller needs (extra is appended verbatim, or NULL).
 *
 * Deliberately a file of its own rather than the repo's .tetrishrc: the daemon
 * must not pick up the developer's log_db or log_level, and a test that asserts
 * on the level filter has to be the one that chose it. Returns 0 on success.
 */
static int rc_fixture(const char *sock, const char *extra)
{
    rc_fixture_free();
    snprintf(g_rc_dir, sizeof(g_rc_dir), "/tmp/tetrish-logd-XXXXXX");
    if (mkdtemp(g_rc_dir) == NULL)
    {
        g_rc_dir[0] = '\0';
        return -1;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.tetrishrc", g_rc_dir);
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -1;
    fprintf(f, "log_ipc = %s\nlog_path = %s\n%s", sock, LOG_PATH,
            extra != NULL ? extra : "");
    return fclose(f) == 0 ? 0 : -1;
}

/* Start the daemon with extra directives in its rc file (or NULL for none) and
 * wait until its socket is bound. Returns the child pid, or -1. */
static pid_t start_logd(const char *extra)
{
    unlink(SOCK_PATH);
    unlink(LOG_PATH);

    if (rc_fixture(SOCK_PATH, extra) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        /* The child's whole configuration, and nothing the parent's own
         * environment brought along. */
        setenv("TETRISH_ROOT", g_rc_dir, 1);
        char *const argv[] = {(char *)LOGD_BIN, NULL};
        execv(LOGD_BIN, argv);
        perror("execv " LOGD_BIN);
        _exit(127);
    }

    if (wait_for_path(SOCK_PATH) < 0)
    {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        rc_fixture_free();
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
    rc_fixture_free();
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

    ssize_t n =
        sendto(fd, data, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

#define CTL_SOCK_PATH "var/run/test_logd_ctl.sock"

/*
 * A control plane just real enough for tetrisctl to talk to.
 *
 * The point of the test below is the LOGGING boundary - that a control
 * request's outcome reaches tetrislogd, once, with the right level - not the
 * daemon behind it. So the daemon is a socket in this file rather than a
 * scenario switch inside bin/tetrisctl: the fake stays in the test, and the
 * shipped binary has exactly one code path.
 *
 * Serves one connection and exits. STATUS answers 200 with a body shaped like
 * control_plane.c's, RELOAD answers 501, so the test still gets one INFO
 * outcome and one ERROR outcome out of a real parse/serialise round trip.
 */
static int serve_one_ctl_request(int lfd)
{
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return -1;

    static uint8_t frame[CTL_MAX_FRAME];
    uint32_t len = 0;
    if (ctl_frame_read(cfd, frame, sizeof(frame), &len) != 0)
    {
        close(cfd);
        return -1;
    }

    htttp_request_t req;
    if (htttp_parse_request(frame, len, &req) != HTTTP_OK)
    {
        close(cfd);
        return -1;
    }

    htttp_response_t res;
    memset(&res, 0, sizeof(res));
    const char *body = NULL;
    if (strcmp(req.method, "RELOAD") == 0)
    {
        res.status = 501;
    }
    else
    {
        res.status = 200;
        body = "{\"uptime\":252,\"sessions\":3,\"rooms\":1}";
    }
    if (body != NULL)
    {
        res.body = (const uint8_t *)body;
        res.body_len = (uint32_t)strlen(body);
    }

    uint32_t out_len = sizeof(frame);
    int rc = -1;
    if (htttp_serialize_response(&res, frame, &out_len) == HTTTP_OK)
        rc = ctl_frame_write(cfd, frame, out_len);
    close(cfd);
    return rc;
}

/* Bind the stub's socket before forking tetrisctl, so the connect below cannot
 * race the listener into existence. Returns the listening fd, or -1. */
static int ctl_stub_listen(void)
{
    unlink(CTL_SOCK_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CTL_SOCK_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0)
    {
        close(fd);
        unlink(CTL_SOCK_PATH);
        return -1;
    }
    return fd;
}

/*
 * Run one tetrisctl command against the stub and return its exit code.
 *
 * The stub is served from this process while the child runs, rather than from
 * a second fork: one connection, one reply, and the waitpid below is then the
 * only synchronisation the test needs.
 */
static int run_tetrisctl(int lfd, const char *command)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        setenv("TETRISH_ROOT", g_rc_dir, 1);
        char *const argv[] = {(char *)"bin/tetrisctl", (char *)command, NULL};
        execv(argv[0], argv);
        _exit(127);
    }

    serve_one_ctl_request(lfd);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int test_control_boundary_outcomes(void)
{
    /* tetrisctl resolves ctl_ipc from the same rc as the daemon, so pointing it
     * at the stub is one extra directive rather than a command-line flag. */
    pid_t pid = start_logd("ctl_ipc = " CTL_SOCK_PATH "\n");
    CHECK(pid > 0, "daemon did not start");

    int lfd = ctl_stub_listen();
    CHECK(lfd >= 0, "control stub did not bind");

    CHECK(run_tetrisctl(lfd, "status") == 0, "status failed");
    CHECK(run_tetrisctl(lfd, "reload") == 1, "reload was not refused");
    close(lfd);
    unlink(CTL_SOCK_PATH);

    CHECK(wait_for_count("operation=ctl_request", 2) == 2,
          "control boundary records not written");
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(record_has(
              buf, "[INFO ]",
              "operation=ctl_request phase=complete method=STATUS status=200"),
          "successful control outcome missing");
    CHECK(record_has(
              buf, "[ERROR]",
              "operation=ctl_request phase=complete method=RELOAD status=501"),
          "reload outcome missing");
    CHECK(strstr(buf, "phase=start") == NULL,
          "routine entry record was written");
    return 0;
}

static int test_room_refusal_outcome(void)
{
    pid_t pid = start_logd(NULL);
    CHECK(pid > 0, "daemon did not start");
    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");

    int peer[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, peer) == 0,
          "room socketpair failed");
    client_add(peer[0], 4242);
    AdminMsg join = {.type = ADMIN_JOIN, .room_id = 1};
    client_handle(peer[0], &join);
    client_handle(peer[0], &join);

    CHECK(wait_for_count("operation=client_handle phase=complete status=409",
                         1) >= 1,
          "room refusal record not written");
    client_close(peer[0]);
    close(peer[0]);
    close(peer[1]);
    log_close();
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(record_has(buf, "[WARN ]",
                     "operation=client_handle phase=complete status=409"),
          "room conflict was not WARN");
    return 0;
}

static int test_gameplay_does_not_log(void)
{
    pid_t pid = start_logd("log_level = debug\n");
    CHECK(pid > 0, "daemon did not start");
    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");

    GameState game;
    memset(&game, 0, sizeof game);
    tetrisbrain_init(&game, 7);
    tetrisbrain_input(&game, MOVE_LEFT);
    tetrisbrain_input(&game, MOVE_HARD_DROP);
    tetrisbrain_input(&game, HOLD);
    tetrisbrain_tick(&game);
    log_close();
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "operation=tetrisbrain_init") == NULL,
          "game initialization escaped the brain logging boundary");
    CHECK(strstr(buf, "operation=tetrisbrain_input") == NULL,
          "game input escaped INFO filter");
    CHECK(strstr(buf, "operation=tetrisbrain_tick") == NULL,
          "game tick escaped INFO filter");
    CHECK(strstr(buf, "hard drop") == NULL,
          "hard drop wrote an internal game record");
    CHECK(strstr(buf, "hold requested") == NULL,
          "hold wrote an internal game record");
    return 0;
}

static int test_rc_accessors_are_silent(void)
{
    pid_t pid = start_logd("log_level = debug\n");
    CHECK(pid > 0, "daemon did not start");
    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");

    int number;
    int boolean;
    CHECK(rc_get_int("missing_test_number", 7, 0, 10, &number) ==
              RC_VALUE_ABSENT,
          "rc_get_int fallback failed");
    CHECK(rc_get_bool("missing_test_boolean", 1, &boolean) == RC_VALUE_ABSENT,
          "rc_get_bool fallback failed");
    CHECK(log_send(LOG_INFO, "rc accessor sentinel") == 0, "send sentinel");
    CHECK(wait_for_count("rc accessor sentinel", 1) == 1,
          "sentinel record not written");
    log_close();
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "operation=rc_get_int") == NULL,
          "minor rc_get_int wrote a routine record");
    CHECK(strstr(buf, "operation=rc_get_bool") == NULL,
          "minor rc_get_bool wrote a routine record");
    return 0;
}

/* Records of every level arrive, in order, formatted with the sender's pid. */
static int test_basic_delivery(void)
{
    pid_t pid = start_logd("log_level = debug\n");
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

/* log_level = warn keeps WARN/ERROR and discards INFO/DEBUG. */
static int test_level_filter(void)
{
    pid_t pid = start_logd("log_level = warn\n");
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
    pid_t pid = start_logd(NULL);
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
    /* Three: the two wrong-sized datagrams, plus the hostile record, whose
     * 0x41414141 drop claim is rejected as implausible instead of being added
     * to the counter an operator reads. */
    CHECK(strstr(buf, "malformed=3") != NULL, "malformed count wrong");
    CHECK(strstr(buf, "truncated=1") != NULL, "truncated count wrong");
    /* Anchored on the preceding field: a bare "dropped=0" would also match
     * inside "db_dropped=0" and pass no matter what the real counter says. */
    CHECK(strstr(buf, "truncated=1 dropped=0") != NULL,
          "forged drop claim was believed");
    return 0;
}

/* SIGHUP after the file is moved aside: the daemon must write to a fresh
 * file, which is how rotation works. */
static int test_rotation(void)
{
    pid_t pid = start_logd(NULL);
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

    pid_t pid = start_logd(NULL);
    CHECK(pid > 0, "daemon did not start");

    /* Same sender, no re-open: the transparent reconnect must pick up the
     * newly bound socket. */
    CHECK(log_send(LOG_INFO, "first-run") == 0, "send after daemon start");
    CHECK(wait_for_count("first-run", 1) >= 1,
          "record lost after daemon start");
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    /* Daemon restarted: its socket is a new inode, so the sender's connected
     * fd is stale and must be replaced under the hood. */
    pid = start_logd(NULL);
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
    pid_t pid = start_logd("log_level = debug\n");
    CHECK(pid > 0, "daemon did not start");

    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");
    /* Stop the daemon reading, so the queue fills for certain. */
    CHECK(kill(pid, SIGSTOP) == 0, "SIGSTOP failed");
    for (int i = 0; i < 20000; i++)
        log_send(LOG_DEBUG, "burst %d", i);
    CHECK(get_log_dropped() > 0,
          "queue never filled, test is not exercising drops");
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

/* Drops must reach the daemon and surface on their own, on a timer: an
 * operator learns the channel is losing records without asking anything.
 *
 * log_summary_secs = 1 makes a window a second rather than the default thirty.
 * log_level = warn is what keeps the assertions readable: the flood is sent at
 * INFO, so the ~1000 records the kernel did accept are filtered instead of
 * written, leaving a short file - and proving on the way that a record
 * discarded by the level filter still hands over the drop count it was
 * carrying.
 *
 * SIGSTOP fills the receive queue for certain. Nothing else in this test asks
 * the daemon for anything: the summary appears because its clock said so. */
static int test_drop_summary(void)
{
    pid_t pid = start_logd("log_summary_secs = 1\nlog_level = warn\n");
    CHECK(pid > 0, "daemon did not start");

    CHECK(log_open(SOCK_PATH) == 0, "log_open failed");
    /* Settle first: earlier tests in this process left drops outstanding, and
     * one delivered record hands all of them over. Everything counted after
     * this line was lost by this test. */
    CHECK(log_send(LOG_WARN, "summary-test-start") == 0,
          "settling send failed");
    CHECK(wait_for_count("summary-test-start", 1) >= 1, "settling record lost");
    unsigned long before = get_log_dropped();

    CHECK(kill(pid, SIGSTOP) == 0, "SIGSTOP failed");
    for (int i = 0; i < 20000; i++)
        log_send(LOG_INFO, "flood %d", i);
    CHECK(get_log_dropped() > before,
          "queue never filled, no drops to summarise");
    CHECK(kill(pid, SIGCONT) == 0, "SIGCONT failed");

    /* One record has to get through to carry the report, which cannot happen
     * until the daemon has worked the backlog down far enough to free queue
     * space. Retry until it does; each failure is itself a drop, and lands in
     * the same report. */
    int sent = -1;
    for (int i = 0; i < 300 && (sent = log_send(LOG_WARN, "after-flood")) != 0;
         i++)
        nap(10);
    CHECK(sent == 0, "sender never got a record through after the flood");
    unsigned long lost = get_log_dropped() - before;

    CHECK(wait_for_count(" in last ", 1) >= 1, "no summary line emitted");
    log_close();
    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    /* Anchored on the daemon's own pid immediately followed by "received=":
     * the shutdown banner also carries every one of these fields, but with
     * "tetrislogd stopping: " sitting between the pid and "received=", so
     * this cannot match that line instead of the periodic one. */
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "pid=%d: received=", (int)pid);
    const char *line = strstr(buf, prefix);
    CHECK(line != NULL, "summary line missing or malformed");
    /* The window is the configured one, give or take the tick the daemon was
     * mid-drain for. */
    CHECK(strstr(line, "in last 1s") != NULL ||
              strstr(line, "in last 2s") != NULL,
          "summary window is not the one configured");

    /* The daemon's total must agree with what the sender says it lost. Leading
     * space matters: "db_dropped=" also ends in "dropped=", and without it
     * this could match that field instead. */
    char expect[64];
    snprintf(expect, sizeof(expect), " dropped=%lu ", lost);
    CHECK(strstr(line, expect) != NULL, "daemon total disagrees with sender");
    return 0;
}

/* tetrisd dying - crashing, or being restarted between rounds - must be a
 * non-event for the logger. Two throwaway senders stand in for two runs of
 * tetrisd: the first is killed outright while holding the socket, the second
 * starts fresh afterwards and must be served by the same untouched daemon. */
static int test_survives_peer_restart(void)
{
    pid_t pid = start_logd(NULL);
    CHECK(pid > 0, "daemon did not start");

    /* Round 1: a peer that is SIGKILLed mid-life, the worst case - no
     * log_close, no exit handler, its socket torn down by the kernel. */
    pid_t peer = fork();
    CHECK(peer >= 0, "fork failed");
    if (peer == 0)
    {
        log_open(SOCK_PATH);
        log_send(LOG_INFO, "peer-run-one");
        for (;;)
            nap(50); /* hold the socket open until killed */
    }
    CHECK(wait_for_count("peer-run-one", 1) >= 1, "first peer record missing");
    CHECK(kill(peer, SIGKILL) == 0, "SIGKILL failed");
    CHECK(waitpid(peer, NULL, 0) == peer, "waitpid failed");

    /* The daemon has no peer at all now. It must still be running: a logger
     * that exits with its producer takes the crash diagnostics with it. */
    nap(100);
    int status = 0;
    CHECK(waitpid(pid, &status, WNOHANG) == 0, "daemon died with its peer");

    /* Round 2: the restarted tetrisd. A new process, a new connection, the
     * same socket file - which is the whole reason the channel is
     * connectionless datagrams and not a stream the daemon has to re-accept. */
    peer = fork();
    CHECK(peer >= 0, "fork failed");
    if (peer == 0)
    {
        int ok =
            log_open(SOCK_PATH) == 0 && log_send(LOG_INFO, "peer-run-two") == 0;
        log_close();
        _exit(ok ? 0 : 1);
    }
    CHECK(waitpid(peer, &status, 0) == peer, "waitpid failed");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "second peer failed to send");
    CHECK(wait_for_count("peer-run-two", 1) >= 1, "second peer record missing");

    CHECK(stop_logd(pid) == 0, "daemon exited non-zero");

    char buf[8192];
    CHECK(slurp(LOG_PATH, buf, sizeof(buf)) == 0, "cannot read log");
    CHECK(strstr(buf, "peer-run-one") != NULL, "pre-restart record missing");
    CHECK(strstr(buf, "peer-run-two") != NULL, "post-restart record missing");
    /* One daemon served both: no second startup banner in the file. */
    const char *first = strstr(buf, "tetrislogd started");
    CHECK(first != NULL, "startup banner missing");
    CHECK(strstr(first + 1, "tetrislogd started") == NULL,
          "daemon restarted instead of surviving");
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

    CHECK(rc_fixture(decoy, NULL) == 0, "cannot write rc fixture");
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0)
    {
        setenv("TETRISH_ROOT", g_rc_dir, 1);
        char *const argv[] = {(char *)LOGD_BIN, NULL};
        execv(LOGD_BIN, argv);
        _exit(127);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed");
    rc_fixture_free();
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
    if (fn() == 0)
    {
        test_output_pass(name);
    }
    else
    {
        tests_failed++;
        test_output_fail(name);
    }
    if (g_logd > 0)
    {
        kill(g_logd, SIGKILL);
        waitpid(g_logd, NULL, 0);
        g_logd = -1;
    }
    rc_fixture_free();
}

int main(void)
{
    test_output_begin("test_logd");
    struct stat st;
    if (stat(LOGD_BIN, &st) != 0)
    {
        test_output_failure_detailf(__FILE__, __LINE__,
                                    "%s not built (run make)", LOGD_BIN);
        test_output_fail("tetrislogd binary is built");
        test_output_summary(1, 1, 0);
        return 1;
    }

    run("basic delivery", test_basic_delivery);
    run("control boundary outcomes", test_control_boundary_outcomes);
    run("room refusal outcome", test_room_refusal_outcome);
    run("gameplay does not log", test_gameplay_does_not_log);
    run("rc accessors are silent", test_rc_accessors_are_silent);
    run("level filter", test_level_filter);
    run("malformed + log injection", test_malformed_and_injection);
    run("SIGHUP rotation", test_rotation);
    run("sender survives restart", test_sender_survives_restart);
    run("burst never blocks sender", test_burst_never_blocks);
    run("periodic drop summary", test_drop_summary);
    run("survives peer restart", test_survives_peer_restart);
    run("refuses non-socket path", test_refuses_non_socket);

    unlink(SOCK_PATH);
    unlink(LOG_PATH);

    test_output_summary(tests_run, tests_failed, 0);
    return tests_failed == 0 ? 0 : 1;
}
