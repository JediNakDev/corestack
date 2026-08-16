/*
 * Path-complete system tests for README UC-1 through UC-8.
 *
 * This executable is intentionally strict. It runs the public libballotclient
 * API against real ballotd, ballot_session, tetrisauth, tetriSH, and SimpleDB
 * processes. Missing runtime dependencies are failures, never skips.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ftw.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#include "libballotbrain/db.h"
#include "libballotclient/voter.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "tetrisdb/runner.h"

#define BALLOTD_BIN "bin/ballotd"
#define BALLOT_SESSION_BIN "bin/ballot_session"
#define CA_PATH "auth/cacsertificate.crt"
#define CTL_PATH "var/run/test_system_e2e.ctl"
#define LOG_PATH "var/run/test_system_e2e.log"
#define HOST "127.0.0.1"
#define TEST_PASSWORD "correcthorsebatterystaple"

static pid_t g_ballotd = -1;
static char g_repo_root[PATH_MAX];
static char g_fixture[PATH_MAX];
static char g_reg_sem[64];
static int g_port;
static int g_runner_started;
static int g_run;
static int g_failed;
static const char *g_current_case;

static int start_ballotd(void);

#define REQUIRE(cond, fmt, ...)                                                \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "\n    case: %s\n    action: %s\n    expected: " fmt  \
                      "\n    actual: assertion was false\n",                    \
              g_current_case, #cond, ##__VA_ARGS__);                           \
      return -1;                                                               \
    }                                                                          \
  } while (0)

#define REQUIRE_EQ(actual_expr, expected_expr, fmt, ...)                       \
  do {                                                                         \
    long long actual_value = (long long)(actual_expr);                         \
    long long expected_value = (long long)(expected_expr);                     \
    if (actual_value != expected_value) {                                      \
      fprintf(stderr,                                                          \
              "\n    case: %s\n    action: %s\n    expected: %lld; " fmt       \
              "\n    actual: %lld\n",                                           \
              g_current_case, #actual_expr, expected_value, ##__VA_ARGS__,     \
              actual_value);                                                   \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static void nap(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static int wait_for_tcp(void) {
  for (int i = 0; i < 300; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof addr);
      addr.sin_family = AF_INET;
      addr.sin_port = htons((uint16_t)g_port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        close(fd);
        return 0;
      }
      close(fd);
    }
    nap(10);
  }
  return -1;
}

static int command_available(const char *command, const char *arg) {
  pid_t pid = fork();
  if (pid < 0) return 0;
  if (pid == 0) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    execlp(command, command, arg, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  for (int i = 0; i < 500; i++) {
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (done < 0 && errno != EINTR) return 0;
    nap(10);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int run_tetrisdb(const char *verb) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); }
    execl("bin/tetrisdb", "bin/tetrisdb", verb, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  for (int i = 0; i < 1500; i++) {
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    if (done < 0 && errno != EINTR) return -1;
    nap(10);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
  return -1;
}

static int start_runner(void) {
  if (db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_ELECTION, BB_DB_SCHEMA_ELECTION) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_ELIGIBLE, BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) != 0)
    return -1;
  if (run_tetrisdb("start") != 0) return -1;
  db_socket_opts_t opts;
  db_socket_opts_load(&opts);
  for (int i = 0; i < 100; i++) {
    db_socket_t *probe = db_socket_open(&opts);
    if (probe != NULL) {
      db_socket_close(probe);
      return 0;
    }
    nap(100);
  }
  return -1;
}

static int write_text(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  size_t left = strlen(text);
  const char *p = text;
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); return -1; }
    p += n;
    left -= (size_t)n;
  }
  return close(fd);
}

static int copy_file(const char *source, const char *destination, mode_t mode) {
  int in = open(source, O_RDONLY);
  int out = open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (in < 0 || out < 0) { if (in >= 0) close(in); if (out >= 0) close(out); return -1; }
  char buf[4096];
  ssize_t n;
  while ((n = read(in, buf, sizeof buf)) > 0) {
    ssize_t written = 0;
    while (written < n) {
      ssize_t part = write(out, buf + written, (size_t)(n - written));
      if (part < 0 && errno == EINTR) continue;
      if (part <= 0) { close(in); close(out); return -1; }
      written += part;
    }
  }
  int rc = n < 0 || close(in) != 0 || close(out) != 0 ? -1 : 0;
  return rc;
}

static int allocate_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) { close(fd); return -1; }
  socklen_t len = sizeof addr;
  if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) { close(fd); return -1; }
  g_port = ntohs(addr.sin_port);
  close(fd);
  return 0;
}

static int fixture_start(void) {
  char template_path[] = "/tmp/ballotbox-e2e-XXXXXX";
  char *made = mkdtemp(template_path);
  if (made == NULL) return -1;
  snprintf(g_fixture, sizeof g_fixture, "%s", made);
  char path[PATH_MAX], target[PATH_MAX], rc[PATH_MAX * 2];
  snprintf(path, sizeof path, "%s/auth", g_fixture);
  if (mkdir(path, 0700) != 0) return -1;
  snprintf(path, sizeof path, "%s/var", g_fixture);
  if (mkdir(path, 0700) != 0) return -1;
  snprintf(path, sizeof path, "%s/var/run", g_fixture);
  if (mkdir(path, 0700) != 0) return -1;
  snprintf(path, sizeof path, "%s/var/log", g_fixture);
  if (mkdir(path, 0700) != 0) return -1;
  snprintf(path, sizeof path, "%s/bin", g_fixture);
  snprintf(target, sizeof target, "%s/bin", g_repo_root);
  if (symlink(target, path) != 0) return -1;
  const char *auth_files[] = {"cacsertificate.crt", "server_signed.crt", "private_key.pem"};
  for (size_t i = 0; i < sizeof auth_files / sizeof auth_files[0]; i++) {
    snprintf(path, sizeof path, "%s/auth/%s", g_fixture, auth_files[i]);
    snprintf(target, sizeof target, "%s/auth/%s", g_repo_root, auth_files[i]);
    if (copy_file(target, path, 0600) != 0) return -1;
  }
  snprintf(rc, sizeof rc,
           "db = true\n"
           "db_dir = var/db\n"
           "db_ipc = var/run/tetrisdb.sock\n"
           "db_jar = %s/db/dist/simpledb.jar\n"
           "db_err_path = var/log/tetrisdb.err\n"
           "db_timeout = 2000\n"
           "auth_pbkdf2_iters = 1\n", g_repo_root);
  snprintf(path, sizeof path, "%s/.tetrishrc", g_fixture);
  snprintf(g_reg_sem, sizeof g_reg_sem, "/ballotbox_e2e_%ld_%d", (long)getpid(), g_run);
  if (setenv("TETRISH_REG_SEM", g_reg_sem, 1) != 0 || write_text(path, rc) != 0 ||
      allocate_port() != 0 || chdir(g_fixture) != 0) return -1;
  if (start_runner() != 0) return -1;
  g_runner_started = 1;
  if (start_ballotd() != 0) return -1;
  return 0;
}

static int start_ballotd(void) {
  unlink(CTL_PATH);
  unlink(LOG_PATH);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log_fd < 0 || dup2(log_fd, STDERR_FILENO) < 0) _exit(126);
    close(log_fd);
    char port[16];
    snprintf(port, sizeof port, "%d", g_port);
    execl(BALLOTD_BIN, BALLOTD_BIN, "-p", port, "-C", CTL_PATH, "-d", DB_DEFAULT_DIR, "-i",
          DB_DEFAULT_IPC, (char *)NULL);
    _exit(127);
  }
  g_ballotd = pid;
  return wait_for_tcp();
}

static int stop_ballotd(void) {
  int stopped = 1;
  if (g_ballotd > 0) {
    kill(g_ballotd, SIGTERM);
    int reaped = 0;
    for (int i = 0; i < 500; i++) {
      pid_t done = waitpid(g_ballotd, NULL, WNOHANG);
      if (done == g_ballotd || (done < 0 && errno == ECHILD)) { reaped = 1; break; }
      nap(10);
    }
    if (!reaped) {
      stopped = 0;
      kill(g_ballotd, SIGKILL);
      while (waitpid(g_ballotd, NULL, 0) < 0 && errno == EINTR);
    }
    g_ballotd = -1;
  }
  unlink(CTL_PATH);
  return stopped ? 0 : -1;
}

static int cleanup(void) {
  int ok = 1;
  if (stop_ballotd() != 0) ok = 0;
  if (g_runner_started && run_tetrisdb("stop") != 0) ok = 0;
  g_runner_started = 0;
  if (unlink(LOG_PATH) != 0 && errno != ENOENT) ok = 0;
  return ok ? 0 : -1;
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw) {
  (void)st;
  (void)type;
  (void)ftw;
  return remove(path);
}

static int fixture_stop(void) {
  int ok = cleanup() == 0;
  if (g_reg_sem[0] != '\0') {
    if (sem_unlink(g_reg_sem) != 0 && errno != ENOENT) ok = 0;
    g_reg_sem[0] = '\0';
    unsetenv("TETRISH_REG_SEM");
  }
  if (chdir(g_repo_root) != 0) return -1;
  if (g_fixture[0] != '\0') {
    if (nftw(g_fixture, remove_entry, 32, FTW_DEPTH | FTW_PHYS) != 0) ok = 0;
    g_fixture[0] = '\0';
  }
  return ok ? 0 : -1;
}

static bcl_ctx *admin_client(void) {
  bcl_ctx *ctx = bcl_create();
  if (ctx != NULL) bcl_set_ctl_path(ctx, CTL_PATH);
  return ctx;
}

static bcl_request_t create_request(const char *title, int option_count, const char *eligible) {
  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  snprintf(req.config.title, BB_TITLE_LEN, "%s", title);
  if (option_count > 0) snprintf(req.config.options[0], BB_OPTION_LEN, "Yes");
  if (option_count > 1) snprintf(req.config.options[1], BB_OPTION_LEN, "No");
  req.config.option_count = option_count;
  if (eligible != NULL) {
    snprintf(req.config.eligible[0], BB_CERT_LEN, "%s", eligible);
    req.config.eligible_count = 1;
  }
  snprintf(req.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(req.config.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");
  return req;
}

static bb_result_t send_admin(bcl_ctx *ctx, bcl_op_t op, const char *id,
                              bcl_response_t *response) {
  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = op;
  if (id != NULL) snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  memset(response, 0, sizeof *response);
  return bcl_send(ctx, &req, response);
}

static int create_election(bcl_ctx *admin, const char *title, const char *eligible,
                           char id[BB_ID_LEN]) {
  bcl_request_t req = create_request(title, 2, eligible);
  bcl_response_t resp;
  memset(&resp, 0, sizeof resp);
  if (bcl_send(admin, &req, &resp) != BB_OK) return -1;
  snprintf(id, BB_ID_LEN, "%s", resp.election.id);
  return id[0] == '\0' ? -1 : 0;
}

static bcl_ctx *voter_client(const char *username) {
  bcl_ctx *ctx = bcl_create();
  if (ctx == NULL) return NULL;
  if (bcl_connect(ctx, HOST, g_port, CA_PATH) != BB_OK) goto fail;
  int status = 0;
  if (bcl_auth(ctx, "LOGIN", username, TEST_PASSWORD, &status) == 0 && status == 200) return ctx;
  if (status == 404 && bcl_auth(ctx, "REGISTER", username, TEST_PASSWORD, &status) == 0 &&
      status == 200)
    return ctx;
fail:
  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  return NULL;
}

static void close_voter(bcl_ctx *ctx) {
  if (ctx == NULL) return;
  bcl_disconnect(ctx);
  bcl_destroy(ctx);
}

static bb_result_t voter_request(bcl_ctx *ctx, bcl_op_t op, const char *id, const char *user,
                                 const char *hash, bcl_response_t *resp) {
  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = op;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  if (user != NULL) snprintf(req.cert_name, BB_CERT_LEN, "%s", user);
  if (hash != NULL) snprintf(req.hash, BB_HASH_LEN, "%s", hash);
  memset(resp, 0, sizeof *resp);
  return bcl_send(ctx, &req, resp);
}

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int encrypted;
  int release;
  int done;
} submit_barrier_t;

typedef struct {
  bcl_ctx *ctx;
  bu_session_t *session;
  submit_barrier_t *barrier;
  int option;
  const char *nonce;
  bb_result_t result;
} submit_job_t;

static void pause_before_send(void *arg) {
  submit_barrier_t *barrier = arg;
  pthread_mutex_lock(&barrier->mutex);
  barrier->encrypted = 1;
  pthread_cond_broadcast(&barrier->cond);
  while (!barrier->release) pthread_cond_wait(&barrier->cond, &barrier->mutex);
  pthread_mutex_unlock(&barrier->mutex);
}

static void *submit_in_thread(void *arg) {
  submit_job_t *job = arg;
  job->result = bu_submit_vote(job->ctx, job->session, job->option, job->nonce, NULL);
  submit_barrier_t *barrier = job->barrier;
  pthread_mutex_lock(&barrier->mutex);
  barrier->done = 1;
  pthread_cond_broadcast(&barrier->cond);
  pthread_mutex_unlock(&barrier->mutex);
  return NULL;
}

static int wait_for_barrier_flag(submit_barrier_t *barrier, int *flag) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 5;
  pthread_mutex_lock(&barrier->mutex);
  int rc = 0;
  while (!*flag && rc == 0)
    rc = pthread_cond_timedwait(&barrier->cond, &barrier->mutex, &deadline);
  pthread_mutex_unlock(&barrier->mutex);
  return *flag ? 0 : -1;
}

static void release_submission(submit_barrier_t *barrier) {
  pthread_mutex_lock(&barrier->mutex);
  barrier->release = 1;
  pthread_cond_broadcast(&barrier->cond);
  pthread_mutex_unlock(&barrier->mutex);
}

static int close_during_encrypted_submission(bcl_ctx *admin, bcl_ctx *voter,
                                             bu_session_t *session, const char *id,
                                             int option, const char *nonce) {
  submit_barrier_t barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0};
  submit_job_t job = {voter, session, &barrier, option, nonce, BB_OK};
  pthread_t thread;
  bu_set_before_submit(voter, pause_before_send, &barrier);
  if (pthread_create(&thread, NULL, submit_in_thread, &job) != 0) return -1;
  if (wait_for_barrier_flag(&barrier, &barrier.encrypted) != 0) {
    release_submission(&barrier);
    pthread_join(thread, NULL);
    return -1;
  }
  bcl_response_t resp;
  bb_result_t close_result = send_admin(admin, BCL_CLOSE, id, &resp);
  release_submission(&barrier);
  int completed = wait_for_barrier_flag(&barrier, &barrier.done);
  if (completed != 0) pthread_cancel(thread);
  pthread_join(thread, NULL);
  bu_set_before_submit(voter, NULL, NULL);
  pthread_cond_destroy(&barrier.cond);
  pthread_mutex_destroy(&barrier.mutex);
  return completed == 0 && close_result == BB_OK && job.result == BB_ERR_CLOSED ? 0 : -1;
}

static int test_uc1_create_paths(void) {
  bcl_ctx *admin = admin_client();
  REQUIRE(admin != NULL, "admin client");
  bcl_response_t resp;
  char id[BB_ID_LEN];
  if (strcmp(g_current_case, "UC1-MAIN") == 0) {
    REQUIRE(create_election(admin, "UC1 happy", "e2ealice", id) == 0,
            "valid create succeeds");
    REQUIRE(send_admin(admin, BCL_OPEN, id, &resp) == BB_OK, "DRAFT opens to OPEN");
  } else if (strcmp(g_current_case, "UC1-ID-TAKEN") == 0) {
    REQUIRE(create_election(admin, "original", "e2ealice", id) == 0, "original create");
    bcl_request_t duplicate = create_request("duplicate", 2, "e2ealice");
    snprintf(duplicate.election_id, BB_ID_LEN, "%s", id);
    REQUIRE(bcl_send(admin, &duplicate, &resp) == BB_ERR_CONFIG_ID_TAKEN,
            "duplicate requested id is refused");
    REQUIRE(send_admin(admin, BCL_OPEN, id, &resp) == BB_OK,
            "duplicate refusal leaves original DRAFT election unchanged");
  } else {
    bcl_request_t req = create_request("valid title", 2, "e2ealice");
    bb_result_t expected = BB_ERR_CONFIG_TIME;
    if (strcmp(g_current_case, "UC1-2A-TITLE") == 0) {
      req.config.title[0] = '\0';
      expected = BB_ERR_CONFIG_TITLE;
    } else if (strncmp(g_current_case, "UC1-2A-OPTIONS", 14) == 0) {
      req.config.option_count = strstr(g_current_case, "ZERO") != NULL ? 0 : 1;
      expected = BB_ERR_CONFIG_OPTIONS;
    } else {
      snprintf(req.config.close_time, BB_TIME_LEN, "%s",
               strstr(g_current_case, "REVERSED") != NULL
                   ? "2025-12-31T00:00:00Z"
                   : req.config.open_time);
    }
    REQUIRE_EQ(bcl_send(admin, &req, &resp), expected,
               "invalid configuration returns its specific error");
    REQUIRE(resp.election.id[0] == '\0', "invalid configuration creates no election");
  }
  bcl_destroy(admin);
  return 0;
}

static int prepare_state(bcl_ctx *admin, bb_state_t state, const char *user, char id[BB_ID_LEN]) {
  bcl_response_t resp;
  if (create_election(admin, "state fixture", user, id) != 0) return -1;
  if (state >= BB_STATE_OPEN && send_admin(admin, BCL_OPEN, id, &resp) != BB_OK) return -1;
  if (state >= BB_STATE_CLOSED && send_admin(admin, BCL_CLOSE, id, &resp) != BB_OK) return -1;
  if (state >= BB_STATE_PUBLISHED && send_admin(admin, BCL_PUBLISH, id, &resp) != BB_OK) return -1;
  return 0;
}

static int test_uc2_join_paths(void) {
  bcl_ctx *admin = admin_client();
  REQUIRE(admin != NULL, "admin client");
  if (strcmp(g_current_case, "UC2-2A") == 0) {
    bcl_ctx *down = bcl_create();
    REQUIRE(down != NULL, "unreachable client allocation");
    REQUIRE(bcl_connect(down, HOST, g_port + 1, CA_PATH) == BB_ERR_DB,
            "unreachable ballotd fails within the transport deadline");
    bcl_destroy(down);
    bcl_destroy(admin);
    return 0;
  }
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(alice != NULL, "authenticated voter client");
  char id[BB_ID_LEN];
  bu_session_t session;
  memset(&session, 0, sizeof session);
  if (strcmp(g_current_case, "UC2-MAIN") == 0) {
    REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", id) == 0, "OPEN fixture");
    REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED,
            "eligible OPEN join");
    REQUIRE(session.joined && strcmp(session.title, "state fixture") == 0 &&
                session.option_count == 2,
            "join exposes title and options");
  } else if (strcmp(g_current_case, "UC2-2B") == 0) {
    REQUIRE(bu_join(alice, &session, "E-DOES-NOT-EXIST", "e2ealice") ==
                BU_JOIN_NOT_FOUND,
            "unknown election is not found");
    REQUIRE(!session.joined, "unknown election creates no joined session");
  } else if (strcmp(g_current_case, "UC2-3A") == 0) {
    REQUIRE(prepare_state(admin, BB_STATE_OPEN, "somebodyelse", id) == 0,
            "ineligible fixture");
    REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_NOT_ELIGIBLE,
            "unlisted authenticated voter is refused");
    REQUIRE(!session.joined, "ineligible refusal returns no session");
  } else {
    bb_state_t state = strstr(g_current_case, "DRAFT") != NULL ? BB_STATE_DRAFT
                       : strstr(g_current_case, "PUBLISHED") != NULL ? BB_STATE_PUBLISHED
                                                                     : BB_STATE_CLOSED;
    REQUIRE(prepare_state(admin, state, "e2ealice", id) == 0,
            "non-open fixture");
    REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_NOT_OPEN,
            "non-open election is remembered but not joined");
    REQUIRE(!session.joined && strcmp(session.election_id, id) == 0,
            "not-open outcome saves the election for later");
  }
  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static int test_uc3_uc4_vote_paths(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(admin != NULL && alice != NULL, "clients");
  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", id) == 0, "OPEN fixture");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  bb_receipt_t first, second;
  memset(&first, 0, sizeof first);
  memset(&second, 0, sizeof second);
  if (strcmp(g_current_case, "UC3-1A") == 0 || strcmp(g_current_case, "UC4-1A") == 0) {
    REQUIRE(bu_submit_vote(alice, &session, 0, "not-joined", NULL) == BB_ERR_NOT_JOINED,
            "not-joined submission is refused locally");
  } else {
    REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED, "join");
    if (strcmp(g_current_case, "UC4-1B") == 0) {
      REQUIRE(bu_route_vote(&session) == BU_CAST, "no prior ballot routes to cast");
    } else if (strcmp(g_current_case, "UC3-4A") == 0) {
      bcl_response_t resp;
      REQUIRE(close_during_encrypted_submission(admin, alice, &session, id, 0,
                                                "uc3-after-close") == 0,
              "cast pauses after encryption, closes, then is rejected");
      REQUIRE(!session.has_ballot, "rejected cast preserves client state");
      REQUIRE(send_admin(admin, BCL_PUBLISH, id, &resp) == BB_OK, "publish race fixture");
      REQUIRE(send_admin(admin, BCL_ADMIN_RESULTS, id, &resp) == BB_OK &&
                  resp.hash_count == 0 && resp.tally[0] == 0 && resp.tally[1] == 0,
              "rejected close-race cast writes no ballot");
    } else {
      REQUIRE(bu_submit_vote(alice, &session, 0, "first-cast", &first) == BB_OK,
              "valid cast returns receipt");
      REQUIRE(first.hash[0] != '\0' && session.ballot_version == 1,
              "cast persists version one");
      if (strcmp(g_current_case, "UC3-MAIN") == 0) {
        REQUIRE(bu_route_vote(&session) == BU_UPDATE,
                "cast receipt routes the next vote to update");
      } else if (strcmp(g_current_case, "UC3-1B") == 0) {
        REQUIRE(bu_route_vote(&session) == BU_UPDATE, "prior ballot routes to UC-4");
      } else if (strcmp(g_current_case, "UC4-4A") == 0) {
        bcl_response_t resp;
        REQUIRE(close_during_encrypted_submission(admin, alice, &session, id, 1,
                                                  "uc4-after-close") == 0,
                "update pauses after encryption, closes, then is rejected");
        REQUIRE(session.ballot_version == 1 && strcmp(session.my_hash, first.hash) == 0,
                "rejected update preserves the prior client receipt");
        REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", first.hash, &resp) == BB_OK &&
                    resp.found_option == 0,
                "rejected update leaves the prior stored ballot live");
      } else if (strcmp(g_current_case, "UC4-RECONNECT") == 0) {
        bcl_disconnect(alice);
        bcl_destroy(alice);
        alice = voter_client("e2ealice");
        REQUIRE(alice != NULL, "reconnected voter");
        memset(&session, 0, sizeof session);
        REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED,
                "returning voter rejoins");
        REQUIRE(session.has_ballot && session.ballot_version == 1 &&
                    bu_route_vote(&session) == BU_UPDATE,
                "rejoin recovers authoritative prior-ballot version");
      } else {
        REQUIRE(bu_submit_vote(alice, &session, 1, "valid-update", &second) == BB_OK,
                "valid update returns receipt");
        REQUIRE(strcmp(first.hash, second.hash) != 0 && session.ballot_version == 2,
                "update produces a fresh version-two receipt");
      }
    }
  }

  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static int log_has_voter_choice_link(const char *voter) {
  FILE *log = fopen(LOG_PATH, "r");
  if (log == NULL) return -1;
  char line[1024];
  int linked = 0;
  while (fgets(line, sizeof line, log) != NULL) {
    if (strstr(line, voter) != NULL && strstr(line, "option=") != NULL) {
      linked = 1;
      break;
    }
  }
  fclose(log);
  return linked;
}

static int log_contains(const char *needle) {
  FILE *log = fopen(LOG_PATH, "r");
  if (log == NULL) return -1;
  char line[1024];
  int found = 0;
  while (fgets(line, sizeof line, log) != NULL) {
    if (strstr(line, needle) != NULL) { found = 1; break; }
  }
  fclose(log);
  return found;
}

static int test_uc3_log_secrecy(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *voter = voter_client("secrecyvoter");
  REQUIRE(admin != NULL && voter != NULL, "clients");
  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "secrecyvoter", id) == 0, "OPEN fixture");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(voter, &session, id, "secrecyvoter") == BU_JOIN_ADMITTED, "join");
  bb_receipt_t receipt;
  REQUIRE(bu_submit_vote(voter, &session, 1, "secrecy-system-cast", &receipt) == BB_OK,
          "successful system cast");
  REQUIRE(log_contains(receipt.hash) == 1, "successful receipt %s appears in real log",
          receipt.hash);
  REQUIRE(log_has_voter_choice_link("secrecyvoter") == 0,
          "no system log record links voter identity to option");
  close_voter(voter);
  bcl_destroy(admin);
  return 0;
}

static int test_uc5_results_paths(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(admin != NULL && alice != NULL, "clients");
  bcl_response_t resp;
  char id[BB_ID_LEN];
  if (strcmp(g_current_case, "UC5-MAIN") == 0) {
    REQUIRE(prepare_state(admin, BB_STATE_PUBLISHED, "e2ealice", id) == 0,
            "published fixture");
    REQUIRE(voter_request(alice, BCL_RESULTS, id, "e2ealice", NULL, &resp) == BB_OK,
            "eligible observer sees published results");
    REQUIRE(resp.option_count == 2 && strcmp(resp.election.title, "state fixture") == 0,
            "observer sees exact title and option set");
    REQUIRE(send_admin(admin, BCL_ADMIN_RESULTS, id, &resp) == BB_OK,
            "local admin sees results without voter membership");
  } else if (strcmp(g_current_case, "UC5-2A") == 0) {
    REQUIRE(prepare_state(admin, BB_STATE_PUBLISHED, "somebodyelse", id) == 0,
            "restricted published fixture");
    REQUIRE(voter_request(alice, BCL_RESULTS, id, "e2ealice", NULL, &resp) ==
                BB_ERR_NOT_ELIGIBLE,
            "ineligible observer is refused");
    REQUIRE(resp.option_count == 0 && resp.hash_count == 0,
            "ineligible observer receives no tally or hashes");
  } else {
    bb_state_t state = strstr(g_current_case, "DRAFT") != NULL ? BB_STATE_DRAFT
                       : strstr(g_current_case, "OPEN") != NULL ? BB_STATE_OPEN
                                                                : BB_STATE_CLOSED;
    REQUIRE(prepare_state(admin, state, "e2ealice", id) == 0,
            "unpublished fixture");
    REQUIRE(voter_request(alice, BCL_RESULTS, id, "e2ealice", NULL, &resp) ==
                BB_ERR_NOT_PUBLISHED,
            "results are unavailable before publication");
    REQUIRE(resp.option_count == 0 && resp.hash_count == 0,
            "unavailable results expose no tally or hashes");
  }
  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static int test_uc6_check_paths(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(admin != NULL && alice != NULL, "clients");
  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", id) == 0, "OPEN fixture");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED, "join");
  bb_receipt_t old_receipt, latest_receipt;
  REQUIRE(bu_submit_vote(alice, &session, 0, "uc6-old", &old_receipt) == BB_OK, "first vote");
  bcl_response_t resp;
  if (strcmp(g_current_case, "UC6-MAIN") == 0) {
    REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", old_receipt.hash, &resp) == BB_OK &&
                resp.found && resp.found_option == 0,
            "live key verifies before publication with recorded choice");
    REQUIRE(send_admin(admin, BCL_CLOSE, id, &resp) == BB_OK, "close");
    REQUIRE(send_admin(admin, BCL_PUBLISH, id, &resp) == BB_OK, "publish");
    REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", old_receipt.hash, &resp) == BB_OK &&
                resp.found,
            "live key verifies after publication");
  } else {
    REQUIRE(bu_submit_vote(alice, &session, 1, "uc6-latest", &latest_receipt) == BB_OK,
            "update");
    REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", old_receipt.hash, &resp) ==
                BB_ERR_NOT_FOUND && !resp.found,
            "superseded key is reported as dropped");
    REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice",
                          "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                          &resp) == BB_ERR_NOT_FOUND && !resp.found,
            "unknown key is reported as dropped");
  }
  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static bcl_op_t transition_op(bb_state_t state) {
  if (state == BB_STATE_OPEN) return BCL_OPEN;
  if (state == BB_STATE_CLOSED) return BCL_CLOSE;
  return BCL_PUBLISH;
}

static int test_lifecycle_transition(bb_state_t from, bb_state_t to) {
  bcl_ctx *admin = admin_client();
  REQUIRE(admin != NULL, "admin client");
  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, from, "e2ealice", id) == 0, "source state %d", from);
  bcl_response_t resp;
  bb_result_t expected = (to == from + 1) ? BB_OK : BB_ERR_ILLEGAL_TRANSITION;
  REQUIRE_EQ(send_admin(admin, transition_op(to), id, &resp), expected,
             "transition %d -> %d", from, to);
  if (expected == BB_OK) {
    bcl_destroy(admin);
    REQUIRE(stop_ballotd() == 0, "daemon stops within deadline");
    REQUIRE(start_ballotd() == 0, "daemon reconnect after legal transition %d -> %d", from,
            to);
    admin = admin_client();
    REQUIRE(admin != NULL, "client reconnect after legal transition %d -> %d", from, to);
    REQUIRE_EQ(send_admin(admin, transition_op(to), id, &resp), BB_ERR_ILLEGAL_TRANSITION,
               "persisted state rejects repeated transition %d -> %d", from, to);
  } else if (from < BB_STATE_PUBLISHED) {
    REQUIRE_EQ(send_admin(admin, transition_op((bb_state_t)(from + 1)), id, &resp), BB_OK,
               "illegal transition %d -> %d leaves state unchanged", from, to);
  } else {
    REQUIRE_EQ(send_admin(admin, BCL_ADMIN_RESULTS, id, &resp), BB_OK,
               "published terminal state remains readable after rejected transition");
  }
  bcl_destroy(admin);
  return 0;
}

#define TRANSITION_CASE(name, from, to) \
  static int name(void) { return test_lifecycle_transition(from, to); }

TRANSITION_CASE(test_draft_open, BB_STATE_DRAFT, BB_STATE_OPEN)
TRANSITION_CASE(test_draft_close, BB_STATE_DRAFT, BB_STATE_CLOSED)
TRANSITION_CASE(test_draft_publish, BB_STATE_DRAFT, BB_STATE_PUBLISHED)
TRANSITION_CASE(test_open_open, BB_STATE_OPEN, BB_STATE_OPEN)
TRANSITION_CASE(test_open_close, BB_STATE_OPEN, BB_STATE_CLOSED)
TRANSITION_CASE(test_open_publish, BB_STATE_OPEN, BB_STATE_PUBLISHED)
TRANSITION_CASE(test_closed_open, BB_STATE_CLOSED, BB_STATE_OPEN)
TRANSITION_CASE(test_closed_close, BB_STATE_CLOSED, BB_STATE_CLOSED)
TRANSITION_CASE(test_closed_publish, BB_STATE_CLOSED, BB_STATE_PUBLISHED)
TRANSITION_CASE(test_published_open, BB_STATE_PUBLISHED, BB_STATE_OPEN)
TRANSITION_CASE(test_published_close, BB_STATE_PUBLISHED, BB_STATE_CLOSED)
TRANSITION_CASE(test_published_publish, BB_STATE_PUBLISHED, BB_STATE_PUBLISHED)

static int test_complete_election_journey(void) {
  bcl_ctx *admin = admin_client();
  REQUIRE(admin != NULL, "admin client");
  char id[BB_ID_LEN];
  REQUIRE(create_election(admin, "Complete journey", "journeyvoter", id) == 0, "create DRAFT");
  bcl_response_t resp;
  REQUIRE(send_admin(admin, BCL_OPEN, id, &resp) == BB_OK, "open");
  bcl_ctx *voter = voter_client("journeyvoter");
  REQUIRE(voter != NULL, "authenticated voter");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(voter, &session, id, "journeyvoter") == BU_JOIN_ADMITTED, "join");
  bb_receipt_t first, second;
  REQUIRE(bu_submit_vote(voter, &session, 0, "journey-first", &first) == BB_OK, "cast");
  close_voter(voter);

  voter = voter_client("journeyvoter");
  REQUIRE(voter != NULL, "reconnect and authenticate");
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(voter, &session, id, "journeyvoter") == BU_JOIN_ADMITTED &&
              bu_route_vote(&session) == BU_UPDATE,
          "rejoin recovers prior ballot and routes update");
  REQUIRE(bu_submit_vote(voter, &session, 1, "journey-second", &second) == BB_OK, "update");
  REQUIRE(strcmp(first.hash, second.hash) != 0, "receipts differ");
  REQUIRE(send_admin(admin, BCL_CLOSE, id, &resp) == BB_OK, "close");
  REQUIRE(send_admin(admin, BCL_PUBLISH, id, &resp) == BB_OK, "publish");
  REQUIRE(voter_request(voter, BCL_RESULTS, id, "journeyvoter", NULL, &resp) == BB_OK,
          "view published results");
  REQUIRE(resp.tally[0] == 0 && resp.tally[1] == 1 && resp.hash_count == 1,
          "exact tally counts only the latest ballot");
  REQUIRE(strcmp(resp.hashes[0].hash, second.hash) == 0, "published hash is latest receipt");
  REQUIRE(voter_request(voter, BCL_CHECK, id, "journeyvoter", first.hash, &resp) ==
              BB_ERR_NOT_FOUND,
          "first receipt is superseded");
  REQUIRE(voter_request(voter, BCL_CHECK, id, "journeyvoter", second.hash, &resp) == BB_OK &&
              resp.found_option == 1,
          "latest receipt remains live");
  close_voter(voter);
  bcl_destroy(admin);
  return 0;
}

static void run_case(const char *id, const char *name, int (*fn)(void)) {
  g_run++;
  g_current_case = id;
  printf("  %-9s %-58s", id, name);
  fflush(stdout);
  int setup = fixture_start();
  int result = setup == 0 ? fn() : -1;
  int teardown = fixture_stop();
  if (setup != 0)
    fprintf(stderr, "\n    case: %s\n    action: fixture startup\n    expected: isolated services ready\n    actual: startup failed or timed out\n", id);
  if (teardown != 0) {
    fprintf(stderr, "\n    case: %s\n    action: fixture cleanup\n    expected: no process or fixture remains\n    actual: cleanup failed or timed out\n", id);
    result = -1;
  }
  if (result == 0) {
    printf("ok\n");
  } else {
    g_failed++;
    printf("FAILED\n");
  }
}

typedef struct {
  const char *id;
  const char *name;
  int (*fn)(void);
} system_case_t;

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  if (getcwd(g_repo_root, sizeof g_repo_root) == NULL) {
    perror("system E2E: getcwd");
    return 1;
  }
  struct stat st;
  if (stat(BALLOTD_BIN, &st) != 0 || stat(BALLOT_SESSION_BIN, &st) != 0) {
    fprintf(stderr, "system E2E: required ballot executables are not built\n");
    return 1;
  }
  char jar[PATH_MAX];
  snprintf(jar, sizeof jar, "%s/db/dist/simpledb.jar", g_repo_root);
  if (access(jar, R_OK) != 0) {
    fprintf(stderr, "system E2E: required %s is unavailable\n", jar);
    return 1;
  }
  if (!command_available("java", "-version")) {
    fprintf(stderr, "system E2E: java is unavailable\n");
    return 1;
  }
  printf("path-complete system E2E tests\n");
  const system_case_t cases[] = {
      {"UC1-MAIN", "create draft and open it", test_uc1_create_paths},
      {"UC1-2A-TITLE", "reject missing title without a write", test_uc1_create_paths},
      {"UC1-2A-OPTIONS-ZERO", "reject zero options without a write", test_uc1_create_paths},
      {"UC1-2A-OPTIONS-ONE", "reject one option without a write", test_uc1_create_paths},
      {"UC1-2A-TIME-EQUAL", "reject an equal time window without a write", test_uc1_create_paths},
      {"UC1-2A-TIME-REVERSED", "reject a reversed time window without a write", test_uc1_create_paths},
      {"UC1-ID-TAKEN", "reject a duplicate requested identifier", test_uc1_create_paths},
      {"UC2-MAIN", "authenticate and join an eligible open election", test_uc2_join_paths},
      {"UC2-4A-DRAFT", "remember a draft election without joining", test_uc2_join_paths},
      {"UC2-4A-CLOSED", "remember a closed election without joining", test_uc2_join_paths},
      {"UC2-4A-PUBLISHED", "remember a published election without joining", test_uc2_join_paths},
      {"UC2-2A", "bound an unreachable-daemon connection failure", test_uc2_join_paths},
      {"UC2-2B", "reject an unknown election", test_uc2_join_paths},
      {"UC2-3A", "reject an ineligible authenticated voter", test_uc2_join_paths},
      {"UC3-MAIN", "cast an encrypted ballot and receive a receipt", test_uc3_uc4_vote_paths},
      {"UC3-1B", "route a voter with a prior ballot to update", test_uc3_uc4_vote_paths},
      {"UC3-1A", "reject a cast before join without sending", test_uc3_uc4_vote_paths},
      {"UC3-4A", "close after encryption and reject cast without a write", test_uc3_uc4_vote_paths},
      {"UC3-LOG", "log the receipt without linking voter and choice", test_uc3_log_secrecy},
      {"UC4-MAIN", "supersede a ballot with a higher-version receipt", test_uc3_uc4_vote_paths},
      {"UC4-1B", "route a voter without a prior ballot to cast", test_uc3_uc4_vote_paths},
      {"UC4-1A", "reject an update before join without sending", test_uc3_uc4_vote_paths},
      {"UC4-4A", "close after encryption and preserve the prior ballot", test_uc3_uc4_vote_paths},
      {"UC4-RECONNECT", "recover update routing after voter reconnect", test_uc3_uc4_vote_paths},
      {"UC5-MAIN", "return published tally and hashes to observer and admin", test_uc5_results_paths},
      {"UC5-2A", "refuse an ineligible observer without data", test_uc5_results_paths},
      {"UC5-3A-DRAFT", "gate draft results without data", test_uc5_results_paths},
      {"UC5-3A-OPEN", "gate open results without data", test_uc5_results_paths},
      {"UC5-3A-CLOSED", "gate closed results without data", test_uc5_results_paths},
      {"UC6-MAIN", "find a live receipt before and after publication", test_uc6_check_paths},
      {"UC6-4A", "report unknown and superseded receipts as dropped", test_uc6_check_paths},
      {"LC-DRAFT-OPEN", "persist the legal draft-to-open transition", test_draft_open},
      {"UC7-ERR-DRAFT", "reject close from draft without a write", test_draft_close},
      {"UC8-ERR-DRAFT", "reject publish from draft without a write", test_draft_publish},
      {"LC-SELF-OPEN", "reject the open self-transition", test_open_open},
      {"UC7-MAIN", "persist the legal open-to-closed transition", test_open_close},
      {"UC8-ERR-OPEN", "reject publish from open without a write", test_open_publish},
      {"LC-ERR-CLOSED-OPEN", "reject reopening a closed election", test_closed_open},
      {"UC7-SELF-CLOSED", "reject the closed self-transition", test_closed_close},
      {"UC8-MAIN", "persist the legal closed-to-published transition", test_closed_publish},
      {"LC-ERR-PUBLISHED-OPEN", "reject reopening a published election", test_published_open},
      {"UC7-ERR-PUBLISHED", "reject close from published without a write", test_published_close},
      {"UC8-SELF-PUBLISHED", "reject the published self-transition", test_published_publish},
      {"JOURNEY", "complete create-through-verification after reconnects", test_complete_election_journey},
  };
  const char *selected = argc == 2 ? argv[1] : NULL;
  if (argc > 2) {
    fprintf(stderr, "usage: %s [CASE-ID]\n", argv[0]);
    return 2;
  }
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    if (selected == NULL || strcmp(selected, cases[i].id) == 0)
      run_case(cases[i].id, cases[i].name, cases[i].fn);
  }
  if (selected != NULL && g_run == 0) {
    fprintf(stderr, "system E2E: unknown case '%s'\n", selected);
    return 2;
  }

  printf("%d/%d independently isolated system cases passed, 0 skipped\n",
         g_run - g_failed, g_run);
  return g_failed == 0 ? 0 : 1;
}
