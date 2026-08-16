/*
 * U-21: no ballot-to-voter link in the operation log (secrecy invariant R2).
 *
 * The unit under test here is db_exec itself - the real one, not a substitute -
 * because the rendering of each command into a log line is where the invariant
 * lives. Every op is driven through it with a distinctive cert name present in
 * the command, and the log is then checked for that name.
 *
 * The commands come straight from the seam's own vocabulary rather than from a
 * caller, so this stays a single-function test and stays valid unchanged when
 * SimpleDB replaces the seam body.
 */

#include "libballotbrain/ballotbrain.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

#define SECRET_CERT "ZZ-secret-cert-alice-9f3a"
#define BALLOT_HASH "9c1f7ae4c0de5511"

static bb_ctx *ctx;
static char *log_buf;
static size_t log_size;
static FILE *log_stream;

void setUp(void) {
  ctx = bb_create();
  log_buf = NULL;
  log_size = 0;
  log_stream = open_memstream(&log_buf, &log_size);
  TEST_ASSERT_NOT_NULL(log_stream);
  bb_set_log(ctx, log_stream);
}

void tearDown(void) {
  bb_set_log(ctx, NULL);
  if (log_stream) {
    fclose(log_stream);
  }
  free(log_buf);
  bb_destroy(ctx);
}

/* Every op, each carrying the submitting cert in the command struct. */
static void exec_every_op(void) {
  bb_ballot_hash_t row;
  memset(&row, 0, sizeof(row));
  snprintf(row.hash, BB_HASH_LEN, BALLOT_HASH);
  row.option_index = 1;
  row.version = 1;

  const bb_db_op_t ops[] = {BB_DB_INSERT_ELECTION, BB_DB_UPDATE_STATE,  BB_DB_APPEND_BALLOT,
                            BB_DB_MARK_SUPERSEDED, BB_DB_NONCE_MARK,    BB_DB_GET_ELECTION,
                            BB_DB_GET_TALLY,       BB_DB_GET_HASHES,    BB_DB_FIND_HASH,
                            BB_DB_NONCE_SEEN,      BB_DB_GET_PRIOR_BALLOT};

  for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    bb_db_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = ops[i];
    snprintf(cmd.election_id, BB_ID_LEN, "E-100");
    cmd.new_state = BB_STATE_OPEN;
    snprintf(cmd.hash, BB_HASH_LEN, BALLOT_HASH);
    snprintf(cmd.nonce, BB_NONCE_LEN, "nonce-001");
    snprintf(cmd.cert_name, BB_CERT_LEN, SECRET_CERT);
    cmd.hash_row = &row;

    bb_db_result_t out;
    memset(&out, 0, sizeof(out));
    (void)db_exec(ctx, &cmd, &out);
  }
  fflush(log_stream);
}

void test_U21_no_cert_appears_in_any_log_line(void) {
  exec_every_op();

  /* The ballot hash was logged, so the absence of the cert below is meaningful
   * and not just an empty buffer. */
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_buf, BALLOT_HASH),
                               "expected the ballot hash to appear in the operation log");

  /* No op renders the submitting cert, so no line can pair a voter with a
   * ballot - strictly stronger than "no single line has both". */
  TEST_ASSERT_NULL_MESSAGE(strstr(log_buf, SECRET_CERT),
                           "secrecy violation: submitting cert found in the operation log");
}

/* The one query keyed by voter identity binds the cert as a parameter instead
 * of rendering it, so even the prior-ballot lookup leaks nothing. */
void test_U21_prior_ballot_query_binds_the_cert(void) {
  bb_db_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.op = BB_DB_GET_PRIOR_BALLOT;
  snprintf(cmd.election_id, BB_ID_LEN, "E-100");
  snprintf(cmd.cert_name, BB_CERT_LEN, SECRET_CERT);

  bb_db_result_t out;
  memset(&out, 0, sizeof(out));
  (void)db_exec(ctx, &cmd, &out);
  fflush(log_stream);

  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_buf, "cert=?"),
                               "the prior-ballot query must bind the cert, not render it");
  TEST_ASSERT_NULL(strstr(log_buf, SECRET_CERT));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U21_no_cert_appears_in_any_log_line);
  RUN_TEST(test_U21_prior_ballot_query_binds_the_cert);
  return UNITY_END();
}
