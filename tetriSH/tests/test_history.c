/* Tests for src/tetrisd/history.c's read functions: history_db_read_player(),
 * history_db_read_recent() and history_db_read_best_scores().
 *
 * One section, unlike test_db.c/test_auth.c: every case here needs the same
 * fixed SocketRunner and the same db_ipc, so - unlike test_auth.c, whose
 * cases fork because they vary .tetrishrc or rely on file-static state that
 * never resets - one process, one rc file, one runner serves the whole
 * suite. history_db_read_*() hold no state of their own between calls.
 *
 * Isolation between cases is by DISTINCT USER NAMES rather than by wiping the
 * table between them (SimpleDB has no DELETE, and this suite is not the place
 * to restart a live runner mid-run just to clear one). Each case that cares
 * about an exact row set uses a name no other case writes.
 *
 * Skips cleanly without a JVM or db/dist/simpledb.jar, exactly like
 * tests/test_db.c and tests/test_auth.c section C.
 * TETRISH_REQUIRE_RUNNER=1 turns that skip into a failure.
 *
 * Run from the repo root: make test */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "libtetrisdb/socket/db.h"
#include "tetrisdb/runner.h"
#include "tetrisd/history.h"

#define TEST_ROOT "var/history_test"
#define TEST_RC TEST_ROOT "/.tetrishrc"
#define TEST_DB_DIR TEST_ROOT "/db"
#define TEST_SOCK TEST_ROOT "/run/tetrisdb.sock"
#define TEST_JAR "db/dist/simpledb.jar"

#define WATCHDOG_SECS 20

static int tests_run = 0, tests_failed = 0, tests_skipped = 0;

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

/* --- fixture: one SocketRunner over its own table, for the whole suite --- */

static int runner_disabled(void)
{
    return getenv("TETRISH_NO_RUNNER") != NULL;
}

static int have_jar(void)
{
    return access(TEST_JAR, R_OK) == 0;
}

static int have_java(void)
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

static int write_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (len > 0 && write(fd, data, len) != (ssize_t)len)
    {
        close(fd);
        return -1;
    }
    return close(fd);
}

static pid_t g_runner = -1;

static void stop_runner(void)
{
    if (g_runner <= 0)
        return;
    kill(g_runner, SIGTERM);
    while (waitpid(g_runner, NULL, 0) < 0 && errno == EINTR)
        ;
    g_runner = -1;
    (void)unlink(TEST_SOCK);
}

/* Removes a previous run's table, so this run's ids start at 1 again with
 * nothing already ahead of them - load-bearing, not tidiness: the
 * "server-wide recent" case assumes the rows it just inserted have the
 * highest ids in the whole table, which a leftover row from an earlier run
 * would falsify. SimpleDB's own write-ahead log ("log" in the repo root,
 * shared by every runner) is deliberately left alone, exactly as
 * tests/test_db.c's reset_dir() explains. */
static void reset_dir(void)
{
    (void)unlink(TEST_DB_DIR "/catalog.txt");
    (void)unlink(TEST_DB_DIR "/history.dat");
    (void)unlink(TEST_SOCK);
    (void)unlink(TEST_RC);
    (void)rmdir(TEST_DB_DIR);
    (void)rmdir(TEST_ROOT "/run");
    (void)rmdir(TEST_ROOT);
}

/* recover = 0 for the reason test_db.c/test_auth.c both record: SimpleDB's
 * write-ahead log is a file called "log" in the CURRENT DIRECTORY, shared by
 * every runner ever started from the repo root, while this table is created
 * fresh for this run - recovery would replay an unrelated previous run's
 * records onto it. */
static int start_runner(void)
{
    CHECK(db_ensure_table(TEST_DB_DIR, HISTORY_DB_TABLE, HISTORY_DB_SCHEMA) ==
              0,
          "fixture: create the history table");
    /* db_ensure_table() makes TEST_DB_DIR for the catalog and heap file, but
     * nothing makes the socket's own parent - the runner does not mkdir -p
     * the path it is told to bind. */
    CHECK(mkdir(TEST_ROOT "/run", 0700) == 0 || errno == EEXIST,
          "fixture: create the run directory");

    db_runner_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    snprintf(opts.java, sizeof(opts.java), "%s", DB_DEFAULT_JAVA);
    snprintf(opts.dir, sizeof opts.dir, "%s", TEST_DB_DIR);
    snprintf(opts.jar, sizeof opts.jar, "%s", TEST_JAR);
    snprintf(opts.ipc, sizeof opts.ipc, "%s", TEST_SOCK);
    opts.sessions = 8;
    opts.recover = 0;

    g_runner = db_runner_spawn(&opts, -1);
    CHECK(g_runner > 0, "fixture: spawn a runner");
    CHECK(db_runner_wait(TEST_SOCK, g_runner, 20000) == 0,
          "fixture: the runner never accepted a connection");
    return 0;
}

/* Points history.c's rc_get("db_ipc", ...) at TEST_SOCK for the rest of this
 * process's life. rc.c caches the file on first read, so this must happen
 * before the first history_db_read_*() call and needs no per-case repeat. No
 * chdir: TETRISH_ROOT only decides where the .tetrishrc file is read from
 * (ADR 0003 - libtetrisutil resolves no project root), and db_ipc's own value
 * below is written relative to the repo root, which stays this process's cwd
 * throughout. */
static int write_rc(void)
{
    char rc[256];
    int n =
        snprintf(rc, sizeof rc, "db_ipc = %s\ndb_timeout = 4000\n", TEST_SOCK);
    CHECK(n > 0 && (size_t)n < sizeof rc, "fixture: rc content");
    CHECK(write_file(TEST_RC, rc, (size_t)n) == 0, "fixture: write .tetrishrc");
    CHECK(setenv("TETRISH_ROOT", TEST_ROOT, 1) == 0,
          "fixture: setenv TETRISH_ROOT");
    return 0;
}

/* One statement on a connection of this process's own, bypassing rc - for the
 * fixtures that seed rows directly, mirroring tests/test_auth.c's query(). */
static int query(const char *sql, char *body, size_t cap)
{
    db_socket_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    snprintf(opts.sock, sizeof opts.sock, "%s", TEST_SOCK);
    opts.timeout_ms = 8000;

    db_socket_t *conn = db_socket_open(&opts);
    if (conn == NULL)
        return -1;
    db_status_t st = db_socket_exec(conn, sql, body, cap);
    db_socket_close(conn);
    return st == DB_OK ? 0 : -1;
}

static int g_next_id = 1;

static int insert_row(const char *user_name, int score, int lines,
                      long long ts_start, long long ts_end)
{
    char quoted[64];
    db_quote(quoted, sizeof quoted, user_name);
    char sql[256];
    snprintf(sql, sizeof sql,
             "insert into " HISTORY_DB_TABLE
             " values (%d, 0, %s, %d, %d, %lld, %lld);",
             g_next_id++, quoted, score, lines, ts_start, ts_end);
    return query(sql, NULL, 0);
}

/* --- cases --------------------------------------------------------------- */

static int test_player_more_than_view_holds(void)
{
    const char *name = "hist_seven_rounds";
    for (int i = 0; i < 7; i++)
        CHECK(insert_row(name, 100 + i, i, 1000 + i, 1010 + i) == 0,
              "fixture: insert a round");

    player_history_t h;
    history_db_read_player(name, &h);
    CHECK(h.status == HISTORY_VIEW_OK, "seven rounds must read back OK");
    CHECK(h.recent_count == HISTORY_VIEW_ROUNDS,
          "must cap at HISTORY_VIEW_ROUNDS, not return all seven");
    /* Newest first: round i=6 (score 106) was inserted last. */
    CHECK(h.recent[0].score == 106, "newest round must be first");
    CHECK(h.recent[4].score == 102, "fifth-newest round must be last shown");
    CHECK(h.best_score == 106, "best_score must be the maximum, not the last");
    CHECK(h.best_lines == 6, "best_lines must be the maximum");
    CHECK(h.games_played == 7,
          "games_played must count every round, not just the shown five");
    return 0;
}

static int test_player_fewer_than_view_holds(void)
{
    const char *name = "hist_two_rounds";
    CHECK(insert_row(name, 50, 3, 2000, 2010) == 0, "fixture: round 1");
    CHECK(insert_row(name, 75, 5, 2020, 2033) == 0, "fixture: round 2");

    player_history_t h;
    history_db_read_player(name, &h);
    CHECK(h.status == HISTORY_VIEW_OK, "two rounds must read back OK");
    CHECK(h.recent_count == 2, "must report exactly the rounds that exist");
    CHECK(h.recent[0].score == 75, "newest round must be first");
    CHECK(h.recent[1].score == 50, "oldest of the two must be second");
    CHECK(h.games_played == 2, "games_played must be 2");
    return 0;
}

static int test_player_with_no_rounds_is_empty(void)
{
    player_history_t h;
    history_db_read_player("hist_never_played", &h);
    CHECK(h.status == HISTORY_VIEW_EMPTY,
          "a name with no rows must report EMPTY, not OK with zeroes");
    CHECK(h.recent_count == 0, "an EMPTY result must carry no rounds");
    CHECK(h.best_score == 0 && h.games_played == 0,
          "an EMPTY result must not carry stray figures");
    return 0;
}

static int test_recent_spans_several_players(void)
{
    /* Inserted last, so these are guaranteed to be the newest three rows in
     * the whole table regardless of what earlier cases wrote. */
    CHECK(insert_row("hist_wide_x", 10, 1, 3000, 3005) == 0, "fixture: x");
    CHECK(insert_row("hist_wide_y", 20, 2, 3010, 3016) == 0, "fixture: y");
    CHECK(insert_row("hist_wide_z", 30, 3, 3020, 3028) == 0, "fixture: z");

    history_row_t rows[3];
    int n = history_db_read_recent(rows, 3);
    CHECK(n == 3, "must return exactly the 3 rows asked for");
    CHECK(strcmp(rows[0].user_name, "hist_wide_z") == 0 && rows[0].score == 30,
          "newest row must be first");
    CHECK(strcmp(rows[1].user_name, "hist_wide_y") == 0 && rows[1].score == 20,
          "second-newest row must be second");
    CHECK(strcmp(rows[2].user_name, "hist_wide_x") == 0 && rows[2].score == 10,
          "third-newest row must be third");
    return 0;
}

/* Search helper for the best-scores case, which cannot assume its two names
 * land at any particular index among whatever earlier cases also wrote. */
static const history_row_t *find_row(const history_row_t *rows, int n,
                                     const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(rows[i].user_name, name) == 0)
            return &rows[i];
    return NULL;
}

static int test_best_scores_is_max_not_latest(void)
{
    CHECK(insert_row("hist_best_alice", 500, 10, 4000, 4020) == 0,
          "fixture: alice's high round, first");
    CHECK(insert_row("hist_best_alice", 200, 4, 4030, 4035) == 0,
          "fixture: alice's low round, last");
    CHECK(insert_row("hist_best_bob", 90, 3, 4040, 4044) == 0,
          "fixture: bob's only round");

    history_row_t rows[HISTORY_BEST_SCORES_MAX];
    int n = history_db_read_best_scores(rows, HISTORY_BEST_SCORES_MAX);
    CHECK(n > 0, "best-scores query must return at least these rows");

    const history_row_t *alice = find_row(rows, n, "hist_best_alice");
    CHECK(alice != NULL, "alice must appear in the best-scores table");
    CHECK(alice->score == 500,
          "alice's best must be her high round (500), not her latest (200)");

    const history_row_t *bob = find_row(rows, n, "hist_best_bob");
    CHECK(bob != NULL, "bob must appear in the best-scores table");
    CHECK(bob->score == 90, "bob's best must be his one round");
    return 0;
}

static int test_name_with_a_quote_survives(void)
{
    const char *name = "hist_o'hare";
    CHECK(insert_row(name, 42, 2, 5000, 5009) == 0,
          "fixture: a round for a quoted name");

    player_history_t h;
    history_db_read_player(name, &h);
    CHECK(h.status == HISTORY_VIEW_OK,
          "a name containing a quote must still read back its own round");
    CHECK(h.recent_count == 1, "exactly the one round must be found");
    CHECK(h.recent[0].score == 42, "the round's score must survive intact");
    return 0;
}

int main(void)
{
    test_output_begin("history read functions");

    if (!runner_disabled() && have_jar() && have_java())
    {
        reset_dir();
        alarm(WATCHDOG_SECS * 6); /* covers JVM startup + the whole suite */
        int fixture_ok = start_runner() == 0 && write_rc() == 0;
        if (!fixture_ok)
        {
            tests_run++;
            tests_failed++;
            test_output_fail("fixture: runner and rc setup");
        }
        else
        {
            run("player: more rounds than the view holds returns the newest "
                "five",
                test_player_more_than_view_holds);
            run("player: fewer rounds than the view holds returns what exists",
                test_player_fewer_than_view_holds);
            run("player: no rounds reports EMPTY, not zeroes",
                test_player_with_no_rounds_is_empty);
            run("recent: spans several players, newest first",
                test_recent_spans_several_players);
            run("best scores: each name's maximum, not its latest",
                test_best_scores_is_max_not_latest);
            run("player: a name containing a quote survives",
                test_name_with_a_quote_survives);
        }
        alarm(0);
        stop_runner();
        reset_dir();
    }
    else
    {
        /* TETRISH_REQUIRE_RUNNER=1 turns this skip into a failure, matching
         * tests/test_db.c and tests/test_auth.c section C - a CI job that
         * sets it is not permitted to let these go quietly missing. */
        if (!runner_disabled() && getenv("TETRISH_REQUIRE_RUNNER") != NULL)
        {
            test_output_failure_detail(
                "TETRISH_REQUIRE_RUNNER is set and the runner is unavailable",
                __FILE__, __LINE__);
            tests_run++;
            tests_failed++;
            test_output_fail("history read tests are available");
        }
        else
        {
            test_output_skip("history read tests",
                             runner_disabled() ? "TETRISH_NO_RUNNER is set"
                             : !have_jar()     ? "no " TEST_JAR
                                             " - run `ant dist` in db/"
                                           : "java did not run");
            tests_skipped++;
        }
    }

    test_output_summary(tests_run + tests_skipped, tests_failed, tests_skipped);
    return tests_failed == 0 ? 0 : 1;
}
