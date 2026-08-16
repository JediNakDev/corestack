/* Public-interface tests for the .tetrishrc configuration contract.
 *
 * Every reader now asks rc_get() for the keys it owns, by name, so a fixture
 * is a directory holding a .tetrishrc with TETRISH_ROOT pointed at it - the
 * same thing the daemons see.
 *
 * auth_conf_load() freezes itself after its first successful call and never
 * re-reads the file (see src/libtetrisauth/lib/conf.c), so unlike config()
 * it cannot be re-exercised per fixture within this one process. It gets
 * exactly one case here, run last, against a fixture broad enough to touch
 * every auth_ key at once.
 *
 * Run from the repo root: make bin/test_rc && ./bin/test_rc */
#include <limits.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../src/tetrislogd/logger.h"
#include "libtetrisutil/rc.h"
#include "auth.h"

static int tests_run;
static int tests_failed;

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

/* The fixture directory currently pointed at by TETRISH_ROOT. */
static char g_fixture[64];

/**
 * Plants text as the .tetrishrc every reader will find, and returns 0.
 *
 * Called at the top of each case. text == NULL leaves the directory empty,
 * which is how "there is no rc file" is tested - a different answer from an
 * rc file with nothing in it.
 */
static int fixture(const char *text)
{
    snprintf(g_fixture, sizeof(g_fixture), "/tmp/tetrish-rc-XXXXXX");
    if (mkdtemp(g_fixture) == NULL)
        return -1;

    if (text != NULL)
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", g_fixture, RC_PATH);
        FILE *f = fopen(path, "w");
        if (f == NULL)
            return -1;
        int ok = fputs(text, f) >= 0;
        fclose(f);
        if (!ok)
            return -1;
    }
    /* The snapshot is per process and this one is about to point somewhere
     * new, which is exactly what rc_reload() is for. */
    rc_reload();
    return setenv("TETRISH_ROOT", g_fixture, 1);
}

static void fixture_free(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", g_fixture, RC_PATH);
    unlink(path);
    rmdir(g_fixture);
    unsetenv("TETRISH_ROOT");
    rc_reload();
}

/* --- rc_get(), which is the whole reader ----------------------------- */

static int test_rc_get_finds_a_key(void)
{
    CHECK(fixture("# a comment\n"
                  "PATH = /usr/bin:/bin\n"
                  "tetrislogd -e &\n"
                  "listen_port=6001\n"
                  "  log_level   =   error   \n") == 0,
          "create fixture");

    char port[16], level[16], absent[16];
    int found_port = rc_get("listen_port", NULL, port, sizeof(port));
    int found_level = rc_get("log_level", NULL, level, sizeof(level));
    int missing = rc_get("listen_prot", "fallback", absent, sizeof(absent));
    fixture_free();

    CHECK(found_port == 1, "rc_get did not find a key written without spaces");
    CHECK(strcmp(port, "6001") == 0, "rc_get kept the spacing around '='");
    CHECK(found_level == 1, "rc_get did not find a padded key");
    CHECK(strcmp(level, "error") == 0, "rc_get did not trim the value");
    /* A comment, a PATH line and a bare command are all in that file: a
     * reader asking for its own key must never see another reader's line, or
     * the shell's. */
    CHECK(missing == 0, "rc_get invented a value for an absent key");
    CHECK(strcmp(absent, "fallback") == 0,
          "rc_get did not apply the caller's fallback to an absent key");
    return 0;
}

static int test_rc_get_last_assignment_wins(void)
{
    CHECK(fixture("listen_port = 6001\n"
                  "listen_port = 6002\n") == 0,
          "create fixture");

    char value[16];
    int found = rc_get("listen_port", NULL, value, sizeof(value));
    fixture_free();

    /* An operator appending a line to the bottom of the file expects the
     * bottom line to be the one in force. */
    CHECK(found == 1, "rc_get lost a duplicated key");
    CHECK(strcmp(value, "6002") == 0,
          "rc_get took the first value, not the last");
    return 0;
}

static int test_rc_get_reports_a_value_it_cannot_fit(void)
{
    CHECK(fixture("log_ipc = a-path-far-longer-than-the-buffer\n") == 0,
          "create fixture");

    char tiny[8];
    int result = rc_get("log_ipc", "short", tiny, sizeof(tiny));
    fixture_free();

    /* Silently truncating a socket path is how two processes end up bound to
     * different sockets and neither says anything. */
    CHECK(result == -1, "rc_get truncated a value into a short buffer");
    CHECK(strcmp(tiny, "short") == 0,
          "rc_get left a half-copied value where the fallback belongs");
    return 0;
}

static int test_missing_file_is_not_an_absent_key(void)
{
    CHECK(fixture(NULL) == 0, "create fixture");

    char value[16];
    int no_file = rc_get("listen_port", NULL, value, sizeof(value));
    fixture_free();

    CHECK(fixture("log_level = debug\n") == 0, "create fixture");
    int absent = rc_get("listen_port", NULL, value, sizeof(value));
    fixture_free();

    /* The login path refuses to authenticate when nobody wrote the file, but
     * runs happily on defaults when the file simply says nothing about a key.
     * One return value cannot serve both. */
    CHECK(no_file == RC_NO_FILE, "a missing rc file read as an absent key");
    CHECK(absent == 0, "an absent key read as a missing file");
    return 0;
}

/* --- the readers built on it ------------------------------------------- */

static int test_required_paths_reject_a_missing_file(void)
{
    CHECK(fixture(NULL) == 0, "create fixture");

    logd_opts_t log;
    int log_loaded = config(&log);
    fixture_free();

    CHECK(log_loaded == -1,
          "the log loader accepted a checkout with no required paths");
    return 0;
}

/* Unlike db_ and log_, invalid auth_ values fall back to their defaults
 * rather than refusing startup (see auth_conf_load(), which never returns
 * nonzero) - rc_get_int already applies [lo, hi] and falls back on its own,
 * so auth_conf_load() has nothing left to validate. */
static int test_every_namespace_overlays(void)
{
    CHECK(fixture("db_dir = data\n"
                  "db_ipc = run/db.sock\n"
                  "db_sessions = 254\n"
                  "db_jar = simpledb.jar\n"
                  "db_java = /usr/bin/java\n"
                  "db_timeout = 60000\n"
                  "auth_max_attempts = 100\n"
                  "auth_token_ttl = 31536000\n"
                  "auth_pbkdf2_iters = 10000000\n"
                  "log_path = -\n"
                  "log_ipc = run/log.sock\n"
                  "log_level = error\n"
                  "log_send_attempts = 1000\n"
                  "log_summary_secs = 3600\n"
                  "db = yes\n"
                  "log_db_dir = logs\n"
                  "log_db_jar = simpledb.jar\n"
                  "log_db_java = /usr/bin/java\n"
                  "log_db_queue = 1\n") == 0,
          "create fixture");

    logd_opts_t log;
    int log_loaded = config(&log);

    /* The one and only call this binary makes: auth_conf_load() freezes
     * after this, so every later case must not depend on a different auth_
     * fixture. */
    int auth_loaded = auth_conf_load();
    const auth_conf_t *auth = auth_conf();
    fixture_free();

    /* Each reader took its own namespace out of one file and was untroubled
     * by the other two. */
    CHECK(log_loaded == 0, "the log loader rejected settled bounds");
    CHECK(strcmp(log.log_path, "-") == 0, "log_path overlay");
    CHECK(strcmp(log.socket_path, "run/log.sock") == 0, "log_ipc overlay");
    CHECK(log.min_level == LOG_ERROR, "log_level overlay");
    CHECK(log.summary_secs == 3600, "log_summary_secs upper bound");
    CHECK(log.db_enable == 1, "log_db overlay");
    CHECK(strcmp(log.db.dir, "logs") == 0, "log_db_dir overlay");
    CHECK(log.db.queue_cap == 1, "log_db_queue lower bound");

    CHECK(auth_loaded == 0, "auth_conf_load refused settled upper bounds");
    CHECK(auth->max_attempts == 100, "auth_max_attempts upper bound");
    CHECK(auth->token_ttl == 31536000, "auth_token_ttl upper bound");
    CHECK(auth->pbkdf2_iters == 10000000, "auth_pbkdf2_iters upper bound");
    CHECK(auth->db_timeout_ms == 60000, "db_timeout overlay");
    CHECK(strcmp(auth->db_sock, "run/db.sock") == 0, "db_ipc overlay");
    return 0;
}

/* Returns 1 if config accepted value and opts.socket_path holds it, 0 if
 * config rejected it, and -1 on fixture failure. */
static int log_socket_path_kept(size_t len)
{
    char value[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
    memset(value, 's', len);
    value[len] = '\0';

    char line[sizeof(value) + 64];
    snprintf(line, sizeof(line),
             "log_path = var/log/tetrisd.log\nlog_ipc = %s\n", value);

    if (fixture(line) != 0)
        return -1;
    logd_opts_t opts;
    int loaded = config(&opts);
    int kept = loaded == 0 && strcmp(opts.socket_path, value) == 0;
    fixture_free();
    return kept;
}

static int test_socket_path_bounds(void)
{
    size_t cap = sizeof(((struct sockaddr_un *)0)->sun_path);
    CHECK(log_socket_path_kept(cap - 1) == 1,
          "log_ipc did not keep the longest usable socket path");
    CHECK(log_socket_path_kept(cap) == 0,
          "log_ipc accepted a socket path that cannot fit");
    return 0;
}

/* Copies sample.tetrishrc into a fixture and runs the log_ reader over it.
 * The sample is what an operator starts from, so a value that reader
 * refuses is a broken checkout rather than a broken test. auth_conf_load()
 * is not re-run here - it already got its one call, above. */
static int test_sample_is_usable(void)
{
    FILE *src = fopen("sample.tetrishrc", "r");
    CHECK(src != NULL, "sample.tetrishrc is readable");

    char text[8192];
    size_t n = fread(text, 1, sizeof(text) - 1, src);
    fclose(src);
    text[n] = '\0';
    CHECK(n > 0 && n < sizeof(text) - 1, "sample.tetrishrc fits the buffer");

    CHECK(fixture(text) == 0, "create fixture");
    logd_opts_t log;
    int log_loaded = config(&log);
    char port[16];
    int has_port = rc_get("listen_port", NULL, port, sizeof(port));
    fixture_free();

    CHECK(log_loaded == 0, "sample has an invalid log_ value");
    CHECK(has_port == 1, "sample no longer sets listen_port");
    return 0;
}

int main(void)
{
    test_output_begin("test_rc");
    run("rc_get finds a key", test_rc_get_finds_a_key);
    run("rc_get: last assignment wins", test_rc_get_last_assignment_wins);
    run("rc_get reports a value it cannot fit",
        test_rc_get_reports_a_value_it_cannot_fit);
    run("a missing file is not an absent key",
        test_missing_file_is_not_an_absent_key);
    run("required paths reject a missing file",
        test_required_paths_reject_a_missing_file);
    run("socket path bounds", test_socket_path_bounds);
    run("sample is usable", test_sample_is_usable);
    /* Last: the only case allowed to call auth_conf_load(). */
    run("every namespace overlays", test_every_namespace_overlays);
    test_output_summary(tests_run, tests_failed, 0);
    return tests_failed == 0 ? 0 : 1;
}
