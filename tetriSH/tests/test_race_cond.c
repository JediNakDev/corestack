/* End-to-end database allocation races for registration and game history. */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_output.h"

#include "libhtttp/htttp.h"
#include "libtetrisauth/auth.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/db.h"
#include "libtetrissh/tetrissh.h"
#include "libtetrisutil/limits.h"
#include "tetrisd/history.h"
#include "libtetrisdb/socket/conf.h"
#include "tetrisdb/runner.h"

#define TEST_ROOT "var/race_cond_test"
#define TEST_DB_DIR TEST_ROOT "/db"
#define TEST_RUN_DIR TEST_ROOT "/run"
#define TEST_SOCK TEST_RUN_DIR "/tetrisdb.sock"
#define TEST_RC TEST_ROOT "/.tetrishrc"
#define TEST_SECRET TEST_ROOT "/auth/jwt_secret"
#define TEST_JAR "db/dist/simpledb.jar"

#define RACERS 254
#define REGISTER_ATTEMPTS 3
#define WORKER_TIMEOUT_MS 15000

static int tests_run;
static int tests_failed;
static int tests_skipped;
static pid_t runner = -1;
static unsigned long race_seed;
static const char *last_phase = "setup";

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
    if (fn() == 0)
        test_output_pass(name);
    else
    {
        test_output_failure_detailf(__FILE__, __LINE__, "seed=%lu phase=%s",
                                    race_seed, last_phase);
        tests_failed++;
        test_output_fail(name);
    }
}

static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void nap_ms(int milliseconds)
{
    struct timespec pause = {.tv_sec = milliseconds / 1000,
                             .tv_nsec = (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

static int worker_order(int index)
{
    unsigned long mixed = race_seed + (unsigned long)index * 1103515245UL;
    return (int)(mixed % RACERS);
}

static int release_workers(int fd)
{
    char release = 'R';
    for (int i = 0; i < RACERS; i++)
    {
        ssize_t written;
        do
            written = write(fd, &release, 1);
        while (written < 0 && errno == EINTR);
        if (written != 1)
        {
            close(fd);
            return -1;
        }
    }
    return close(fd);
}

static int reap_workers(pid_t workers[], const char *kind)
{
    int remaining = RACERS;
    int failed = 0;
    long deadline = monotonic_ms() + WORKER_TIMEOUT_MS;
    int status[RACERS];
    memset(status, 0, sizeof status);

    while (remaining > 0 && monotonic_ms() < deadline)
    {
        for (int i = 0; i < RACERS; i++)
        {
            if (workers[i] <= 0)
                continue;
            pid_t done = waitpid(workers[i], &status[i], WNOHANG);
            if (done == workers[i])
            {
                workers[i] = -1;
                remaining--;
                if (!WIFEXITED(status[i]) || WEXITSTATUS(status[i]) != 0)
                {
                    test_output_failure_detailf(
                        __FILE__, __LINE__,
                        "worker=%s index=%d seed=%lu phase=%s status=%d", kind,
                        i, race_seed, last_phase, status[i]);
                    failed = 1;
                }
            }
            else if (done < 0)
            {
                workers[i] = -1;
                remaining--;
                failed = 1;
            }
        }
        if (remaining > 0)
            nap_ms(5);
    }

    if (remaining == 0)
        return failed ? -1 : 0;
    for (int i = 0; i < RACERS; i++)
        if (workers[i] > 0)
            kill(workers[i], SIGTERM);
    for (int i = 0; i < RACERS; i++)
        if (workers[i] > 0)
        {
            if (waitpid(workers[i], NULL, WNOHANG) == 0)
                kill(workers[i], SIGKILL);
            while (waitpid(workers[i], NULL, 0) < 0 && errno == EINTR)
                ;
        }
    test_output_failure_detailf(__FILE__, __LINE__,
                                "worker=%s deadline_ms=%d seed=%lu phase=%s",
                                kind, WORKER_TIMEOUT_MS, race_seed, last_phase);
    return -1;
}

static void abort_workers(pid_t workers[], int count)
{
    for (int i = 0; i < count; i++)
        if (workers[i] > 0)
            (void)kill(workers[i], SIGTERM);
    for (int i = 0; i < count; i++)
    {
        if (workers[i] <= 0)
            continue;
        while (waitpid(workers[i], NULL, 0) < 0 && errno == EINTR)
            ;
        workers[i] = -1;
    }
}

static int load_seed(void)
{
    const char *text = getenv("TETRISH_RACE_SEED");
    char *end;
    errno = 0;
    race_seed = text != NULL ? strtoul(text, &end, 10) : 20260811UL;
    if (text != NULL && (errno != 0 || end == text || *end != '\0'))
        return -1;
    return 0;
}

static int runner_disabled(void)
{
    return getenv("TETRISH_NO_RUNNER") != NULL;
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
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    ssize_t written = len == 0 ? 0 : write(fd, data, len);
    int close_result = close(fd);
    return written == (ssize_t)len && close_result == 0 ? 0 : -1;
}

static void reset_fixture(void)
{
    (void)sem_unlink("/tetrish_register");
    (void)sem_unlink(HISTORY_SEM_NAME);
    (void)unlink(TEST_DB_DIR "/catalog.txt");
    (void)unlink(TEST_DB_DIR "/user.dat");
    (void)unlink(TEST_DB_DIR "/history.dat");
    (void)unlink(TEST_SOCK);
    (void)unlink(TEST_RC);
    (void)unlink(TEST_SECRET);
    (void)rmdir(TEST_ROOT "/auth");
    (void)rmdir(TEST_DB_DIR);
    (void)rmdir(TEST_RUN_DIR);
    (void)rmdir(TEST_ROOT);
}

static int setup_fixture(void)
{
    char cwd[PATH_MAX];
    char root_path[PATH_MAX];
    char socket_path[PATH_MAX];
    char rc[PATH_MAX + 128];
    unsigned char secret[32];

    reset_fixture();
    CHECK(mkdir(TEST_ROOT, 0700) == 0, "create race fixture root");
    CHECK(mkdir(TEST_ROOT "/auth", 0700) == 0,
          "create race fixture auth directory");
    CHECK(mkdir(TEST_RUN_DIR, 0700) == 0, "create race fixture run directory");
    memset(secret, 0x2b, sizeof secret);
    CHECK(write_file(TEST_SECRET, secret, sizeof secret) == 0,
          "write race fixture JWT secret");
    CHECK(db_ensure_table(TEST_DB_DIR, TETRISAUTH_DB_TABLE,
                          TETRISAUTH_DB_SCHEMA) == 0,
          "create race fixture user table");
    CHECK(db_ensure_table(TEST_DB_DIR, HISTORY_DB_TABLE, HISTORY_DB_SCHEMA) ==
              0,
          "create race fixture history table");

    CHECK(getcwd(cwd, sizeof cwd) != NULL, "resolve repository directory");
    int n = snprintf(socket_path, sizeof socket_path, "%s/%s", cwd, TEST_SOCK);
    CHECK(n > 0 && (size_t)n < sizeof socket_path,
          "build absolute race socket path");
    n = snprintf(root_path, sizeof root_path, "%s/%s", cwd, TEST_ROOT);
    CHECK(n > 0 && (size_t)n < sizeof root_path,
          "build absolute race fixture root");
    n = snprintf(rc, sizeof rc,
                 "db_ipc = %s\ndb_timeout = 4000\n"
                 "auth_pbkdf2_iters = 1000\n",
                 socket_path);
    CHECK(n > 0 && (size_t)n < sizeof rc, "build race fixture rc");
    CHECK(write_file(TEST_RC, rc, (size_t)n) == 0, "write race fixture rc");
    CHECK(setenv("TETRISH_ROOT", root_path, 1) == 0, "set race fixture root");

    db_runner_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    snprintf(opts.java, sizeof(opts.java), "%s", DB_DEFAULT_JAVA);
    snprintf(opts.dir, sizeof opts.dir, "%s", TEST_DB_DIR);
    snprintf(opts.jar, sizeof opts.jar, "%s", TEST_JAR);
    snprintf(opts.ipc, sizeof opts.ipc, "%s", TEST_SOCK);
    opts.sessions = 8;
    opts.recover = 0;
    runner = db_runner_spawn(&opts, -1);
    CHECK(runner > 0, "spawn race fixture SocketRunner");
    CHECK(db_runner_wait(TEST_SOCK, runner, 20000) == 0,
          "race fixture SocketRunner must accept connections");
    return 0;
}

static void stop_fixture(void)
{
    if (runner > 0)
    {
        kill(runner, SIGTERM);
        while (waitpid(runner, NULL, 0) < 0 && errno == EINTR)
            ;
        runner = -1;
    }
    reset_fixture();
}

static int restart_fixture_runner(void)
{
    if (runner > 0)
    {
        CHECK(kill(runner, SIGTERM) == 0, "stop runner before restart");
        long deadline = monotonic_ms() + WORKER_TIMEOUT_MS;
        int status = 0;
        pid_t done = 0;
        while ((done = waitpid(runner, &status, WNOHANG)) == 0 &&
               monotonic_ms() < deadline)
            nap_ms(10);
        CHECK(done == runner, "runner did not exit before restart");
        runner = -1;
    }
    unlink(TEST_SOCK);

    db_runner_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    snprintf(opts.java, sizeof(opts.java), "%s", DB_DEFAULT_JAVA);
    snprintf(opts.dir, sizeof(opts.dir), "%s", TEST_DB_DIR);
    snprintf(opts.jar, sizeof(opts.jar), "%s", TEST_JAR);
    snprintf(opts.ipc, sizeof(opts.ipc), "%s", TEST_SOCK);
    opts.sessions = 8;
    opts.recover = 0;
    runner = db_runner_spawn(&opts, -1);
    CHECK(runner > 0, "restart SocketRunner");
    CHECK(db_runner_wait(TEST_SOCK, runner, 20000) == 0,
          "restarted SocketRunner must accept connections");
    return 0;
}

static int query(const char *sql, char *body, size_t cap)
{
    db_socket_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    snprintf(opts.sock, sizeof opts.sock, "%s", TEST_SOCK);
    opts.timeout_ms = 8000;
    db_socket_t *conn = db_socket_open(&opts);
    if (conn == NULL)
        return -1;
    db_status_t status = db_socket_exec(conn, sql, body, cap);
    db_socket_close(conn);
    return status == DB_OK ? 0 : -1;
}

static void fake_session(session_t *session, int fd)
{
    memset(session, 0, sizeof *session);
    memset(session->key, 0x5a, sizeof session->key);
    session->fd = fd;
    session->established = 1;
}

static void silence_worker_stderr(void)
{
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0)
    {
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
}

static int put(session_t *client, const char *method, const char *path,
               const char *body)
{
    htttp_request_t request;
    uint8_t frame[2048];
    uint32_t len = sizeof frame;
    memset(&request, 0, sizeof request);
    snprintf(request.method, sizeof request.method, "%s", method);
    snprintf(request.path, sizeof request.path, "%s", path);
    if (body != NULL)
    {
        request.body = (const uint8_t *)body;
        request.body_len = (uint32_t)strlen(body);
    }
    if (htttp_serialize_request(&request, frame, &len) != HTTTP_OK)
        return -1;
    return session_send(client, frame, len) == SESSION_OK ? 0 : -1;
}

static int get_status(session_t *client)
{
    uint8_t frame[2048];
    uint32_t len = sizeof frame;
    htttp_response_t response;
    if (session_recv(client, frame, &len) != SESSION_OK ||
        htttp_parse_response(frame, len, &response) != HTTTP_OK)
        return -1;
    return response.status;
}

typedef struct
{
    session_t session;
    int result;
} auth_thread_t;

static void *run_auth(void *arg)
{
    auth_thread_t *auth = arg;
    auth->result = auth_retry_handler(&auth->session);
    return NULL;
}

static int register_worker(int ordinal, int barrier_fd)
{
    char released;
    if (read(barrier_fd, &released, 1) != 1)
        return 1;
    close(barrier_fd);
    silence_worker_stderr();
    if (chdir(TEST_ROOT) != 0)
        return 1;

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    auth_thread_t auth;
    fake_session(&auth.session, sockets[1]);
    auth.result = AUTH_DROP;
    pthread_t thread;
    if (pthread_create(&thread, NULL, run_auth, &auth) != 0)
        return 1;

    session_t client;
    fake_session(&client, sockets[0]);
    char body[64];
    snprintf(body, sizeof body, "herd%03d\nrace-password", ordinal);
    int status = 500;
    for (int attempt = 0; attempt < REGISTER_ATTEMPTS; attempt++)
    {
        if (put(&client, "REGISTER", "/auth/register", body) != 0)
            break;
        status = get_status(&client);
        if (status != 500)
            break;
    }
    if (status != 200 && (put(&client, "GUEST", "/auth/guest", NULL) != 0 ||
                          get_status(&client) != 200))
        status = -1;

    close(sockets[0]);
    (void)pthread_join(thread, NULL);
    close(sockets[1]);
    return (status == 200 || status == 409 || status == 500) &&
                   auth.result == AUTH_OK
               ? 0
               : 1;
}

static int login_worker(int ordinal, int barrier_fd, int shared)
{
    char released;
    if (read(barrier_fd, &released, 1) != 1)
        return 1;
    close(barrier_fd);
    silence_worker_stderr();
    if (chdir(TEST_ROOT) != 0)
        return 1;

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    auth_thread_t auth;
    fake_session(&auth.session, sockets[1]);
    auth.result = AUTH_DROP;
    pthread_t thread;
    if (pthread_create(&thread, NULL, run_auth, &auth) != 0)
        return 1;

    int account = shared ? 0 : ordinal;
    char expected[MAX_USER_NAME];
    char body[64];
    snprintf(expected, sizeof(expected), "herd%03d", account);
    snprintf(body, sizeof(body), "%s\nrace-password", expected);
    session_t client;
    fake_session(&client, sockets[0]);
    int status = 500;
    for (int attempt = 0; attempt < REGISTER_ATTEMPTS; attempt++)
    {
        if (put(&client, "LOGIN", "/auth/login", body) != 0)
            break;
        status = get_status(&client);
        if (status != 500)
            break;
    }
    close(sockets[0]);
    (void)pthread_join(thread, NULL);
    close(sockets[1]);

    char actual[MAX_USER_NAME];
    auth_get_name(actual, sizeof(actual));
    return status == 200 && auth.result == AUTH_OK &&
                   strcmp(actual, expected) == 0
               ? 0
               : 1;
}

static int test_login_race(int shared)
{
    int barrier[2];
    CHECK(pipe(barrier) == 0, "create login barrier");
    pid_t workers[RACERS];
    last_phase = shared ? "shared-account login workers"
                        : "distinct-account login workers";
    for (int i = 0; i < RACERS; i++)
    {
        workers[i] = fork();
        if (workers[i] < 0)
        {
            close(barrier[0]);
            close(barrier[1]);
            abort_workers(workers, i);
            CHECK(0, "fork login worker");
        }
        if (workers[i] == 0)
        {
            close(barrier[1]);
            _exit(login_worker(worker_order(i), barrier[0], shared));
        }
    }
    close(barrier[0]);
    CHECK(release_workers(barrier[1]) == 0, "release login workers");
    return reap_workers(workers, shared ? "shared-login" : "distinct-login");
}

static int test_distinct_login_race(void)
{
    return test_login_race(0);
}

static int test_shared_login_race(void)
{
    return test_login_race(1);
}

static int compare_ll(const void *left, const void *right)
{
    long long a = *(const long long *)left;
    long long b = *(const long long *)right;
    return (a > b) - (a < b);
}

static int registration_ids(long long *ids, int capacity)
{
    char body[65536];
    CHECK(query("select id, name from " TETRISAUTH_DB_TABLE ";", body,
                sizeof body) == 0,
          "read registrations after race");
    int rows = db_row_count(body);
    CHECK(rows >= 0, "registration query must carry a table");
    int found = 0;
    for (int row = 0; row < rows; row++)
    {
        const char *field[2];
        size_t len[2];
        CHECK(db_row_fields(body, row, field, len, 2) == 2,
              "registration row must carry id and name");
        if (len[1] != 7 || memcmp(field[1], "herd", 4) != 0)
            continue;
        CHECK(found < capacity, "registration race wrote too many rows");
        char text[32];
        CHECK(len[0] < sizeof text, "registration id is too wide");
        memcpy(text, field[0], len[0]);
        text[len[0]] = '\0';
        ids[found++] = strtoll(text, NULL, 10);
    }
    return found;
}

static int test_registration_race(void)
{
    int barrier[2];
    CHECK(pipe(barrier) == 0, "create registration barrier");
    pid_t workers[RACERS];
    last_phase = "registration workers";
    for (int i = 0; i < RACERS; i++)
    {
        workers[i] = fork();
        if (workers[i] < 0)
        {
            close(barrier[0]);
            close(barrier[1]);
            abort_workers(workers, i);
            CHECK(0, "fork registration worker");
        }
        if (workers[i] == 0)
        {
            close(barrier[1]);
            _exit(register_worker(worker_order(i), barrier[0]));
        }
    }
    close(barrier[0]);
    CHECK(release_workers(barrier[1]) == 0, "release registration workers");
    CHECK(reap_workers(workers, "registration") == 0,
          "registration workers must finish the full auth gate");

    long long ids[RACERS];
    int inserted = registration_ids(ids, RACERS);
    CHECK(inserted == RACERS,
          "every distinct login account must commit before login races");
    qsort(ids, (size_t)inserted, sizeof ids[0], compare_ll);
    for (int i = 0; i < inserted; i++)
        CHECK(ids[i] == ids[0] + i,
              "committed registration ids must be unique and gap-free");
    return 0;
}

static int history_worker(int ordinal, int barrier_fd)
{
    char released;
    if (read(barrier_fd, &released, 1) != 1)
        return 1;
    close(barrier_fd);
    silence_worker_stderr();
    char name[MAX_USER_NAME];
    snprintf(name, sizeof name, "history%03d", ordinal);
    history_db_insert(900000 + ordinal, name, 1000 + ordinal, ordinal,
                      6000 + ordinal, 7000 + ordinal);
    return 0;
}

static int test_history_race(void)
{
    int barrier[2];
    CHECK(pipe(barrier) == 0, "create history barrier");
    pid_t workers[RACERS];
    last_phase = "history workers";
    for (int i = 0; i < RACERS; i++)
    {
        workers[i] = fork();
        if (workers[i] < 0)
        {
            close(barrier[0]);
            close(barrier[1]);
            abort_workers(workers, i);
            CHECK(0, "fork history worker");
        }
        if (workers[i] == 0)
        {
            close(barrier[1]);
            _exit(history_worker(worker_order(i), barrier[0]));
        }
    }
    close(barrier[0]);
    CHECK(release_workers(barrier[1]) == 0, "release history workers");
    CHECK(reap_workers(workers, "history") == 0,
          "history workers must return, including allowed failures");

    char body[65536];
    CHECK(query("select id from " HISTORY_DB_TABLE " order by id asc;", body,
                sizeof body) == 0,
          "read history ids after race");
    int rows = db_row_count(body);
    CHECK(rows > 0 && rows <= RACERS,
          "history row count must include only successful inserts");
    for (int row = 0; row < rows; row++)
    {
        const char *field[1];
        size_t len[1];
        CHECK(db_row_fields(body, row, field, len, 1) == 1,
              "history row must carry one id");
        char text[32];
        CHECK(len[0] < sizeof text, "history id is too wide");
        memcpy(text, field[0], len[0]);
        text[len[0]] = '\0';
        CHECK(strtoll(text, NULL, 10) == 1 + row,
              "committed history ids must be unique and gap-free");
    }
    return 0;
}

static int test_runner_restart_and_shutdown(void)
{
    last_phase = "runner restart";
    CHECK(restart_fixture_runner() == 0, "runner restart failed");
    char body[1024];
    CHECK(query("select id from " TETRISAUTH_DB_TABLE ";", body,
                sizeof(body)) == 0,
          "restarted runner did not serve login table");

    int barrier[2];
    CHECK(pipe(barrier) == 0, "create shutdown barrier");
    pid_t workers[RACERS];
    last_phase = "runner shutdown history workers";
    for (int i = 0; i < RACERS; i++)
    {
        workers[i] = fork();
        if (workers[i] < 0)
        {
            close(barrier[0]);
            close(barrier[1]);
            abort_workers(workers, i);
            CHECK(0, "fork shutdown history worker");
        }
        if (workers[i] == 0)
        {
            close(barrier[1]);
            _exit(history_worker(worker_order(i), barrier[0]));
        }
    }
    close(barrier[0]);
    CHECK(release_workers(barrier[1]) == 0, "release shutdown workers");
    nap_ms(10);
    CHECK(kill(runner, SIGTERM) == 0, "stop runner during history race");
    int status = 0;
    long deadline = monotonic_ms() + WORKER_TIMEOUT_MS;
    pid_t done = 0;
    while ((done = waitpid(runner, &status, WNOHANG)) == 0 &&
           monotonic_ms() < deadline)
        nap_ms(10);
    CHECK(done == runner, "runner did not stop during history race");
    runner = -1;
    CHECK(reap_workers(workers, "shutdown-history") == 0,
          "history workers deadlocked during runner shutdown");

    sem_t *sem = sem_open(HISTORY_SEM_NAME, O_CREAT, 0600, 1);
    CHECK(sem != SEM_FAILED, "open history semaphore after shutdown");
    int acquired = sem_trywait(sem);
    if (acquired == 0)
        sem_post(sem);
    sem_close(sem);
    CHECK(acquired == 0, "runner shutdown leaked history semaphore ownership");
    return 0;
}

int main(void)
{
    test_output_begin("race conditions");
    if (load_seed() != 0)
    {
        test_output_failure_detail(
            "TETRISH_RACE_SEED must be an unsigned integer", __FILE__,
            __LINE__);
        tests_run = 5;
        tests_failed = 5;
        test_output_fail("registration race");
        test_output_fail("distinct-account login race");
        test_output_fail("shared-account login race");
        test_output_fail("history race");
        test_output_fail("runner restart and shutdown race");
    }
    else if (runner_disabled() || access(TEST_JAR, R_OK) != 0 || !have_java())
    {
        const char *reason = runner_disabled() ? "TETRISH_NO_RUNNER is set"
                             : access(TEST_JAR, R_OK) != 0
                                 ? "no db/dist/simpledb.jar"
                                 : "java did not run";
        if (!runner_disabled() && getenv("TETRISH_REQUIRE_RUNNER") != NULL)
        {
            test_output_failure_detail(
                "TETRISH_REQUIRE_RUNNER is set and the runner is unavailable",
                __FILE__, __LINE__);
            tests_run = 5;
            tests_failed = 5;
            test_output_fail("registration race");
            test_output_fail("distinct-account login race");
            test_output_fail("shared-account login race");
            test_output_fail("history race");
            test_output_fail("runner restart and shutdown race");
        }
        else
        {
            test_output_skip("registration race", reason);
            test_output_skip("distinct-account login race", reason);
            test_output_skip("shared-account login race", reason);
            test_output_skip("history race", reason);
            test_output_skip("runner restart and shutdown race", reason);
            tests_skipped = 5;
        }
    }
    else if (setup_fixture() != 0)
    {
        tests_run = 5;
        tests_failed = 5;
        test_output_fail("registration race");
        test_output_fail("distinct-account login race");
        test_output_fail("shared-account login race");
        test_output_fail("history race");
        test_output_fail("runner restart and shutdown race");
    }
    else
    {
        printf("RACE seed=%lu workers=%d\n", race_seed, RACERS);
        run("254 registrations with three immediate attempts",
            test_registration_race);
        run("254 distinct-account logins retain their identity",
            test_distinct_login_race);
        run("254 shared-account logins retain their identity",
            test_shared_login_race);
        run("254 best-effort history inserts", test_history_race);
        run("runner restart and shutdown release all workers",
            test_runner_restart_and_shutdown);
    }
    stop_fixture();
    test_output_summary(tests_run + tests_skipped, tests_failed, tests_skipped);
    return tests_failed == 0 ? 0 : 1;
}
