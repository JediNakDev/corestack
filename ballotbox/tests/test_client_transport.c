/*
 * End-to-end tests for the real client transport (bcl_connect/bcl_send,
 * src/libballotclient/transport.c) against a real bin/ballotd - the other
 * half of test_ballotd.c's picture. That file drives the daemon from raw
 * sockets to prove the server side; this one drives the real
 * libballotclient API (the same calls ballotu.c makes) to prove the client
 * side actually works, not just the harness that talks to it directly.
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libballotbrain/db.h"
#include "libballotclient/voter.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "tetrisdb/runner.h"

#define BALLOTD_BIN "bin/ballotd"
#define TEST_PORT 17678
#define CTL_PATH "var/run/test_client_transport.ctl"
#define CA_PATH "auth/cacsertificate.crt"
#define HOST "127.0.0.1"
#define TEST_PASSWORD "correcthorsebatterystaple"

/* UNREACH_*: a sandbox of this file's own, same isolation reasoning as
 * test_ballotd.c - a socket nothing binds, for tests that want a
 * deterministic "the store is unreachable" outcome.
 *
 * LIVE_*: NOT a sandbox, unlike UNREACH_* - the shared project default
 * (var/db, var/run/tetrisdb.sock). See test_ballotd.c's LIVE_DB_DIR comment
 * for why: every voter-channel op now goes through auth_login() first,
 * which reads db_ipc/db_dir from .tetrishrc with no override this file can
 * reach, so the live-store tests use the same runner auth is already
 * committed to rather than sandbox ballotd's own tables away from it. */
#define UNREACH_DB_DIR "var/db/test_client_transport_unreachable"
#define UNREACH_DB_SOCK "var/run/test_client_transport_unreachable.sock"
#define LIVE_DB_DIR DB_DEFAULT_DIR
#define LIVE_DB_SOCK DB_DEFAULT_IPC
#define TEST_JAR "db/dist/simpledb.jar"

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

static int wait_for_tcp(int port) {
  for (int i = 0; i < 300; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof addr);
      addr.sin_family = AF_INET;
      addr.sin_port = htons((unsigned short)port);
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

static pid_t start_ballotd_ex(const char *db_dir, const char *db_sock) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char port_buf[16];
    snprintf(port_buf, sizeof port_buf, "%d", TEST_PORT);
    execl(BALLOTD_BIN, BALLOTD_BIN, "-p", port_buf, "-C", CTL_PATH, "-d", db_dir, "-i", db_sock,
          (char *)NULL);
    perror("execl " BALLOTD_BIN);
    _exit(127);
  }
  if (wait_for_tcp(TEST_PORT) < 0) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  g_ballotd = pid;
  return pid;
}

/* Default fixture: DB deliberately unreachable. */
static pid_t start_ballotd(void) { return start_ballotd_ex(UNREACH_DB_DIR, UNREACH_DB_SOCK); }

/* Fixture for the runner-guarded block below. */
static pid_t start_ballotd_live(void) { return start_ballotd_ex(LIVE_DB_DIR, LIVE_DB_SOCK); }

static int stop_ballotd(pid_t pid) {
  int status = 0;
  if (kill(pid, SIGTERM) < 0) return -1;
  if (waitpid(pid, &status, 0) != pid) return -1;
  g_ballotd = -1;
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

/* Logs in as `user` over ctx's already-open voter session (bcl_connect),
 * registering first if no such account exists yet - idempotent across
 * repeated `make test` runs against the same shared var/db (see
 * LIVE_DB_DIR's comment above). Every JOIN/CAST/UPDATE/RESULTS/CHECK is
 * unreachable until this succeeds - ballotd/session.c calls auth_login()
 * before dispatching anything. */
static int voter_login_or_register(bcl_ctx *ctx, const char *user) {
  int status = 0;
  if (bcl_auth(ctx, "LOGIN", user, TEST_PASSWORD, &status) == 0 && status == 200) return 0;
  if (status != 404) return -1;
  return bcl_auth(ctx, "REGISTER", user, TEST_PASSWORD, &status) == 0 && status == 200 ? 0 : -1;
}

/* ---- tests ------------------------------------------------------------- */

static int test_connect_succeeds(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");

  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect should succeed");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

static int test_connect_fails_when_daemon_down(void) {
  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");

  /* Nothing listens on TEST_PORT here - no daemon was started this test. */
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_ERR_DB,
        "bcl_connect must fail cleanly against a closed port");

  bcl_disconnect(ctx); /* no-op: never connected */
  bcl_destroy(ctx);
  return 0;
}

/* There is deliberately no "JOIN/RESULTS/bcl_auth fails cleanly with no
 * reachable store" case here anymore (this file used to have two: a
 * bu_join-shaped one and a bcl_send(RESULTS)-shaped one). Every voter op is
 * unreachable pre-auth now, and unlike ballotd's own db_exec calls,
 * libtetrisauth's auth_login() does not honour this file's -d/-i sandbox
 * at all - it reads db_ipc/db_dir from .tetrishrc unconditionally (see
 * LIVE_DB_DIR's comment above), which in practice means auth's reachability
 * here tracks whatever the AMBIENT shared runner's state happens to be,
 * not anything UNREACH_DB_SOCK controls. That makes "no reachable store"
 * an assertion this file cannot reliably force anymore, so there is nothing
 * trustworthy left to check - see test_bcl_send_admin_op_without_ctl_path_
 * configured below and test_ballotd.c's admin-channel cases for BB_ERR_DB
 * propagation still covered elsewhere. */

static bcl_request_t make_create_request(const char *title) {
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  snprintf(req.config.title, BB_TITLE_LEN, "%s", title);
  snprintf(req.config.options[0], BB_OPTION_LEN, "Yes");
  snprintf(req.config.options[1], BB_OPTION_LEN, "No");
  req.config.option_count = 2;
  snprintf(req.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(req.config.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");
  return req;
}

/* bcl_send routes CREATE to the ctl socket regardless of whether a voter
 * session is connected - bcl_connect() alone (no bcl_set_ctl_path) must not
 * make an admin op fall back to the voter session; it should fail cleanly
 * instead. The actual "wrong channel" rejection (ballot_session's own gate
 * refusing an admin op over TCP+tetrissh) is exercised at the wire level in
 * test_ballotd.c - the client library's routing means there is no longer a
 * public bcl_send path that could even reach that gate. */
static int test_bcl_send_admin_op_without_ctl_path_configured(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");

  bcl_request_t req = make_create_request("Should be rejected");
  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);

  CHECK(rc == BB_ERR_DB, "admin op with no ctl_path configured must fail cleanly");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* The real admin path: bcl_set_ctl_path, no bcl_connect at all - ballotctl
 * never touches the voter channel. Needs a live store: CREATE really writes
 * now. */
static int test_bcl_send_create_via_ctl_succeeds(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  bcl_set_ctl_path(ctx, CTL_PATH);

  bcl_request_t req = make_create_request("Officers 2026");
  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);

  CHECK(rc == BB_OK, "CREATE over the real ctl socket should succeed");
  CHECK(resp.election.id[0] != '\0', "election id should be set");

  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* bu_join's ADMITTED path, all the way through the real client library: a
 * real CREATE, a real OPEN (bu_join only admits an OPEN election), then a
 * real JOIN as an eligible voter over the encrypted channel. */
static int test_join_admitted_via_live_store(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *actl = bcl_create();
  CHECK(actl != NULL, "bcl_create failed");
  bcl_set_ctl_path(actl, CTL_PATH);

  bcl_request_t creq = make_create_request("Live Join");
  snprintf(creq.config.eligible[0], BB_CERT_LEN, "alice");
  creq.config.eligible_count = 1;
  bcl_response_t cresp;
  memset(&cresp, 0, sizeof cresp);
  CHECK(bcl_send(actl, &creq, &cresp) == BB_OK, "CREATE should succeed");

  bcl_request_t oreq;
  memset(&oreq, 0, sizeof oreq);
  oreq.op = BCL_OPEN;
  snprintf(oreq.election_id, BB_ID_LEN, "%s", cresp.election.id);
  snprintf(oreq.cert_name, BB_CERT_LEN, "admin");
  bcl_response_t oresp;
  memset(&oresp, 0, sizeof oresp);
  CHECK(bcl_send(actl, &oreq, &oresp) == BB_OK, "OPEN should succeed");
  bcl_destroy(actl);

  bcl_ctx *voter = bcl_create();
  CHECK(voter != NULL, "bcl_create failed");
  CHECK(bcl_connect(voter, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");
  CHECK(voter_login_or_register(voter, "alice") == 0, "auth as alice failed");

  bu_session_t session;
  memset(&session, 0, sizeof session);
  bu_join_outcome_t outcome = bu_join(voter, &session, cresp.election.id, "alice");

  CHECK(outcome == BU_JOIN_ADMITTED, "eligible voter on an OPEN election should be admitted");
  CHECK(session.joined == 1, "session should be marked joined");
  CHECK(strcmp(session.election_id, cresp.election.id) == 0, "session should record the election id");

  bcl_disconnect(voter);
  bcl_destroy(voter);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Regression (reported live): log in, join, cast a vote, then act out
 * "close ballotu and reopen it" - a fresh bcl_ctx and a fresh, zeroed
 * bu_session_t, exactly what a new process has - log in again and JOIN the
 * same election again. Before the has_prior_ballot/prior_ballot_version
 * fix (bb_join, handlers.c; bu_join, voter.c), the fresh session had no way
 * to know this cert already had a ballot, so bu_route_vote always chose
 * BU_CAST and a returning voter's next vote silently overwrote their
 * receipt instead of superseding it. This drives the real daemon and the
 * real client library end to end, the same calls ballotu.c makes. */
static int test_rejoin_after_cast_reports_prior_ballot(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *actl = bcl_create();
  CHECK(actl != NULL, "bcl_create failed");
  bcl_set_ctl_path(actl, CTL_PATH);

  bcl_request_t creq = make_create_request("Rejoin Regression");
  snprintf(creq.config.eligible[0], BB_CERT_LEN, "alice");
  creq.config.eligible_count = 1;
  bcl_response_t cresp;
  memset(&cresp, 0, sizeof cresp);
  CHECK(bcl_send(actl, &creq, &cresp) == BB_OK, "CREATE should succeed");

  bcl_request_t oreq;
  memset(&oreq, 0, sizeof oreq);
  oreq.op = BCL_OPEN;
  snprintf(oreq.election_id, BB_ID_LEN, "%s", cresp.election.id);
  snprintf(oreq.cert_name, BB_CERT_LEN, "admin");
  bcl_response_t oresp;
  memset(&oresp, 0, sizeof oresp);
  CHECK(bcl_send(actl, &oreq, &oresp) == BB_OK, "OPEN should succeed");
  bcl_destroy(actl);

  /* First "session": join fresh (no prior ballot yet) and cast. */
  bcl_ctx *voter1 = bcl_create();
  CHECK(voter1 != NULL, "bcl_create failed");
  CHECK(bcl_connect(voter1, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");
  CHECK(voter_login_or_register(voter1, "alice") == 0, "auth as alice failed");

  bu_session_t session1;
  memset(&session1, 0, sizeof session1);
  CHECK(bu_join(voter1, &session1, cresp.election.id, "alice") == BU_JOIN_ADMITTED,
        "first join should be admitted");
  CHECK(session1.has_ballot == 0, "a fresh election has no prior ballot yet");

  bb_receipt_t receipt;
  memset(&receipt, 0, sizeof receipt);
  CHECK(bu_submit_vote(voter1, &session1, 0, "nonce-regression-1", &receipt) == BB_OK,
        "cast should succeed");

  bcl_disconnect(voter1);
  bcl_destroy(voter1);

  /* "Close ballotu and reopen it": a brand new ctx, a brand new (zeroed)
   * session - nothing carried over from session1 in memory. */
  bcl_ctx *voter2 = bcl_create();
  CHECK(voter2 != NULL, "bcl_create failed");
  CHECK(bcl_connect(voter2, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");
  CHECK(voter_login_or_register(voter2, "alice") == 0, "re-auth as alice failed");

  bu_session_t session2;
  memset(&session2, 0, sizeof session2);
  CHECK(bu_join(voter2, &session2, cresp.election.id, "alice") == BU_JOIN_ADMITTED,
        "rejoin should be admitted");

  CHECK(session2.has_ballot == 1, "rejoin must report the prior ballot cast in session1");
  CHECK(session2.ballot_version == 1, "prior ballot's version should be reported as 1");
  CHECK(bu_route_vote(&session2) == BU_UPDATE,
        "a rejoined voter with a prior ballot must route to update, not cast");

  bcl_disconnect(voter2);
  bcl_destroy(voter2);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* RESULTS (admin channel, which needs no eligible-list check) carries the
 * election's title alongside the tally - dispatch.c's BCL_ADMIN_RESULTS
 * case populates resp->election from the same fetch_results() the voter
 * RESULTS path shares, so this covers both. */
static int test_admin_results_include_title(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *actl = bcl_create();
  CHECK(actl != NULL, "bcl_create failed");
  bcl_set_ctl_path(actl, CTL_PATH);

  bcl_request_t creq = make_create_request("Wire Title Check");
  bcl_response_t cresp;
  memset(&cresp, 0, sizeof cresp);
  CHECK(bcl_send(actl, &creq, &cresp) == BB_OK, "CREATE should succeed");

  const bcl_op_t transitions[] = {BCL_OPEN, BCL_CLOSE, BCL_PUBLISH};
  for (int i = 0; i < 3; i++) {
    bcl_request_t treq;
    memset(&treq, 0, sizeof treq);
    treq.op = transitions[i];
    snprintf(treq.election_id, BB_ID_LEN, "%s", cresp.election.id);
    snprintf(treq.cert_name, BB_CERT_LEN, "admin");
    bcl_response_t tresp;
    memset(&tresp, 0, sizeof tresp);
    CHECK(bcl_send(actl, &treq, &tresp) == BB_OK, "lifecycle transition should succeed");
  }

  bcl_request_t rreq;
  memset(&rreq, 0, sizeof rreq);
  rreq.op = BCL_ADMIN_RESULTS;
  snprintf(rreq.election_id, BB_ID_LEN, "%s", cresp.election.id);
  bcl_response_t rresp;
  memset(&rresp, 0, sizeof rresp);
  CHECK(bcl_send(actl, &rreq, &rresp) == BB_OK, "ADMIN_RESULTS should succeed");

  CHECK(strcmp(rresp.election.id, cresp.election.id) == 0, "results should carry the election id");
  CHECK(strcmp(rresp.election.title, "Wire Title Check") == 0,
        "results should carry the election title");

  bcl_destroy(actl);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Pure-logic path (bb_validate_config), no DB dependency - deterministic
 * today regardless of the frozen DB seam, same as test_ballotd.c's version
 * of this case but through the real client library instead of raw sockets. */
static int test_bcl_send_create_invalid_config_via_ctl(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  bcl_set_ctl_path(ctx, CTL_PATH);

  bcl_request_t req = make_create_request(""); /* empty title */
  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);

  CHECK(rc == BB_ERR_CONFIG_TITLE, "empty title should be refused");
  CHECK(resp.election.id[0] == '\0', "no election id on a failed create");

  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

static int test_send_after_disconnect_fails_cleanly(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");
  bcl_disconnect(ctx);

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_RESULTS;
  snprintf(req.election_id, BB_ID_LEN, "E-042");

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  CHECK(bcl_send(ctx, &req, &resp) == BB_ERR_DB, "send after disconnect must fail, not crash");

  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ---- fixture: a live SocketRunner ------------------------------------------ */

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

/* No stop_runner(): unlike UNREACH_*'s fixtures, this file does not own the
 * live runner's lifecycle - see LIVE_DB_DIR's comment above. It is the
 * shared project runner, most likely already running, and there is nothing
 * for this file to tear down afterward. */

/* Provisions BallotBox's own 6 tables (bin/tetrisdb's own startup policy
 * only provisions TETRISAUTH_DB_TABLE, "user"), then starts the shared
 * runner exactly the way an operator would: `bin/tetrisdb start` is
 * idempotent, so a runner already up is left alone rather than fought
 * over. */
static int start_live_runner(void) {
  if (db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELECTION, BB_DB_SCHEMA_ELECTION) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELIGIBLE, BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) != 0) {
    fprintf(stderr, "test_client_transport: fixture: failed to provision tables\n");
    return -1;
  }

  /* Exit code deliberately not treated as pass/fail: "already running" and
   * "just started it" are both fine outcomes here, and the reachability
   * poll below is the actual signal either way. */
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
  fprintf(stderr, "test_client_transport: fixture: shared runner never became reachable\n");
  return -1;
}

/* ---- harness ------------------------------------------------------------- */

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
  struct stat st;
  if (stat(BALLOTD_BIN, &st) != 0) {
    fprintf(stderr, "test_client_transport: %s not built (run make)\n", BALLOTD_BIN);
    return 1;
  }
  if (stat("bin/ballot_session", &st) != 0) {
    fprintf(stderr, "test_client_transport: bin/ballot_session not built (run make)\n");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  printf("client transport end-to-end tests\n");
  run("bcl_connect succeeds against a real ballotd", test_connect_succeeds);
  run("bcl_connect fails cleanly against a closed port", test_connect_fails_when_daemon_down);
  run("bcl_send admin op fails cleanly with no ctl_path set",
      test_bcl_send_admin_op_without_ctl_path_configured);
  run("bcl_send(CREATE) via ctl: invalid config refused",
      test_bcl_send_create_invalid_config_via_ctl);
  run("bcl_send after bcl_disconnect fails cleanly", test_send_after_disconnect_fails_cleanly);

  if (!runner_disabled() && have_jar() && have_java()) {
    if (start_live_runner() == 0) {
      run("bcl_send(CREATE) via ctl succeeds (live store)", test_bcl_send_create_via_ctl_succeeds);
      run("bu_join admits an eligible voter (live store)", test_join_admitted_via_live_store);
      run("rejoin after cast reports prior ballot (live store)",
          test_rejoin_after_cast_reports_prior_ballot);
      run("ADMIN_RESULTS includes the election title (live store)",
          test_admin_results_include_title);
    } else {
      tests_failed++;
    }
  } else {
    printf("  (skipping live-store tests: %s)\n",
           runner_disabled()   ? "TETRISH_NO_RUNNER is set"
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
