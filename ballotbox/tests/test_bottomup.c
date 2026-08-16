/*
 * Bottom-up integration: one cast-vote request, integrated one real
 * component at a time, in the order the presentation deck's "Bottom-up
 * integration" slide lays out. No lower component is re-mocked once a
 * later stage integrates it - each stage either reuses the previous
 * stage's real seam directly, or (stage 3 onward, where the previous
 * stage's sandboxed store cannot be reached from behind ballotd's own
 * auth) drives the exact same real ballotd/SimpleDB pair a manually-run
 * install would use, never a fake.
 *
 *   01 LEAF       SimpleDB adapter   - db_socket_exec/db_quote directly,
 *                                      no libballotbrain, no ballotd.
 *   02 PARENT     ballotd + store    - libballotbrain's real db.c seam
 *                                      (no ballotd process, no wire).
 *   03 TRANSPORT  Secure session     - real tetrissh + the real HTTTP wire
 *                                      codec, hand-built request, no
 *                                      libballotclient session API.
 *   04 TOP        ballotu client     - the public bcl_connect/bu_join/
 *                                      bu_submit_vote path ballotu.c
 *                                      itself calls.
 *
 * Stages 1-2 get their own sandboxed SimpleDB (own directory, own socket,
 * wiped per run) - db_exec has no auth entanglement, so real isolation is
 * free. Stages 3-4 need a real ballotd, and ballotd's auth_login() reads
 * db_ipc/db_dir from .tetrishrc with no override this file can reach (see
 * test_ballotd.c's LIVE_DB_DIR comment for the full reasoning), so they
 * drive the same shared var/db a manually-run installation uses instead of
 * a sandbox - the established convention everywhere else in this suite
 * that goes through a live ballotd.
 *
 * Needs a JVM and db/dist/simpledb.jar; skips (does not fail) without
 * them, same convention as test_db.c/test_ballotd.c.
 *
 * Run from the repo root: make test
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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

#include "ballotd/control_plane.h"
#include "libballotbrain/ballotbrain.h"
#include "libballotclient/codec.h"
#include "libballotclient/voter.h"
#include "libhtttp/htttp.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "libtetrissh/tetrissh.h"
#include "tetrisdb/runner.h"

/* ---- stage 1-2 fixture: this file's own sandboxed SimpleDB --------------- */

#define SANDBOX_DIR "var/db_bottomup"
#define SANDBOX_SOCK SANDBOX_DIR "/test.sock"
#define TEST_JAR "db/dist/simpledb.jar"

/* ---- stage 3-4 fixture: the real ballotd, the shared store --------------- */

#define BALLOTD_BIN "bin/ballotd"
#define TEST_PORT 17679
#define CTL_PATH "var/run/test_bottomup.ctl"
#define CA_PATH "auth/cacsertificate.crt"
#define LIVE_DB_DIR DB_DEFAULT_DIR
#define LIVE_DB_SOCK DB_DEFAULT_IPC
#define TEST_PASSWORD "correcthorsebatterystaple"

/* The one scenario every stage grows: alice, voting for the second option. */
#define VOTER "alice"
#define CHOSEN_OPTION 1 /* "No" */

static int tests_run = 0, tests_failed = 0;
static pid_t g_ballotd = -1;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      fprintf(stderr, "    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      return -1;                                                          \
    }                                                                     \
  } while (0)

static void nap(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* ---- shared: skip-without-a-JVM gate -------------------------------------- */

static int runner_disabled(void) { return getenv("TETRISH_NO_RUNNER") != NULL; }
static int have_jar(void) { return access(TEST_JAR, R_OK) == 0; }

static int have_java(void) {
  pid_t pid = fork();
  if (pid < 0) return 0;
  if (pid == 0) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    execlp("java", "java", "-version", (char *)NULL);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ============================================================
 * 01 / LEAF - SimpleDB adapter
 *
 * Reuses test_db.c's own driver shape (db_socket_open/db_socket_exec,
 * db_quote) directly against the real store seam. No libballotbrain, no
 * ballotd - this stage does not know what an "election" is, only that a
 * ballot row goes in and the same row comes back out, inside a real
 * transaction, over the real wire protocol SimpleDB speaks.
 * ============================================================ */

/* Shared by both stage 1 (which only ever provisions the ballot table) and
 * stage 2 (which provisions all 6) - lists every table either could have
 * created, so rmdir() below always finds an empty directory. A partial
 * list here silently leaves rmdir() failing (a non-empty directory), which
 * leaves stale election data - e.g. "E-PARENT" - to outlive this process
 * and collide with the next run's bb_create_election. */
static void sandbox_reset(void) {
  char path[512];
  const char *files[] = {"election.dat",         "election_option.dat",
                         "election_eligible.dat", "ballot.dat",
                         "ballot_owner.dat",      "nonce.dat",
                         "catalog.txt"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    snprintf(path, sizeof path, "%s/%s", SANDBOX_DIR, files[i]);
    (void)unlink(path);
  }
  (void)unlink(SANDBOX_SOCK);
  (void)rmdir(SANDBOX_DIR);
}

static pid_t sandbox_start_runner(void) {
  if (db_ensure_table(SANDBOX_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0) {
    fprintf(stderr, "test_bottomup: stage 1: failed to provision the ballot table\n");
    return -1;
  }

  db_runner_opts_t opts;
  memset(&opts, 0, sizeof opts);
  snprintf(opts.dir, sizeof opts.dir, "%s", SANDBOX_DIR);
  snprintf(opts.jar, sizeof opts.jar, "%s", TEST_JAR);
  snprintf(opts.java, sizeof opts.java, "%s", DB_DEFAULT_JAVA);
  snprintf(opts.err_path, sizeof opts.err_path, "%s", DB_DEFAULT_ERR_PATH);
  snprintf(opts.ipc, sizeof opts.ipc, "%s", SANDBOX_SOCK);
  opts.sessions = 4;
  /* recover=0 is load-bearing, not tuning: SimpleDB's write-ahead log is a
   * file called "log" in the CURRENT DIRECTORY, shared by every runner
   * started from the repo root regardless of --dir, so replaying it here
   * would replay another runner's history onto this fresh table (same
   * reasoning as test_db.c's identical setting). */
  opts.recover = 0;

  pid_t pid = db_runner_spawn(&opts, -1);
  if (pid < 0) {
    fprintf(stderr, "test_bottomup: stage 1: cannot spawn a runner\n");
    return -1;
  }
  if (db_runner_wait(SANDBOX_SOCK, pid, 20000) != 0) {
    fprintf(stderr, "test_bottomup: stage 1: runner never accepted a connection\n");
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  return pid;
}

static void sandbox_stop_runner(pid_t pid) {
  kill(pid, SIGTERM);
  waitpid(pid, NULL, 0);
  (void)unlink(SANDBOX_SOCK);
}

static int test_stage1_leaf_simpledb_adapter(void) {
  sandbox_reset();
  pid_t pid = sandbox_start_runner();
  CHECK(pid > 0, "sandbox runner did not start");

  db_socket_opts_t sopts;
  memset(&sopts, 0, sizeof sopts);
  snprintf(sopts.sock, sizeof sopts.sock, "%s", SANDBOX_SOCK);
  db_socket_t *c = db_socket_open(&sopts);
  CHECK(c != NULL, "could not connect to the sandboxed runner");

  /* The exact command shape db.c's run_append_ballot builds for a cast
   * vote, driven by hand: election_id, hash, option_index, version,
   * superseded. */
  char qid[256], qhash[256], sql[1024], body[4096];
  db_quote(qid, sizeof qid, "E-LEAF");
  db_quote(qhash, sizeof qhash, "leaf-hash-0001");

  CHECK(db_socket_exec(c, "set transaction read write;", body, sizeof body) == DB_OK,
        "begin transaction");
  snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_BALLOT " values (%s, %s, %d, %d, %d);", qid,
          qhash, CHOSEN_OPTION, 1, 0);
  CHECK(db_socket_exec(c, sql, body, sizeof body) == DB_OK, "insert ballot row");
  CHECK(db_socket_exec(c, "commit;", body, sizeof body) == DB_OK, "commit transaction");

  snprintf(sql, sizeof sql,
          "select election_id, hash, option_index, version, superseded from " BB_DB_TABLE_BALLOT
          " where hash = %s;",
          qhash);
  CHECK(db_socket_exec(c, sql, body, sizeof body) == DB_OK, "select the row back");
  CHECK(db_row_count(body) == 1, "exactly one row for this hash");

  const char *f[8];
  size_t flen[8];
  CHECK(db_row_fields(body, 0, f, flen, 8) == 5, "row should carry all 5 columns");
  CHECK(flen[2] == 1 && f[2][0] == '0' + CHOSEN_OPTION, "option_index should be the chosen option");
  CHECK(flen[4] == 1 && f[4][0] == '0', "a fresh row is never superseded");

  db_socket_close(c);
  sandbox_stop_runner(pid);
  sandbox_reset();
  return 0;
}

/* ============================================================
 * 02 / PARENT - ballotd + store
 *
 * The fake database is gone: bb_set_db_opts points a real bb_ctx at the
 * same sandboxed runner stage 1 just proved works, and bb_record_ballot -
 * libballotbrain's real logic, unmodified from what the unit tests
 * exercise against the fake seam - now runs against the real one. Still no
 * ballotd process, no wire: this stage is "the parent that owns the leaf",
 * not the daemon that owns the parent.
 * ============================================================ */

static int test_stage2_parent_ballotd_and_store(void) {
  sandbox_reset();
  if (db_ensure_table(SANDBOX_DIR, BB_DB_TABLE_ELECTION, BB_DB_SCHEMA_ELECTION) != 0 ||
      db_ensure_table(SANDBOX_DIR, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) != 0 ||
      db_ensure_table(SANDBOX_DIR, BB_DB_TABLE_ELIGIBLE, BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      db_ensure_table(SANDBOX_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0 ||
      db_ensure_table(SANDBOX_DIR, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) != 0 ||
      db_ensure_table(SANDBOX_DIR, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) != 0) {
    fprintf(stderr, "test_bottomup: stage 2: failed to provision tables\n");
    return -1;
  }

  db_runner_opts_t opts;
  memset(&opts, 0, sizeof opts);
  snprintf(opts.dir, sizeof opts.dir, "%s", SANDBOX_DIR);
  snprintf(opts.jar, sizeof opts.jar, "%s", TEST_JAR);
  snprintf(opts.java, sizeof opts.java, "%s", DB_DEFAULT_JAVA);
  snprintf(opts.err_path, sizeof opts.err_path, "%s", DB_DEFAULT_ERR_PATH);
  snprintf(opts.ipc, sizeof opts.ipc, "%s", SANDBOX_SOCK);
  opts.sessions = 4;
  opts.recover = 0;
  pid_t pid = db_runner_spawn(&opts, -1);
  CHECK(pid > 0, "sandbox runner did not start");
  CHECK(db_runner_wait(SANDBOX_SOCK, pid, 20000) == 0, "runner never accepted a connection");

  bb_ctx *ctx = bb_create();
  db_socket_opts_t sopts;
  memset(&sopts, 0, sizeof sopts);
  snprintf(sopts.sock, sizeof sopts.sock, "%s", SANDBOX_SOCK);
  bb_set_db_opts(ctx, &sopts);
  bb_set_log(ctx, NULL);

  bb_config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  snprintf(cfg.title, BB_TITLE_LEN, "Parent Stage");
  snprintf(cfg.options[0], BB_OPTION_LEN, "Yes");
  snprintf(cfg.options[1], BB_OPTION_LEN, "No");
  cfg.option_count = 2;
  snprintf(cfg.eligible[0], BB_CERT_LEN, VOTER);
  cfg.eligible_count = 1;
  snprintf(cfg.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(cfg.close_time, BB_TIME_LEN, "2026-01-08T00:00:00Z");

  char id[BB_ID_LEN];
  CHECK(bb_create_election(ctx, &cfg, "E-PARENT", id) == BB_OK, "create failed");
  CHECK(bb_transition_state(ctx, id, BB_STATE_OPEN) == BB_OK, "open failed");

  bb_ballot_t ballot;
  memset(&ballot, 0, sizeof ballot);
  snprintf(ballot.cert_name, BB_CERT_LEN, VOTER);
  snprintf(ballot.nonce, BB_NONCE_LEN, "nonce-parent-stage");
  ballot.payload[0] = CHOSEN_OPTION;
  ballot.payload_len = 1;

  bb_receipt_t receipt;
  memset(&receipt, 0, sizeof receipt);
  CHECK(bb_record_ballot(ctx, id, &ballot, &receipt) == BB_OK, "record should succeed");
  CHECK(receipt.hash[0] != '\0', "a receipt hash should be issued");

  /* "Persisted exactly once": the exact same nonce cannot land a second row. */
  bb_receipt_t replay;
  memset(&replay, 0, sizeof replay);
  CHECK(bb_record_ballot(ctx, id, &ballot, &replay) == BB_ERR_REPLAY,
        "the identical ballot must not be recorded twice");

  bb_ballot_hash_t row;
  memset(&row, 0, sizeof row);
  CHECK(bb_lookup_hash(ctx, id, receipt.hash, &row, NULL) == BB_OK, "the ballot should be findable");
  CHECK(row.option_index == CHOSEN_OPTION, "stored option should match what was cast");
  CHECK(row.superseded == 0, "the one and only cast should not be superseded");

  bb_destroy(ctx);
  sandbox_stop_runner(pid);
  sandbox_reset();
  return 0;
}

/* ============================================================
 * 03 / TRANSPORT - Secure session
 *
 * Real tetrissh (session_connect over a real TCP socket to a real
 * ballotd), real HTTTP wire encoding (bcl_encode_request/htttp_parse_
 * response, libballotclient/codec.c) - but not libballotclient's session
 * API: this stage drives session_t and the codec by hand, the same shape
 * test_ballotd.c uses, so nothing above the wire is trusted yet. The
 * election is created over the (already-real, already-covered elsewhere)
 * admin channel; what's new at this stage is the voter side growing a
 * transport.
 * ============================================================ */

static pid_t start_ballotd(void) {
  unlink(CTL_PATH);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char port_buf[16];
    snprintf(port_buf, sizeof port_buf, "%d", TEST_PORT);
    execl(BALLOTD_BIN, BALLOTD_BIN, "-p", port_buf, "-C", CTL_PATH, "-d", LIVE_DB_DIR, "-i",
          LIVE_DB_SOCK, (char *)NULL);
    perror("execl " BALLOTD_BIN);
    _exit(127);
  }

  for (int i = 0; i < 300; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof addr);
      addr.sin_family = AF_INET;
      addr.sin_port = htons((unsigned short)TEST_PORT);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        close(fd);
        g_ballotd = pid;
        return pid;
      }
      close(fd);
    }
    nap(10);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  return -1;
}

static int stop_ballotd(pid_t pid) {
  int status = 0;
  if (kill(pid, SIGTERM) < 0) return -1;
  if (waitpid(pid, &status, 0) != pid) return -1;
  g_ballotd = -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int ctl_connect(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", CTL_PATH);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int ctl_send_recv(int fd, const uint8_t *wire, uint32_t wlen, uint8_t *rbuf, uint32_t rcap,
                         htttp_response_t *http) {
  if (ctl_frame_write(fd, wire, wlen) != 0) return -1;
  uint32_t rlen = 0;
  if (ctl_frame_read(fd, rbuf, rcap, &rlen) != 0) return -1;
  return htttp_parse_response(rbuf, rlen, http) == HTTTP_OK ? 0 : -1;
}

/* Creates an election eligible for VOTER and opens it, over the admin
 * channel. Returns 0 and fills out_id, or -1.
 *
 * election_id is left blank so the server auto-allocates a fresh "E-NNN" -
 * this file's stages 3-4 run against the shared project store (see the
 * file header), which persists across repeated runs, unlike the sandboxed
 * stages 1-2. A hardcoded id here would collide with itself on the very
 * next `make test` and fail with BB_ERR_CONFIG_ID_TAKEN. */
static int create_and_open(const char *title, char out_id[BB_ID_LEN]) {
  int fd = ctl_connect();
  if (fd < 0) return -1;

  bcl_request_t creq;
  memset(&creq, 0, sizeof creq);
  creq.op = BCL_CREATE;
  snprintf(creq.cert_name, BB_CERT_LEN, "admin");
  snprintf(creq.config.title, BB_TITLE_LEN, "%s", title);
  snprintf(creq.config.options[0], BB_OPTION_LEN, "Yes");
  snprintf(creq.config.options[1], BB_OPTION_LEN, "No");
  creq.config.option_count = 2;
  snprintf(creq.config.eligible[0], BB_CERT_LEN, VOTER);
  creq.config.eligible_count = 1;
  snprintf(creq.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(creq.config.close_time, BB_TIME_LEN, "2026-01-08T00:00:00Z");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  if (bcl_encode_request(&creq, wire, &wlen) != 0) { close(fd); return -1; }
  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  if (ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) != 0 || http.status != 200) {
    close(fd);
    return -1;
  }
  close(fd);

  bcl_response_t cresp;
  memset(&cresp, 0, sizeof cresp);
  if (bcl_decode_response(&http, &cresp) != 0 || cresp.election.id[0] == '\0') return -1;
  snprintf(out_id, BB_ID_LEN, "%s", cresp.election.id);

  fd = ctl_connect();
  if (fd < 0) return -1;
  bcl_request_t oreq;
  memset(&oreq, 0, sizeof oreq);
  oreq.op = BCL_OPEN;
  snprintf(oreq.election_id, BB_ID_LEN, "%s", out_id);
  snprintf(oreq.cert_name, BB_CERT_LEN, "admin");
  wlen = sizeof wire;
  if (bcl_encode_request(&oreq, wire, &wlen) != 0) { close(fd); return -1; }
  if (ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) != 0 || http.status != 200) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int voter_auth(session_t *cli, const char *user, const char *method) {
  char body[256];
  int blen = snprintf(body, sizeof body, "%s\n%s", user, TEST_PASSWORD);
  if (blen < 0 || (size_t)blen >= sizeof body) return -1;

  htttp_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.method, sizeof req.method, "%s", method);
  snprintf(req.path, sizeof req.path, "/");
  req.body = (const uint8_t *)body;
  req.body_len = (uint32_t)blen;

  uint8_t wire[SESSION_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  if (htttp_serialize_request(&req, wire, &wlen) != HTTTP_OK) return -1;
  if (session_send(cli, wire, wlen) != SESSION_OK) return -1;

  uint8_t rbuf[SESSION_MAX_FRAME];
  uint32_t rlen = sizeof rbuf;
  if (session_recv(cli, rbuf, &rlen) != SESSION_OK) return -1;
  htttp_response_t resp;
  if (htttp_parse_response(rbuf, rlen, &resp) != HTTTP_OK) return -1;
  return resp.status;
}

static int voter_login_or_register(session_t *cli, const char *user) {
  int status = voter_auth(cli, user, "LOGIN");
  if (status == 200) return 0;
  if (status != 404) return -1;
  return voter_auth(cli, user, "REGISTER") == 200 ? 0 : -1;
}

static int test_stage3_transport_secure_session(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  char id[BB_ID_LEN];
  CHECK(create_and_open("Transport Stage", id) == 0, "create+open over the admin channel");

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0, "socket() failed");
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(TEST_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0, "connect failed");

  session_t cli;
  CHECK(session_connect(&cli, fd, CA_PATH) == SESSION_OK, "real tetrissh handshake failed");
  CHECK(voter_login_or_register(&cli, VOTER) == 0, "real auth failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CAST;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.cert_name, BB_CERT_LEN, VOTER);
  snprintf(req.ballot.cert_name, BB_CERT_LEN, VOTER);
  snprintf(req.ballot.nonce, BB_NONCE_LEN, "nonce-transport-stage");
  req.ballot.payload[0] = CHOSEN_OPTION;
  req.ballot.payload_len = 1;

  uint8_t wire[SESSION_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "real wire encoding failed");
  CHECK(session_send(&cli, wire, wlen) == SESSION_OK, "send over the real session failed");

  uint8_t rbuf[SESSION_MAX_FRAME];
  uint32_t rlen = sizeof rbuf;
  CHECK(session_recv(&cli, rbuf, &rlen) == SESSION_OK, "recv over the real session failed");
  htttp_response_t http;
  CHECK(htttp_parse_response(rbuf, rlen, &http) == HTTTP_OK, "real wire decoding failed");
  CHECK(http.status == 200, "CAST over the real secure session should succeed");

  session_close(&cli);
  close(fd);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ============================================================
 * 04 / TOP - ballotu client
 *
 * The public client API, last: bcl_connect/bu_join/bu_submit_vote - the
 * exact calls ballotu.c itself makes, nothing hand-rolled below them
 * anymore. The receipt bu_submit_vote hands back is then checked
 * independently against the store, over the admin channel's ADMIN_CHECK
 * (bb_lookup_hash) - not just trusted because the client claims it - so
 * this stage proves the receipt matches what was actually persisted, the
 * same property stage 2 proved one layer down.
 * ============================================================ */

static int test_stage4_top_ballotu_client(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  char id[BB_ID_LEN];
  CHECK(create_and_open("Top Stage", id) == 0, "create+open over the admin channel");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, "127.0.0.1", TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");

  int status = 0;
  int rc = bcl_auth(ctx, "LOGIN", VOTER, TEST_PASSWORD, &status);
  if (rc != 0 || status == 404) {
    rc = bcl_auth(ctx, "REGISTER", VOTER, TEST_PASSWORD, &status);
  }
  CHECK(rc == 0 && status == 200, "real login through the public client API failed");

  bu_session_t session;
  memset(&session, 0, sizeof session);
  CHECK(bu_join(ctx, &session, id, VOTER) == BU_JOIN_ADMITTED, "bu_join should admit the voter");

  bb_receipt_t receipt;
  memset(&receipt, 0, sizeof receipt);
  CHECK(bu_submit_vote(ctx, &session, CHOSEN_OPTION, "nonce-top-stage", &receipt) == BB_OK,
        "bu_submit_vote should succeed");
  CHECK(receipt.hash[0] != '\0', "the public API should hand back a receipt hash");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);

  /* Independent check: does the store actually agree with what the client
   * believes it received? Over the admin channel, not the voter session
   * that just closed - a fresh, unrelated read. */
  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");
  bcl_request_t creq;
  memset(&creq, 0, sizeof creq);
  creq.op = BCL_ADMIN_CHECK;
  snprintf(creq.election_id, BB_ID_LEN, "%s", id);
  snprintf(creq.hash, BB_HASH_LEN, "%s", receipt.hash);
  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&creq, wire, &wlen) == 0, "encode ADMIN_CHECK");
  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);
  CHECK(http.status == 200, "the receipt's hash should be found in the store");

  bcl_response_t cresp;
  memset(&cresp, 0, sizeof cresp);
  CHECK(bcl_decode_response(&http, &cresp) == 0, "decode ADMIN_CHECK response");
  CHECK(cresp.found == 1, "the stored ballot should be live, not dropped");
  CHECK(cresp.found_option == CHOSEN_OPTION,
        "the stored option should match what the client's receipt claims");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ---- fixture: shared live runner (stages 3-4) ----------------------------- */

static int start_live_runner(void) {
  if (db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELECTION, BB_DB_SCHEMA_ELECTION) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELIGIBLE, BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) != 0) {
    fprintf(stderr, "test_bottomup: fixture: failed to provision tables\n");
    return -1;
  }
  int start_rc = system("./bin/tetrisdb start >/dev/null 2>&1");
  (void)start_rc;

  db_socket_opts_t sopts;
  db_socket_opts_load(&sopts);
  for (int i = 0; i < 100; i++) {
    db_socket_t *probe = db_socket_open(&sopts);
    if (probe != NULL) {
      db_socket_close(probe);
      return 0;
    }
    nap(100);
  }
  fprintf(stderr, "test_bottomup: fixture: shared runner never became reachable\n");
  return -1;
}

/* ---- harness --------------------------------------------------------------- */

static void run(const char *name, int (*fn)(void)) {
  tests_run++;
  printf("  %-52s", name);
  fflush(stdout);
  if (fn() == 0) {
    printf("ok\n");
  } else {
    tests_failed++;
    printf("  -> FAILED\n");
  }
  if (g_ballotd > 0) {
    kill(g_ballotd, SIGKILL);
    waitpid(g_ballotd, NULL, 0);
    g_ballotd = -1;
  }
}

int main(void) {
  printf("bottom-up integration: one cast-vote request, four stages\n");

  if (!runner_disabled() && have_jar() && have_java()) {
    struct stat st;
    if (stat(BALLOTD_BIN, &st) != 0) {
      fprintf(stderr, "test_bottomup: %s not built (run make)\n", BALLOTD_BIN);
      return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    run("01/leaf: SimpleDB adapter", test_stage1_leaf_simpledb_adapter);
    run("02/parent: ballotd + store", test_stage2_parent_ballotd_and_store);

    if (start_live_runner() == 0) {
      run("03/transport: secure session", test_stage3_transport_secure_session);
      run("04/top: ballotu client", test_stage4_top_ballotu_client);
    } else {
      tests_failed++;
    }
  } else {
    printf("  (skipping: %s)\n", runner_disabled()   ? "TETRISH_NO_RUNNER is set"
                                  : !have_jar()       ? "no " TEST_JAR " - run `ant dist` in db/"
                                                       : "no java on PATH");
    if (!runner_disabled() && getenv("TETRISH_REQUIRE_RUNNER") != NULL) {
      fprintf(stderr, "    FAIL: TETRISH_REQUIRE_RUNNER is set and the runner tests could not run\n");
      tests_failed++;
    }
  }

  unlink(CTL_PATH);
  printf("%d/%d passed\n", tests_run - tests_failed, tests_run);
  return tests_failed == 0 ? 0 : 1;
}
