/* Tests for libtetrisdb: table creation, SQL quoting, the read path's row
 * helpers and deadline, and end-to-end round trips through both runners.
 *
 * Four sections, in ascending order of what they need to run:
 *
 *   A. Files only        table creation and quoting
 *   B. Literal strings   the select-reply parser (#44 section 6)
 *   C. Scripted peer     the deadline, the markers, and a dead runner. A
 *                        listening unix socket and a forked child are enough
 *                        to hold a connection silent, answer <<END retry>>, or
 *                        hang up mid-exchange - none of which a real runner
 *                        can be asked to do on demand.
 *   D. Live runner       PipeRunner and SocketRunner for real
 *
 * D needs a JVM and db/dist/simpledb.jar. When either is missing those tests
 * are skipped rather than failed - a machine without a JVM can still build and
 * test the rest of tetriSH, which is the whole reason database mirroring is
 * opt-in. A, B and C need neither, which is deliberate: the deadline and the
 * marker handling are the parts a silent bug hides in, so they are pinned by
 * cases that always run.
 *
 * NO CASE ASSERTS ON ELAPSED WALL-CLOCK TIME. The deadline cases assert the
 * status returned, under an alarm() watchdog that kills a hang outright, which
 * separates "answered correctly" from "never answered" with no tolerance to
 * tune and nothing to go flaky on a loaded machine.
 *
 * Run from the repo root: make test */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libtetrisdb/pipe/db.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "libtetrisdb/socket/db.h"
#include "tetrisdb/runner.h"

#define TEST_DIR "var/db_test"
#define TEST_JAR "db/dist/simpledb.jar"
#define TEST_SCHEMA "id int, pid int, ts int, status string, msg string"
#define TEST_SOCK TEST_DIR "/test.sock"

/* Short enough that a broken deadline shows up as a hung test rather than a
 * slow one, long enough not to fire on a loaded machine before the code under
 * test has done its work. */
#define TEST_TIMEOUT_MS 300

/* Kills the process if a case that must not block does. Chosen over a
 * measured elapsed time on purpose: this asserts "it returned", not "it
 * returned within N ms", and there is no threshold to tune. */
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

/*
 * Remove the scratch directory so each run starts from nothing.
 *
 * SimpleDB's write-ahead log - a file called "log" in the repo root, shared by
 * every runner started from there - is deliberately NOT removed. A tetrislogd
 * started by hand could be using it right now, and deleting a live daemon's
 * WAL to tidy a test is a worse trade than the residue. What the residue costs
 * is explained at test_runner_opts(), which sidesteps it with recover = 0.
 */
static void reset_dir(void)
{
    (void)unlink(TEST_DIR "/catalog.txt");
    (void)unlink(TEST_DIR "/log.dat");
    (void)unlink(TEST_SOCK);
    (void)rmdir(TEST_DIR);
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
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

/* === Table creation === */

static int test_creates_table(void)
{
    reset_dir();
    CHECK(db_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "create failed");

    char catalog[1024];
    CHECK(slurp(TEST_DIR "/catalog.txt", catalog, sizeof(catalog)) == 0,
          "no catalog written");
    CHECK(strstr(catalog, "log (" TEST_SCHEMA ")") != NULL,
          "catalog line missing or malformed");

    /* One empty page, not zero bytes: a zero-length heap file cannot be
     * scanned, so a fresh table would fail every SELECT until first write. */
    CHECK(file_size(TEST_DIR "/log.dat") == 4096, "log.dat is not one page");
    return 0;
}

static int test_create_is_idempotent(void)
{
    reset_dir();
    CHECK(db_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "first failed");

    /* Put a byte in the data file so truncation would be visible. */
    int fd = open(TEST_DIR "/log.dat", O_WRONLY);
    CHECK(fd >= 0, "cannot open log.dat");
    CHECK(write(fd, "x", 1) == 1, "cannot write log.dat");
    close(fd);

    CHECK(db_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "second failed");

    char catalog[1024];
    CHECK(slurp(TEST_DIR "/catalog.txt", catalog, sizeof(catalog)) == 0,
          "no catalog");
    int lines = 0;
    for (const char *p = catalog; (p = strstr(p, "log (")) != NULL; p++)
        lines++;
    CHECK(lines == 1, "catalog gained a duplicate line");
    CHECK(file_size(TEST_DIR "/log.dat") == 4096,
          "existing data was truncated");

    char first = 0;
    fd = open(TEST_DIR "/log.dat", O_RDONLY);
    CHECK(fd >= 0 && read(fd, &first, 1) == 1, "cannot reread log.dat");
    close(fd);
    CHECK(first == 'x', "existing data was overwritten");
    return 0;
}

/* A daemon that forgets to name its data directory must be told, not given a
 * shared default that would collide with another daemon's tables. */
static int test_dir_is_required(void)
{
    db_opts_t opts = {0};

    CHECK(opts.dir[0] == '\0', "zero-initialized options invented a directory");
    CHECK(db_ensure_table("", "log", TEST_SCHEMA) < 0,
          "empty directory was accepted");
    CHECK(db_start(&opts, NULL, NULL, 0) == NULL,
          "started with no directory set");
    return 0;
}

/* === Quoting === */

static int test_quote_plain(void)
{
    char out[64];
    db_quote(out, sizeof(out), "hello");
    CHECK(strcmp(out, "'hello'") == 0, "plain text not wrapped");
    return 0;
}

static int test_quote_doubles_quotes(void)
{
    char out[64];
    db_quote(out, sizeof(out), "it's");
    CHECK(strcmp(out, "'it''s'") == 0, "quote not doubled");
    return 0;
}

static int test_quote_injection_stays_one_literal(void)
{
    char out[128];
    db_quote(out, sizeof(out), "x'); insert into log values (9);--");

    /* Every quote in the payload must be doubled, so the parser sees one
     * literal and never a second statement. Counting is enough: an odd number
     * of consecutive quotes anywhere would end the literal early. */
    int runs_of_odd_quotes = 0;
    for (const char *p = out + 1; *p != '\0' && p[1] != '\0';)
    {
        if (*p != '\'')
        {
            p++;
            continue;
        }
        int n = 0;
        while (p[n] == '\'')
            n++;
        if (n % 2 != 0)
            runs_of_odd_quotes++;
        p += n;
    }
    CHECK(out[0] == '\'', "missing opening quote");
    CHECK(out[strlen(out) - 1] == '\'', "missing closing quote");
    CHECK(runs_of_odd_quotes == 0, "an unescaped quote survived");
    return 0;
}

static int test_quote_truncates_safely(void)
{
    char out[8];
    db_quote(out, sizeof(out), "''''''''''''''''");
    CHECK(strlen(out) < sizeof(out), "overflowed the buffer");
    CHECK(out[0] == '\'' && out[strlen(out) - 1] == '\'',
          "truncation left an unterminated literal");
    return 0;
}

/* === B. The select-reply parser === */

/*
 * Bodies exactly as db_socket_exec() hands them over: the lines above the
 * marker, joined by '\n', with no trailing newline.
 *
 * Copied from a live runner rather than from Query.java, narration included -
 * the table is only the middle of a real reply, and a parser tested against
 * the documented table alone passes here and fails on the wire. The trailing
 * "Transaction N committed." is the specific line that makes "the count is on
 * the last line" wrong.
 */
#define REPLY_TWO_ROWS                                                         \
    "Started a new transaction tid = 3\n"                                      \
    "Added scan of table user\n"                                               \
    "The query plan is:\n"                                                     \
    "  \xcf\x80(user.id,user.name,user.salt),card:0\n"                         \
    "  |\n"                                                                    \
    "scan(user)\n"                                                             \
    "\n"                                                                       \
    "id\tname\tsalt\t\n"                                                       \
    "-------------------------\n"                                              \
    "1\talice\tbeef\n"                                                         \
    "2\tbob\tcafe\n"                                                           \
    "\n"                                                                       \
    " 2 rows.\n"                                                               \
    "Transaction 3 committed."

#define REPLY_NO_ROWS                                                          \
    "Started a new transaction tid = 4\n"                                      \
    "id\tname\t\n"                                                             \
    "------------------\n"                                                     \
    "\n"                                                                       \
    " 0 rows.\n"                                                               \
    "Transaction 4 committed."

static int test_rows_counts_and_splits(void)
{
    const char *f[3];
    size_t len[3];

    CHECK(db_row_count(REPLY_TWO_ROWS) == 2, "wrong row count");
    CHECK(db_row_fields(REPLY_TWO_ROWS, 0, f, len, 3) == 3,
          "wrong field count");
    CHECK(len[1] == 5 && memcmp(f[1], "alice", 5) == 0, "row 0 field 1 wrong");
    CHECK(db_row_fields(REPLY_TWO_ROWS, 1, f, len, 3) == 3,
          "wrong field count");
    CHECK(len[0] == 1 && f[0][0] == '2', "row 1 field 0 wrong");
    CHECK(len[2] == 4 && memcmp(f[2], "cafe", 4) == 0, "row 1 field 2 wrong");
    return 0;
}

/*
 * "No such user" and "that is not an answer" are different results, and the
 * login path branches on the difference: zero rows is a 404, no table at all
 * is a 500. A truncated reply must land in the second group - reporting the
 * rows that happened to fit would refuse a login for an account that exists.
 */
static int test_rows_zero_is_not_an_error(void)
{
    CHECK(db_row_count(REPLY_NO_ROWS) == 0, "empty result miscounted");
    CHECK(db_row_count("Invalid SQL expression: nonsense") == -1,
          "an error message parsed as a table");
    CHECK(db_row_count("Inserted 1 rows.\n 1 rows.") == -1,
          "output with no rule line parsed as a table");
    CHECK(db_row_count("id\tname\t\n-------\n1\talice\n2\tbob") == -1,
          "a body cut off mid-table reported a count");
    CHECK(db_row_count("id\tname\t\n-------\n1\talice\n\n 7 rows.") == -1,
          "a count that disagrees with the rows was believed");
    CHECK(db_row_count("") == -1, "empty body parsed as a table");
    CHECK(db_row_count(NULL) == -1, "NULL body parsed as a table");
    return 0;
}

/* The blank line, the " N rows." trailer and the commit notice after it are
 * not rows, and neither is a stored value that reads like one. */
static int test_rows_ignores_the_trailer(void)
{
    static const char *reply = "id\tmsg\t\n"
                               "----------\n"
                               "1\t9 rows.\n"
                               "\n"
                               " 1 rows.\n"
                               "Transaction 9 committed.";
    const char *f[2];
    size_t len[2];

    CHECK(db_row_count(reply) == 1, "trailer or blank line counted as a row");
    CHECK(db_row_fields(reply, 0, f, len, 2) == 2, "wrong field count");
    CHECK(len[1] == 7 && memcmp(f[1], "9 rows.", 7) == 0, "field text wrong");
    CHECK(db_row_fields(reply, 1, f, len, 2) == -1, "read past the last row");
    CHECK(db_row_fields(reply, -1, f, len, 2) == -1, "accepted a negative row");
    return 0;
}

/*
 * Fields are positional, so a row with more of them than the caller expected
 * is a schema disagreement, not a truncation to absorb quietly. The count
 * comes back honest even though only max were written.
 */
static int test_rows_reports_extra_fields(void)
{
    static const char *reply = "a\tb\tc\t\n"
                               "----------\n"
                               "1\t\t3\n"
                               "\n"
                               " 1 rows.\n"
                               "Transaction 2 committed.";
    const char *f[2];
    size_t len[2];

    CHECK(db_row_fields(reply, 0, f, len, 2) == 3, "extra field not reported");
    /* An empty value is a value: dropping it would shift every field after it,
     * which is exactly how a salt gets read as a digest. */
    CHECK(len[1] == 0, "empty field was skipped");
    CHECK(len[0] == 1 && f[0][0] == '1', "first field wrong");
    return 0;
}

/*
 * Four cases pinning the reply shapes src/tetrisd/history.c's read functions
 * depend on (issue #79) - none of them exercise anything new in rows.c
 * itself, since the wire has no notion of ORDER BY or GROUP BY. They exist
 * so a reader looking for "why does history.c trust field position 0 to be
 * the newest row" or "why does it never call read_scalar() before checking
 * rows > 0" finds the answer pinned here, against literal strings, rather
 * than only in a comment.
 */

#define REPLY_HISTORY_FIVE_ROWS                                                \
    "Started a new transaction tid = 9\n"                                      \
    "The query plan is:\n"                                                     \
    "  "                                                                       \
    "\xcf\x80(history.user_name,history.score,history.lines,history.ts_start," \
    "history.ts_end),card:0\n"                                                 \
    "  |\n"                                                                    \
    "scan(history)\n"                                                          \
    "\n"                                                                       \
    "user_name\tscore\tlines\tts_start\tts_end\t\n"                            \
    "----------------------------------------------\n"                         \
    "carol\t300\t9\t1050\t1090\n"                                              \
    "bob\t200\t6\t1030\t1055\n"                                                \
    "alice\t100\t3\t1000\t1020\n"                                              \
    "carol\t80\t2\t950\t965\n"                                                 \
    "bob\t50\t1\t900\t910\n"                                                   \
    "\n"                                                                       \
    " 5 rows.\n"                                                               \
    "Transaction 9 committed."

/* history_read_recent()/history_read_player() both trust the wire order to
 * already be `order by id desc` - the reader has no id column to re-sort by,
 * only field position, so row 0 must be whatever the wire sent first. */
static int test_rows_preserve_order_across_several_rows(void)
{
    const char *f[5];
    size_t len[5];

    CHECK(db_row_count(REPLY_HISTORY_FIVE_ROWS) == 5, "wrong row count");
    CHECK(db_row_fields(REPLY_HISTORY_FIVE_ROWS, 0, f, len, 5) == 5,
          "wrong field count");
    CHECK(len[0] == 5 && memcmp(f[0], "carol", 5) == 0,
          "row 0 must be the first row the wire sent");
    CHECK(db_row_fields(REPLY_HISTORY_FIVE_ROWS, 4, f, len, 5) == 5,
          "wrong field count");
    CHECK(len[0] == 3 && memcmp(f[0], "bob", 3) == 0,
          "row 4 must be the last row the wire sent, not re-sorted");
    return 0;
}

#define REPLY_BEST_SCORES_GROUPED                                              \
    "Started a new transaction tid = 11\n"                                     \
    "GROUP BY FIELD : history.user_name\n"                                     \
    "\n"                                                                       \
    "user_name\tmax (history.score)\t\n"                                       \
    "--------------------------------\n"                                       \
    "alice\t500\n"                                                             \
    "bob\t90\n"                                                                \
    "\n"                                                                       \
    " 2 rows.\n"                                                               \
    "Transaction 11 committed."

/* history_read_best_scores()'s whole reply shape: one row per distinct
 * account, two columns. Nothing in rows.c is GROUP BY-specific, but a reader
 * should find this pinned rather than have to trust that by inference. */
static int test_rows_group_by_two_columns(void)
{
    const char *f[2];
    size_t len[2];

    CHECK(db_row_count(REPLY_BEST_SCORES_GROUPED) == 2, "wrong row count");
    CHECK(db_row_fields(REPLY_BEST_SCORES_GROUPED, 0, f, len, 2) == 2,
          "wrong field count");
    CHECK(len[0] == 5 && memcmp(f[0], "alice", 5) == 0, "group 0 name wrong");
    CHECK(len[1] == 3 && memcmp(f[1], "500", 3) == 0, "group 0 max wrong");
    CHECK(db_row_fields(REPLY_BEST_SCORES_GROUPED, 1, f, len, 2) == 2,
          "wrong field count");
    CHECK(len[0] == 3 && memcmp(f[0], "bob", 3) == 0, "group 1 name wrong");
    return 0;
}

#define REPLY_AGGREGATE_EMPTY                                                  \
    "Started a new transaction tid = 12\n"                                     \
    "max (history.score)\t\n"                                                  \
    "------------------------\n"                                               \
    "\n"                                                                       \
    " 0 rows.\n"                                                               \
    "Transaction 12 committed."

/*
 * Whether SimpleDB prints zero rows or one row holding a sentinel for an
 * aggregate over an empty group is exactly the ambiguity
 * include/libtetrisdb/socket/db.h documents for db_next_id(). history.c's
 * read_scalar() never has to resolve it: history_read_player() only ever
 * calls it after an earlier query has already shown the group is non-empty.
 * This pins the one shape that is documented (zero rows), not the other.
 */
static int test_rows_aggregate_over_empty_group(void)
{
    CHECK(db_row_count(REPLY_AGGREGATE_EMPTY) == 0,
          "an aggregate over no rows must read as zero rows, not an error");
    return 0;
}

/* What an undersized db_socket_exec() buffer leaves behind: rows, cut off
 * before the blank line and trailer that prove the table is complete.
 * history_read_recent()/history_read_best_scores() depend on this reading as
 * -1 ("unavailable"), never as a truncated-but-believed row count. */
static int test_rows_oversized_reply_is_not_a_table(void)
{
    static const char *cut_off =
        "user_name\tscore\tlines\tts_start\tts_end\t\n"
        "----------------------------------------------\n"
        "carol\t300\t9\t1050\t1090\n"
        "bob\t200\t6\t1030\t1055\n"
        "alice\t100\t3\t1000\t1020";

    CHECK(db_row_count(cut_off) == -1,
          "a reply truncated before its trailer was read as a row count");
    return 0;
}

/* === C. Scripted peer: deadlines, markers, and a runner that dies === */

/* What the fake runner does after a client connects. */
typedef enum
{
    PEER_NEVER_ACCEPTS, /* connected, but nobody is serving: no greeting     */
    PEER_GREETS_ONLY,   /* greets, then never answers a statement            */
    PEER_RETRY,         /* greets, then reports a deadlock abort             */
    PEER_ERROR,         /* greets, then reports a failed statement with text */
    PEER_HANGS_UP       /* greets, then closes mid-exchange                  */
} peer_script_t;

/* Bind and listen before forking, so a connect can never race the bind. */
static int listen_unix(const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* The scratch directory is whatever the previous case left, and one of them
     * ends by removing it. Depending on case order for a directory to exist is
     * the kind of coupling that fails only when someone reorders main(). */
    (void)mkdir(TEST_DIR, 0755);

    (void)unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

/* Serve exactly one connection from lfd, then exit. Returns 0 in the parent
 * for PEER_NEVER_ACCEPTS (no child at all: a full backlog with nobody in
 * accept() is what a queued connection looks like from outside). */
static pid_t fork_peer(int lfd, peer_script_t script)
{
    if (script == PEER_NEVER_ACCEPTS)
        return 0;

    pid_t pid = fork();
    if (pid != 0)
        return pid;

    int c = accept(lfd, NULL, NULL);
    if (c < 0)
        _exit(1);
    (void)!write(c, "<<READY>>\n", 10);

    if (script != PEER_GREETS_ONLY)
    {
        /* Wait for a statement before answering, so the reply cannot be sitting
         * in the buffer before the client has asked anything. */
        char buf[512];
        (void)!read(c, buf, sizeof(buf));
        switch (script)
        {
        case PEER_RETRY:
            (void)!write(c, "<<END retry>>\n", 14);
            break;
        case PEER_ERROR:
            (void)!write(c, "no such table\n<<END error>>\n", 28);
            break;
        default:
            break; /* PEER_HANGS_UP: say nothing, just close */
        }
    }

    if (script == PEER_GREETS_ONLY)
        pause(); /* hold the connection open until the parent kills us */
    close(c);
    _exit(0);
}

static void stop_peer(pid_t pid, int lfd)
{
    if (pid > 0)
    {
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    if (lfd >= 0)
        close(lfd);
    (void)unlink(TEST_SOCK);
}

static void test_socket_opts(db_socket_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    snprintf(opts->sock, sizeof(opts->sock), "%s", TEST_SOCK);
    opts->timeout_ms = TEST_TIMEOUT_MS;
}

/* A missing socket is the runner being down, which must fail immediately
 * rather than after the deadline: there is nothing to wait for. */
static int test_socket_open_without_a_runner(void)
{
    db_socket_opts_t opts;

    test_socket_opts(&opts);
    (void)unlink(TEST_SOCK);
    CHECK(db_socket_open(&opts) == NULL, "opened a connection to nothing");

    memset(&opts, 0, sizeof(opts));
    opts.sock[0] = '\0';
    CHECK(db_socket_open(&opts) == NULL, "opened with no socket path set");
    return 0;
}

/*
 * The case the deadline exists for. The socket is real, the connect succeeds,
 * and the greeting never comes - which is precisely a connection queued behind
 * a full --sessions pool. Without a deadline this is an invisible, unbounded
 * hang inside a session process.
 */
static int test_socket_open_gives_up_without_a_greeting(void)
{
    db_socket_opts_t opts;

    test_socket_opts(&opts);
    int lfd = listen_unix(TEST_SOCK);
    CHECK(lfd >= 0, "cannot listen on the test socket");

    alarm(WATCHDOG_SECS);
    db_socket_t *c = db_socket_open(&opts);
    alarm(0);
    db_socket_close(c);
    stop_peer(0, lfd);

    CHECK(c == NULL, "a connection with no greeting was accepted");
    return 0;
}

/* The deadline spans the connection, not each statement, and an expired one
 * stays expired: a caller's retry loop cannot outrun it. */
static int test_exec_deadline_is_sticky(void)
{
    db_socket_opts_t opts;

    test_socket_opts(&opts);
    int lfd = listen_unix(TEST_SOCK);
    CHECK(lfd >= 0, "cannot listen on the test socket");
    pid_t peer = fork_peer(lfd, PEER_GREETS_ONLY);

    alarm(WATCHDOG_SECS);
    db_socket_t *c = db_socket_open(&opts);
    CHECK(c != NULL, "greeting was not accepted");

    char body[64] = "stale";
    db_status_t first =
        db_socket_exec(c, "select 1 from log;", body, sizeof(body));
    db_status_t second = db_socket_exec(c, "select 1 from log;", NULL, 0);
    alarm(0);

    db_socket_close(c);
    stop_peer(peer, lfd);

    CHECK(first == DB_TIMEOUT, "a silent runner did not time out");
    CHECK(body[0] == '\0', "body was not cleared on a failed exchange");
    CHECK(second == DB_TIMEOUT, "the expired deadline was not sticky");
    return 0;
}

/*
 * <<END retry>> is a distinct outcome, and this is the only place it is
 * asserted: a real deadlock cannot be produced on demand (#44 saw none in 320
 * concurrent logins), and folding retry into error is the failure invariant 3
 * describes - a client that silently drops writes.
 */
static int test_retry_is_not_an_error(void)
{
    db_socket_opts_t opts;

    test_socket_opts(&opts);
    int lfd = listen_unix(TEST_SOCK);
    CHECK(lfd >= 0, "cannot listen on the test socket");
    pid_t peer = fork_peer(lfd, PEER_RETRY);

    alarm(WATCHDOG_SECS);
    db_socket_t *c = db_socket_open(&opts);
    CHECK(c != NULL, "greeting was not accepted");
    db_status_t st = db_socket_exec(c, "insert into log values (1);", NULL, 0);
    alarm(0);

    db_socket_close(c);
    stop_peer(peer, lfd);

    CHECK(st == DB_RETRY, "<<END retry>> was not reported as DB_RETRY");
    return 0;
}

/* A rejected statement is the runner's answer, not a broken connection: the
 * body comes back and the caller may keep using the connection. */
static int test_error_marker_carries_its_body(void)
{
    db_socket_opts_t opts;

    test_socket_opts(&opts);
    int lfd = listen_unix(TEST_SOCK);
    CHECK(lfd >= 0, "cannot listen on the test socket");
    pid_t peer = fork_peer(lfd, PEER_ERROR);

    char body[64];
    alarm(WATCHDOG_SECS);
    db_socket_t *c = db_socket_open(&opts);
    CHECK(c != NULL, "greeting was not accepted");
    db_status_t st =
        db_socket_exec(c, "select 1 from nope;", body, sizeof(body));
    alarm(0);

    db_socket_close(c);
    stop_peer(peer, lfd);

    CHECK(st == DB_ERROR, "a rejected statement was not DB_ERROR");
    CHECK(strcmp(body, "no such table") == 0, "error body was not returned");
    return 0;
}

/* A runner that dies mid-exchange is DB_IO, and the caller finds out rather
 * than waiting out the deadline for it. */
static int test_hangup_is_reported(void)
{
    db_socket_opts_t opts;

    test_socket_opts(&opts);
    int lfd = listen_unix(TEST_SOCK);
    CHECK(lfd >= 0, "cannot listen on the test socket");
    pid_t peer = fork_peer(lfd, PEER_HANGS_UP);

    alarm(WATCHDOG_SECS);
    db_socket_t *c = db_socket_open(&opts);
    CHECK(c != NULL, "greeting was not accepted");
    db_status_t st = db_socket_exec(c, "select 1 from log;", NULL, 0);
    db_status_t again = db_socket_exec(c, "select 1 from log;", NULL, 0);
    alarm(0);

    db_socket_close(c);
    stop_peer(peer, lfd);

    CHECK(st == DB_IO, "a dead runner was not reported as DB_IO");
    CHECK(again == DB_IO, "a dead connection was used again");
    return 0;
}

/*
 * One statement per line is the framing, so a statement carrying a newline
 * would submit a second one whose response nobody reads, leaving every later
 * reply on the connection off by one. Refused before it reaches the wire.
 */
static int test_newline_statement_is_refused(void)
{
    db_socket_opts_t opts;

    test_socket_opts(&opts);
    int lfd = listen_unix(TEST_SOCK);
    CHECK(lfd >= 0, "cannot listen on the test socket");
    pid_t peer = fork_peer(lfd, PEER_GREETS_ONLY);

    alarm(WATCHDOG_SECS);
    db_socket_t *c = db_socket_open(&opts);
    CHECK(c != NULL, "greeting was not accepted");
    /* If this were sent, the peer's silence would make it DB_TIMEOUT. */
    db_status_t st = db_socket_exec(c, "select 1 from log;\ndrop;", NULL, 0);
    alarm(0);

    db_socket_close(c);
    stop_peer(peer, lfd);

    CHECK(st == DB_ERROR, "a two-statement line was sent to the runner");
    return 0;
}

/* === D. End to end === */

/* The two things a runner needs, asked separately: they are different
 * problems with different fixes, and one answer for both is why nobody
 * noticed these tests had never run on a push. */
/* Has the caller opted out of everything needing a real runner? `make test-ci`
 * sets it, because CI has no usable java. Checked before have_java(), so the
 * probe never runs, and it outranks TETRISH_REQUIRE_RUNNER: a deliberate
 * opt-out is not a regression. */
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
    while (waitpid(pid, &status, 0) < 0)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void test_opts(db_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    snprintf(opts->dir, sizeof(opts->dir), "%s", TEST_DIR);
    snprintf(opts->jar, sizeof(opts->jar), "%s", TEST_JAR);
    snprintf(opts->java, sizeof(opts->java), "%s", "java");
}

/* Write two rows, shut down, then reopen and read them back - which also
 * proves the clean shutdown flushed them to disk. */
static int test_round_trip(void)
{
    db_opts_t opts;
    unsigned long dropped = 1, errors = 1;

    reset_dir();
    test_opts(&opts);
    CHECK(db_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "create failed");

    db_t *db = db_start(&opts, NULL, NULL, 0);
    CHECK(db != NULL, "cannot start PipeRunner");

    char quoted[64];
    char sql[256];
    db_quote(quoted, sizeof(quoted), "it's fine");
    snprintf(sql, sizeof(sql), "insert into log values (1, 42, 7, 'INFO', %s);",
             quoted);
    CHECK(db_submit(db, sql) == 0, "submit rejected");
    CHECK(db_submit(
              db, "insert into log values (2, 42, 8, 'WARN', 'second');") == 0,
          "submit rejected");

    db_stop(db, &dropped, &errors);
    CHECK(dropped == 0, "statements were dropped");
    CHECK(errors == 0, "SimpleDB rejected a statement");

    /* Reopen and let the startup probe read the table back. */
    char body[2048];
    db = db_start(&opts, "select max(id) from log;", body, sizeof(body));
    CHECK(db != NULL, "cannot restart PipeRunner");
    db_stop(db, NULL, NULL);

    CHECK(strstr(body, "max (log.id)") != NULL, "probe returned no result");
    /* The rule line, then the value: 2, because two rows were written. */
    const char *rule = strstr(body, "---");
    CHECK(rule != NULL, "probe output has no result table");
    const char *nl = strchr(rule, '\n');
    CHECK(nl != NULL && atoi(nl + 1) == 2,
          "max(id) is not 2 after two inserts");
    return 0;
}

/* A statement the parser rejects must be counted, not silently swallowed,
 * and must not take the connection down with it. */
static int test_bad_sql_is_counted(void)
{
    db_opts_t opts;
    unsigned long dropped = 0, errors = 0;

    reset_dir();
    test_opts(&opts);
    CHECK(db_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "create failed");

    db_t *db = db_start(&opts, NULL, NULL, 0);
    CHECK(db != NULL, "cannot start PipeRunner");

    CHECK(db_submit(db, "insert into nosuchtable values (1);") == 0,
          "submit rejected");
    CHECK(db_submit(db, "insert into log values (1, 1, 1, 'INFO', 'after');") ==
              0,
          "submit rejected");

    db_stop(db, &dropped, &errors);
    CHECK(errors == 1, "bad statement was not counted as an error");
    CHECK(dropped == 0, "good statement after a bad one was dropped");
    return 0;
}

/*
 * How the tests start a runner: through the library, exactly as bin/tetrisdb
 * will. That is deliberate rather than convenient - a hand-rolled fork/exec
 * here would drift from the argv the launcher actually uses, and then these
 * cases would prove nothing about it.
 *
 * recover = 0 is the one field that is load-bearing rather than tuning.
 * SimpleDB's write-ahead log is a file called "log" in the CURRENT DIRECTORY,
 * shared by every runner ever started from the repo root, while this table is
 * created fresh for each case - so the startup recovery pass replays a
 * previous run's records onto a brand new heap file and the counts below come
 * back wrong in a way that looks like a parser bug. There is nothing
 * legitimate for a table created seconds ago to recover.
 */
static void test_runner_opts(db_runner_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    snprintf(opts->java, sizeof(opts->java), "%s", DB_DEFAULT_JAVA);
    snprintf(opts->dir, sizeof(opts->dir), "%s", TEST_DIR);
    snprintf(opts->jar, sizeof(opts->jar), "%s", TEST_JAR);
    snprintf(opts->ipc, sizeof(opts->ipc), "%s", TEST_SOCK);
    opts->sessions = 4;
    opts->recover = 0;
}

static void stop_runner(pid_t pid)
{
    kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    (void)unlink(TEST_SOCK);
}

/* Run fn against a freshly created table and a live runner, and take the
 * runner down afterwards whichever way fn went. */
static int with_runner(int (*fn)(void))
{
    db_runner_opts_t opts;

    reset_dir();
    CHECK(db_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "create failed");

    test_runner_opts(&opts);
    pid_t pid = db_runner_spawn(&opts, -1);
    CHECK(pid > 0, "cannot spawn a runner");

    int rc = db_runner_wait(TEST_SOCK, pid, 20000);
    if (rc == 0)
        rc = fn();
    else
        test_output_failure_detail("the runner never accepted a connection",
                                   __FILE__, __LINE__);

    stop_runner(pid);
    return rc;
}

/*
 * The launcher's own failure modes, none of which need a JVM: a runner that
 * cannot start must say which of the two things is missing, and must not leave
 * a child behind for the caller to reap.
 */
static int test_runner_refuses_before_forking(void)
{
    db_runner_opts_t opts;

    test_runner_opts(&opts);
    opts.dir[0] = '\0';
    CHECK(db_runner_spawn(&opts, -1) < 0, "spawned with no data directory");

    test_runner_opts(&opts);
    snprintf(opts.jar, sizeof(opts.jar), "%s", "db/dist/does-not-exist.jar");
    CHECK(db_runner_spawn(&opts, -1) < 0, "spawned with a missing jar");

    test_runner_opts(&opts);
    snprintf(opts.java, sizeof(opts.java), "%s",
             "definitely-not-a-java-binary");
    CHECK(db_runner_spawn(&opts, -1) < 0, "spawned with an unrunnable java");

    /* The catalog is read once at startup, so a runner started before the
     * tables exist serves a database with no tables in it until it restarts. */
    reset_dir();
    test_runner_opts(&opts);
    CHECK(db_runner_spawn(&opts, -1) < 0, "spawned with no catalog");
    return 0;
}

/* Waiting on a socket nobody is serving must end, and end quickly when the
 * child is already gone rather than sitting out the whole timeout. */
static int test_runner_wait_gives_up(void)
{
    (void)unlink(TEST_SOCK);

    alarm(WATCHDOG_SECS);
    CHECK(db_runner_wait(TEST_SOCK, -1, 200) < 0, "waited on nothing and won");

    /* A child that exits immediately: the wait must notice the death, not the
     * deadline, so the exit status can be reported. */
    pid_t pid = fork();
    CHECK(pid >= 0, "cannot fork");
    if (pid == 0)
        _exit(3);
    CHECK(db_runner_wait(TEST_SOCK, pid, 20000) < 0, "a dead child looked up");
    alarm(0);
    return 0;
}

/* Insert, read back, and check that a rejected statement leaves the
 * connection usable - the runner's answer to a bad statement is an answer,
 * not a broken session. */
static int conn_queries(void)
{
    db_socket_opts_t opts;
    char body[1024];
    const char *f[2];
    size_t len[2];

    test_socket_opts(&opts);
    opts.timeout_ms = 5000; /* a cold JVM's first statement is not fast */

    db_socket_t *c = db_socket_open(&opts);
    CHECK(c != NULL, "cannot connect to the runner");

    CHECK(db_socket_exec(c, "insert into log values (1, 7, 7, 'INFO', 'one');",
                         NULL, 0) == DB_OK,
          "insert failed");
    CHECK(db_socket_exec(c, "insert into log values (2, 7, 8, 'WARN', 'two');",
                         NULL, 0) == DB_OK,
          "insert failed");

    CHECK(db_socket_exec(c, "select id, msg from log;", body, sizeof(body)) ==
              DB_OK,
          "select failed");
    CHECK(db_row_count(body) == 2, "select did not return two rows");
    CHECK(db_row_fields(body, 1, f, len, 2) == 2, "row 1 has the wrong shape");
    CHECK(len[1] == 3 && memcmp(f[1], "two", 3) == 0,
          "row 1 carries wrong text");

    /* A select that matches nothing is the login path's "no such user": it must
     * read as zero rows, which is a 404, and never as no table at all, which is
     * a 500. The distinction is invisible in a body until something reads it.
     */
    CHECK(db_socket_exec(c, "select id from log where msg = 'nobody';", body,
                         sizeof(body)) == DB_OK,
          "select matching nothing failed");
    CHECK(db_row_count(body) == 0, "an empty result did not read as zero rows");

    CHECK(db_socket_exec(c, "select * from nosuchtable;", body, sizeof(body)) ==
              DB_ERROR,
          "a bad statement was not DB_ERROR");
    CHECK(db_socket_exec(c, "select id from log;", body, sizeof(body)) == DB_OK,
          "the connection did not survive a rejected statement");
    CHECK(db_row_count(body) == 2, "row count changed after a bad statement");

    db_socket_close(c);
    return 0;
}

/*
 * A transaction is four ordinary statements on one connection, which is the
 * whole reason the API needed nothing added for it. Both endings matter to
 * registration: commit is the account, rollback is the 409.
 */
static int conn_transactions(void)
{
    db_socket_opts_t opts;
    char body[1024];

    test_socket_opts(&opts);
    opts.timeout_ms = 5000;

    db_socket_t *c = db_socket_open(&opts);
    CHECK(c != NULL, "cannot connect to the runner");

    CHECK(db_socket_exec(c, "set transaction read write;", NULL, 0) == DB_OK,
          "cannot open a transaction");
    CHECK(db_socket_exec(c, "insert into log values (1, 1, 1, 'INFO', 'kept');",
                         NULL, 0) == DB_OK,
          "insert in transaction failed");
    CHECK(db_socket_exec(c, "commit;", NULL, 0) == DB_OK, "commit failed");

    CHECK(db_socket_exec(c, "set transaction read write;", NULL, 0) == DB_OK,
          "cannot open a second transaction");
    CHECK(db_socket_exec(c, "insert into log values (2, 2, 2, 'INFO', 'gone');",
                         NULL, 0) == DB_OK,
          "insert in transaction failed");
    CHECK(db_socket_exec(c, "rollback;", NULL, 0) == DB_OK, "rollback failed");
    db_socket_close(c);

    /* A second connection, because the point is what the runner kept, not what
     * this session remembers. */
    c = db_socket_open(&opts);
    CHECK(c != NULL, "cannot reconnect to the runner");
    CHECK(db_socket_exec(c, "select id from log;", body, sizeof(body)) == DB_OK,
          "select failed");
    db_socket_close(c);

    CHECK(db_row_count(body) == 1,
          "the committed row is missing or the rolled-back one survived");
    return 0;
}

static int test_socket_queries(void)
{
    return with_runner(conn_queries);
}
static int test_socket_transactions(void)
{
    return with_runner(conn_transactions);
}

int main(void)
{
    test_output_begin("test_db");

    run("creates catalog entry and one-page data file", test_creates_table);
    run("second create leaves existing table alone", test_create_is_idempotent);
    run("refuses to run without a data directory", test_dir_is_required);
    run("quotes plain text", test_quote_plain);
    run("doubles embedded quotes", test_quote_doubles_quotes);
    run("keeps an injection payload inside one literal",
        test_quote_injection_stays_one_literal);
    run("truncates without breaking the literal", test_quote_truncates_safely);

    run("counts rows and splits fields of a select reply",
        test_rows_counts_and_splits);
    run("tells no rows from no result table", test_rows_zero_is_not_an_error);
    run("does not mistake the trailer for a row",
        test_rows_ignores_the_trailer);
    run("reports a row with more fields than expected",
        test_rows_reports_extra_fields);
    run("preserves wire order across several rows",
        test_rows_preserve_order_across_several_rows);
    run("reads a two-column group-by reply", test_rows_group_by_two_columns);
    run("reads an aggregate over an empty group as zero rows",
        test_rows_aggregate_over_empty_group);
    run("reports an oversized reply as unavailable, not truncated",
        test_rows_oversized_reply_is_not_a_table);

    run("refuses to start a runner it cannot start",
        test_runner_refuses_before_forking);
    run("gives up waiting for a runner that is not coming",
        test_runner_wait_gives_up);
    run("fails at once when no runner is listening",
        test_socket_open_without_a_runner);
    run("gives up on a connection that is never greeted",
        test_socket_open_gives_up_without_a_greeting);
    run("keeps returning DB_TIMEOUT once the deadline has passed",
        test_exec_deadline_is_sticky);
    run("reports a deadlock abort as DB_RETRY, not an error",
        test_retry_is_not_an_error);
    run("returns the body of a rejected statement",
        test_error_marker_carries_its_body);
    run("reports a runner that hangs up as DB_IO", test_hangup_is_reported);
    run("refuses a statement carrying a newline",
        test_newline_statement_is_refused);

    if (!runner_disabled() && have_jar() && have_java())
    {
        run("writes rows and reads them back after restart", test_round_trip);
        run("counts a rejected statement and keeps going",
            test_bad_sql_is_counted);
        run("reads rows over a socket and survives a bad statement",
            test_socket_queries);
        run("commits and rolls back across statements",
            test_socket_transactions);
    }
    else
    {
        /*
         * TETRISH_REQUIRE_RUNNER=1 turns this skip into a failure, for a CI job
         * that is not permitted to skip. A printed skip line is not a
         * mechanism: the jar is gitignored and CI has no JDK step, so these
         * tests have never run on a push, which is this repository proving the
         * point (#54).
         */
        if (!runner_disabled() && getenv("TETRISH_REQUIRE_RUNNER") != NULL)
        {
            test_output_failure_detail(
                "TETRISH_REQUIRE_RUNNER is set and the runner is unavailable",
                __FILE__, __LINE__);
            tests_run++;
            tests_failed++;
            test_output_fail("runner integration tests are available");
        }
        else
        {
            test_output_skip("runner integration tests",
                             runner_disabled() ? "TETRISH_NO_RUNNER is set"
                             : !have_jar()     ? "no " TEST_JAR
                                             " - run `ant dist` in db/"
                                           : "java did not run");
            tests_skipped++;
        }
    }

    reset_dir();
    test_output_summary(tests_run + tests_skipped, tests_failed, tests_skipped);
    return tests_failed == 0 ? 0 : 1;
}
