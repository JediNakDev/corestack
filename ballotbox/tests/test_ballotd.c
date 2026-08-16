/*
 * End-to-end tests for ballotd: the real daemon, the real TCP+tetrissh
 * voter channel (forking the real bin/ballot_session per connection), and
 * the real local admin channel. Each test starts a fresh daemon, drives it
 * over one or both channels, and stops it with SIGTERM.
 *
 * Run from the repo root: make test
 */
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
#include "libballotbrain/db.h"
#include "libballotclient/codec.h"
#include "libhtttp/htttp.h"
#include "libtetrisauth/auth.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "tetrisdb/runner.h"
#include "libtetrissh/tetrissh.h"

#define BALLOTD_BIN "bin/ballotd"
#define TEST_PORT 17677
#define CTL_PATH "var/run/test_ballotd.ctl"
#define CA_PATH "auth/cacsertificate.crt"

/* UNREACH_* names a path nothing ever binds, for admin-channel tests (the
 * only ones left that can still be exercised with no DB at all - see below)
 * that want a deterministic "the store is unreachable" outcome without a
 * live runner.
 *
 * LIVE_* is the shared project default (var/db, var/run/tetrisdb.sock), NOT
 * a sandbox of this file's own, unlike every other db_dir/db_sock pair in
 * this codebase. That is not a style slip: ballotd's own db_exec honours
 * whatever -d/-i this file passes it, but libtetrisauth's auth_login() -
 * which every voter-channel test now goes through first, since JOIN/CAST/
 * etc. are unreachable pre-auth - reads db_ipc/db_dir from .tetrishrc via
 * auth_conf_load(), with no override this file can reach. Sandboxing
 * ballotd's own tables while auth still talks to the shared runner would
 * just mean two different databases disagree about what "the store" is, so
 * the live-store tests use the one auth is already committed to instead. A
 * real consequence: these tests write real rows into the same var/db a
 * manually-run ballotd/bin/tetrisdb also uses - acceptable for now, but
 * worth fixing properly (an isolated sandbox directory with its own
 * .tetrishrc, the way tetriSH's own test_auth.c does it) if this suite's
 * isolation from manual testing starts to matter more than it does today. */
#define UNREACH_DB_DIR "var/db/test_ballotd_unreachable"
#define UNREACH_DB_SOCK "var/run/test_ballotd_unreachable.sock"
#define LIVE_DB_DIR DB_DEFAULT_DIR
#define LIVE_DB_SOCK DB_DEFAULT_IPC
#define TEST_JAR "db/dist/simpledb.jar"
#define TEST_PASSWORD "correcthorsebatterystaple"

static int tests_run = 0, tests_failed = 0;

/* The daemon a test currently has running, so a failing test (which returns
 * early) cannot leak one into the next test - same convention as
 * tests/test_logd.c's g_logd. */
static pid_t g_ballotd = -1;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);    \
      return -1;                                                             \
    }                                                                        \
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

static int wait_for_path(const char *path) {
  struct stat st;
  for (int i = 0; i < 300; i++) {
    if (lstat(path, &st) == 0) return 0;
    nap(10);
  }
  return -1;
}

static pid_t start_ballotd_ex(const char *db_dir, const char *db_sock) {
  unlink(CTL_PATH);

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

  if (wait_for_tcp(TEST_PORT) < 0 || wait_for_path(CTL_PATH) < 0) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  g_ballotd = pid;
  return pid;
}

/* Default fixture: DB deliberately unreachable, for every test that does not
 * itself need a live store (see UNREACH_DB_SOCK above). */
static pid_t start_ballotd(void) { return start_ballotd_ex(UNREACH_DB_DIR, UNREACH_DB_SOCK); }

/* Fixture for the runner-guarded block: the real db_dir/db_sock a live
 * bin/tetrisdb (started by start_runner(), below) is bound to. */
static pid_t start_ballotd_live(void) { return start_ballotd_ex(LIVE_DB_DIR, LIVE_DB_SOCK); }

static int stop_ballotd(pid_t pid) {
  int status = 0;
  if (kill(pid, SIGTERM) < 0) return -1;
  if (waitpid(pid, &status, 0) != pid) return -1;
  g_ballotd = -1;
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

/* ---- voter channel: TCP + tetrissh ---------------------------------------- */

static int voter_connect(session_t *cli) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(TEST_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  if (session_connect(cli, fd, CA_PATH) != SESSION_OK) {
    close(fd);
    return -1;
  }
  return fd;
}

static int voter_send_recv(session_t *cli, const uint8_t *wire, uint32_t wlen, uint8_t *rbuf,
                           uint32_t rcap, htttp_response_t *http) {
  if (session_send(cli, wire, wlen) != SESSION_OK) return -1;
  uint32_t rlen = rcap;
  if (session_recv(cli, rbuf, &rlen) != SESSION_OK) return -1;
  return htttp_parse_response(rbuf, rlen, http) == HTTTP_OK ? 0 : -1;
}

/* One raw LOGIN or REGISTER, answered by ballotd/session.c's auth_login()
 * call - this file has no bcl_ctx (it drives session_t directly), so it
 * cannot reuse libballotclient's bcl_auth() and builds the same wire shape
 * by hand instead. Returns the HTTTP status, or -1 on a transport failure. */
static int voter_auth(session_t *cli, const char *user, const char *method) {
  char body[256];
  int blen = snprintf(body, sizeof body, "%s\n%s", user, TEST_PASSWORD);
  if (blen < 0 || (size_t)blen >= sizeof body) return -1;

  htttp_request_t req;
  memset(&req, 0, sizeof req);
  if (snprintf(req.method, sizeof req.method, "%s", method) >= (int)sizeof req.method) return -1;
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

/* Logs in as `user`, registering first if no such account exists yet -
 * idempotent across repeated `make test` runs against the same shared
 * var/db (see LIVE_DB_DIR's comment above: this suite does not get its own
 * sandboxed user table to wipe between runs). */
static int voter_login_or_register(session_t *cli, const char *user) {
  int status = voter_auth(cli, user, "LOGIN");
  if (status == 200) return 0;
  if (status != 404) return -1;
  status = voter_auth(cli, user, "REGISTER");
  return status == 200 ? 0 : -1;
}

/* voter_connect() plus the real pre-auth exchange every voter-channel op
 * now requires before ballotd will dispatch anything at all. */
static int voter_connect_as(session_t *cli, const char *user) {
  int fd = voter_connect(cli);
  if (fd < 0) return -1;
  if (voter_login_or_register(cli, user) != 0) {
    session_close(cli);
    close(fd);
    return -1;
  }
  return fd;
}

/* ---- admin channel: local AF_UNIX, one-shot per connection ---------------- */

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

/* ---- fixtures --------------------------------------------------------------- */

static bb_config_t valid_config(const char *title) {
  bb_config_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.title, BB_TITLE_LEN, "%s", title);
  snprintf(c.options[0], BB_OPTION_LEN, "Yes");
  snprintf(c.options[1], BB_OPTION_LEN, "No");
  c.option_count = 2;
  snprintf(c.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(c.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");
  return c;
}

/* ---- tests: channel separation ---------------------------------------------- */

/* CREATE is an admin op; sent over the voter TCP+tetrissh channel it must be
 * refused - this is the actual enforcement of "only ballotctl manages
 * elections", not a permission check inside the domain logic. Needs a live
 * store now: every voter-channel request, CREATE included, is unreachable
 * until auth_login()'s pre-auth exchange resolves, and that needs a real
 * account DB - see LIVE_DB_DIR's comment above. */
static int test_voter_handshake_and_wrong_channel_rejected(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  session_t cli;
  int fd = voter_connect_as(&cli, "chanreject");
  CHECK(fd >= 0, "voter handshake or auth failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  req.config = valid_config("Should Be Rejected");

  uint8_t wire[SESSION_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

  uint8_t rbuf[SESSION_MAX_FRAME];
  htttp_response_t http;
  CHECK(voter_send_recv(&cli, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "roundtrip");
  CHECK(http.status == 400, "CREATE over the voter channel must be rejected");

  session_close(&cli);
  close(fd);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* JOIN is a voter op; sent over the admin channel it must be refused too -
 * the same enforcement, the other direction. */
static int test_ctl_rejects_voter_op(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_JOIN;
  snprintf(req.election_id, BB_ID_LEN, "E-100");
  snprintf(req.cert_name, BB_CERT_LEN, "alice");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode JOIN");

  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);
  CHECK(http.status == 400, "JOIN over the admin channel must be rejected");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ---- tests: real dispatch through to libballotbrain, no DB needed ------------- */

/* CREATE with the store unreachable: db_exec's own connect failure must come
 * back as BB_ERR_DB, not a hang or a crash - the same "no socket" contract
 * test_auth.c exercises for LOGIN. Replaces the old
 * "GET_ELECTION is still a stubbed read" case now that db.c talks to a real
 * SocketRunner; see the have_jar()-guarded block below for the case where
 * a store IS reachable. */
static int test_ctl_create_no_db_fails_cleanly(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  req.config = valid_config("Officers 2026");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);

  bcl_response_t resp;
  CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
  CHECK(resp.status == BB_ERR_DB, "CREATE with no reachable store must fail cleanly");
  CHECK(resp.election.id[0] == '\0', "no election id on a failed create");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* There is deliberately no "JOIN with the store unreachable" case here
 * anymore. It used to pair with test_ctl_create_no_db_fails_cleanly above,
 * proving BB_ERR_DB propagates cleanly on the voter channel too - but JOIN
 * is unreachable pre-auth now, and auth itself needs the same store this
 * test wanted absent, so "voter channel, no DB" is no longer a state that
 * exists to test. db.c's BB_ERR_DB handling is already exercised generically
 * by the admin-channel case; nothing voter-specific was left to cover once
 * the premise stopped being reachable. */

/* ---- tests: real dispatch through to libballotbrain, needs a live store ------- */

static int test_ctl_create_ok(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  req.config = valid_config("Officers 2026");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);

  bcl_response_t resp;
  CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
  CHECK(resp.status == BB_OK, "CREATE should succeed");
  CHECK(resp.election.id[0] != '\0', "election id should be set");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Pure-logic path (bb_validate_config), no DB dependency - deterministic
 * today regardless of the frozen DB seam. */
static int test_ctl_create_invalid_config(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  /* title left empty on purpose */
  snprintf(req.config.options[0], BB_OPTION_LEN, "Yes");
  snprintf(req.config.options[1], BB_OPTION_LEN, "No");
  req.config.option_count = 2;
  snprintf(req.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(req.config.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);

  bcl_response_t resp;
  CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
  CHECK(resp.status == BB_ERR_CONFIG_TITLE, "empty title should be refused");
  CHECK(resp.election.id[0] == '\0', "no election id on a failed create");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* A real CREATE followed by a real JOIN through the SAME running daemon:
 * proves the election a CREATE persisted is the one a JOIN reads back, not
 * just that each op individually reaches the store. */
static int test_create_then_join_round_trips(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  int cfd = ctl_connect();
  CHECK(cfd >= 0, "ctl connect failed");

  bcl_request_t creq;
  memset(&creq, 0, sizeof creq);
  creq.op = BCL_CREATE;
  snprintf(creq.cert_name, BB_CERT_LEN, "admin");
  creq.config = valid_config("Round Trip");
  snprintf(creq.config.eligible[0], BB_CERT_LEN, "alice");
  creq.config.eligible_count = 1;

  uint8_t cwire[CTL_MAX_FRAME];
  uint32_t cwlen = sizeof cwire;
  CHECK(bcl_encode_request(&creq, cwire, &cwlen) == 0, "encode CREATE");

  uint8_t crbuf[CTL_MAX_FRAME];
  htttp_response_t chttp;
  CHECK(ctl_send_recv(cfd, cwire, cwlen, crbuf, sizeof crbuf, &chttp) == 0, "ctl roundtrip");
  close(cfd);

  bcl_response_t cresp;
  CHECK(bcl_decode_response(&chttp, &cresp) == 0, "decode CREATE response");
  CHECK(cresp.status == BB_OK, "CREATE should succeed");

  /* JOIN only succeeds once OPEN (bb_join's own gate) - open it over the
   * same admin channel before joining. */
  int ofd = ctl_connect();
  CHECK(ofd >= 0, "ctl connect failed");

  bcl_request_t oreq;
  memset(&oreq, 0, sizeof oreq);
  oreq.op = BCL_OPEN;
  snprintf(oreq.election_id, BB_ID_LEN, "%s", cresp.election.id);
  snprintf(oreq.cert_name, BB_CERT_LEN, "admin");

  uint8_t owire[CTL_MAX_FRAME];
  uint32_t owlen = sizeof owire;
  CHECK(bcl_encode_request(&oreq, owire, &owlen) == 0, "encode OPEN");

  uint8_t orbuf[CTL_MAX_FRAME];
  htttp_response_t ohttp;
  CHECK(ctl_send_recv(ofd, owire, owlen, orbuf, sizeof orbuf, &ohttp) == 0, "ctl roundtrip");
  close(ofd);

  bcl_response_t oresp;
  CHECK(bcl_decode_response(&ohttp, &oresp) == 0, "decode OPEN response");
  CHECK(oresp.status == BB_OK, "OPEN should succeed");

  session_t cli;
  int fd = voter_connect_as(&cli, "alice");
  CHECK(fd >= 0, "voter handshake or auth failed");

  bcl_request_t jreq;
  memset(&jreq, 0, sizeof jreq);
  jreq.op = BCL_JOIN;
  snprintf(jreq.election_id, BB_ID_LEN, "%s", cresp.election.id);
  /* cert_name left blank on purpose: ballotd now overwrites it with the
   * auth_login()-verified identity (see session.c) regardless of what a
   * request carries, so "alice" here would be misleading - it is
   * voter_connect_as()'s job, not this field's, to say who this is. */

  uint8_t jwire[SESSION_MAX_FRAME];
  uint32_t jwlen = sizeof jwire;
  CHECK(bcl_encode_request(&jreq, jwire, &jwlen) == 0, "encode JOIN");

  uint8_t jrbuf[SESSION_MAX_FRAME];
  htttp_response_t jhttp;
  CHECK(voter_send_recv(&cli, jwire, jwlen, jrbuf, sizeof jrbuf, &jhttp) == 0, "roundtrip");

  bcl_response_t jresp;
  CHECK(bcl_decode_response(&jhttp, &jresp) == 0, "decode JOIN response");
  CHECK(jresp.status == BB_OK, "JOIN should find the election CREATE just persisted");
  CHECK(strcmp(jresp.election.id, cresp.election.id) == 0, "JOIN must read back the same election");

  session_close(&cli);
  close(fd);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Two CREATEs, two fresh ctl connections, one daemon: distinct allocated
 * ids proves both hit the SAME admin_thread / bb_ctx, not two independent
 * ones - the property the whole single-admin-thread design exists for. */
static int test_two_creates_share_admin_thread(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  char ids[2][BB_ID_LEN];

  for (int i = 0; i < 2; i++) {
    int fd = ctl_connect();
    CHECK(fd >= 0, "ctl connect failed");

    bcl_request_t req;
    memset(&req, 0, sizeof req);
    req.op = BCL_CREATE;
    snprintf(req.cert_name, BB_CERT_LEN, "admin");
    req.config = valid_config(i == 0 ? "First" : "Second");

    uint8_t wire[CTL_MAX_FRAME];
    uint32_t wlen = sizeof wire;
    CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

    uint8_t rbuf[CTL_MAX_FRAME];
    htttp_response_t http;
    CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
    close(fd);

    bcl_response_t resp;
    CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
    CHECK(resp.status == BB_OK, "CREATE should succeed");
    snprintf(ids[i], BB_ID_LEN, "%s", resp.election.id);
  }

  CHECK(strcmp(ids[0], ids[1]) != 0, "two CREATEs must get distinct ids from one shared bb_ctx");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ---- tests: hostile / malformed input ---------------------------------------- */

static int test_ctl_malformed_http_gets_400(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  const char *junk = "not an htttp request at all";
  CHECK(ctl_frame_write(fd, (const uint8_t *)junk, (uint32_t)strlen(junk)) == 0, "send junk");

  uint8_t rbuf[CTL_MAX_FRAME];
  uint32_t rlen = 0;
  CHECK(ctl_frame_read(fd, rbuf, sizeof rbuf, &rlen) == 0, "expected a reply frame");
  htttp_response_t http;
  CHECK(htttp_parse_response(rbuf, rlen, &http) == HTTTP_OK, "the reply itself must be well-formed");
  CHECK(http.status == 400, "malformed body should get 400");
  close(fd);

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* A frame whose declared length exceeds CTL_MAX_FRAME is refused before any
 * of it is read as HTTTP; the connection is just closed, no reply - the
 * daemon (and ctl_thread) must survive it either way. */
static int test_ctl_oversized_frame_closed_without_reply(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  uint8_t prefix[4] = {0xFF, 0xFF, 0xFF, 0xFF}; /* hand-written: ctl_frame_write
                                                  * would refuse to build this */
  CHECK(write(fd, prefix, sizeof prefix) == (ssize_t)sizeof prefix, "send oversized prefix");

  uint8_t rbuf[CTL_MAX_FRAME];
  uint32_t rlen = 0;
  CHECK(ctl_frame_read(fd, rbuf, sizeof rbuf, &rlen) != 0, "must not reply to an oversized frame");
  close(fd);

  CHECK(stop_ballotd(pid) == 0, "daemon must survive an oversized frame");
  return 0;
}

/* ---- tests: lifecycle --------------------------------------------------------- */

static int test_sigterm_shutdown_idle(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  CHECK(access(CTL_PATH, F_OK) != 0, "ctl socket file should be removed on shutdown");
  return 0;
}

/* A worker (bin/ballot_session) is still attached, mid-handshake-complete
 * but idle, when the daemon is asked to stop - it must still exit cleanly
 * and reap the worker (admin_teardown's kill+reap loop). */
static int test_sigterm_shutdown_with_worker_attached(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  session_t cli;
  int fd = voter_connect(&cli);
  CHECK(fd >= 0, "voter handshake failed");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero with a worker still attached");

  session_close(&cli);
  close(fd);
  return 0;
}

/* ---- fixture: a live SocketRunner --------------------------------------------- */
/*
 * Same three-way split test_auth.c uses, and for the same reason: whether a
 * runner CAN be started (jar built, java on PATH) is a different failure
 * from whether the caller WANTS one (TETRISH_NO_RUNNER), and conflating them
 * is how these tests silently stop running on a push and nobody notices.
 */
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

/* No stop_runner(): unlike every other fixture in this file, this one does
 * not own the runner's lifecycle (see LIVE_DB_DIR's comment - it is the
 * shared project runner, possibly already running before this file ever
 * touched it), so there is nothing for this file to tear down afterward. */

/* Provisions BallotBox's own 6 tables (bin/tetrisdb's own startup policy
 * only provisions TETRISAUTH_DB_TABLE, "user" - not these), then starts the
 * shared runner exactly the way an operator would: `bin/tetrisdb start` is
 * idempotent, so a runner already up (very likely - this is the same
 * runner a manually-run ballotd/ballotctl/ballotu would be using) is left
 * alone rather than fought over. */
static int start_live_runner(void) {
  if (db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELECTION, BB_DB_SCHEMA_ELECTION) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELIGIBLE, BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) != 0 ||
      db_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) != 0) {
    fprintf(stderr, "test_ballotd: fixture: failed to provision tables\n");
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
  fprintf(stderr, "test_ballotd: fixture: shared runner never became reachable\n");
  return -1;
}

/* ---- harness ------------------------------------------------------------------ */

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
    fprintf(stderr, "test_ballotd: %s not built (run make)\n", BALLOTD_BIN);
    return 1;
  }
  if (stat("bin/ballot_session", &st) != 0) {
    fprintf(stderr, "test_ballotd: bin/ballot_session not built (run make)\n");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  printf("ballotd end-to-end tests\n");
  run("ctl channel rejects voter op (JOIN)", test_ctl_rejects_voter_op);
  run("ctl CREATE invalid config refused", test_ctl_create_invalid_config);
  run("ctl CREATE with no reachable store fails cleanly", test_ctl_create_no_db_fails_cleanly);
  run("ctl malformed HTTTP gets 400", test_ctl_malformed_http_gets_400);
  run("ctl oversized frame closed, no reply", test_ctl_oversized_frame_closed_without_reply);
  run("SIGTERM shutdown while idle", test_sigterm_shutdown_idle);
  run("SIGTERM shutdown with a worker attached", test_sigterm_shutdown_with_worker_attached);

  if (!runner_disabled() && have_jar() && have_java()) {
    if (start_live_runner() == 0) {
      run("voter handshake + wrong-channel CREATE rejected (live store)",
          test_voter_handshake_and_wrong_channel_rejected);
      run("ctl CREATE succeeds (live store)", test_ctl_create_ok);
      run("two CREATEs share one admin thread (live store)", test_two_creates_share_admin_thread);
      run("CREATE then JOIN round trips (live store)", test_create_then_join_round_trips);
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
