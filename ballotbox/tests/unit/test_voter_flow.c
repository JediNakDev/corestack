/*
 * Unit tests for the voter session flows: bu_join (UC-2) and bu_submit_vote
 * (UC-3 / UC-4). Covers the halves of TEST.md U-35, U-36, U-37, U-38 and U-39
 * that are about what goes on the wire and what the local session ends up
 * holding.
 *
 * The transport and the ballot crypto are substituted (fake_client_seams.h), so
 * "the host is unreachable", "the daemon says not open" and "the cast was
 * accepted with this receipt" are programmed answers. Nothing is sent, no
 * daemon is needed, and none of these tests wait on libtetrissh.
 */

#include "fake_client_seams.h"
#include "unity.h"

#include <string.h>

static bcl_ctx *ctx;
static bu_session_t session;

void setUp(void) {
  fake_client_reset();
  ctx = bcl_create();
  bcl_set_log(ctx, NULL);
  memset(&session, 0, sizeof(session));
}

void tearDown(void) {
  bcl_destroy(ctx);
}

/* A session already admitted to E-100 as alice. */
static void joined_session(void) {
  memset(&session, 0, sizeof(session));
  session.joined = 1;
  snprintf(session.cert_name, BB_CERT_LEN, "alice");
  snprintf(session.election_id, BB_ID_LEN, "E-100");
}

/* ---- UC-2: join -------------------------------------------------------- */

/* U-35: an unreachable host is reported as a timeout, no session state is
 * created, and the client is still usable afterwards. */
void test_U35_join_timeout_creates_no_session(void) {
  fake_client.send_result = BB_ERR_NOT_IMPLEMENTED; /* transport-level failure */

  TEST_ASSERT_EQUAL_INT(BU_JOIN_TIMEOUT, bu_join(ctx, &session, "E-100", "alice"));
  TEST_ASSERT_EQUAL_INT(0, session.joined);
  TEST_ASSERT_EQUAL_STRING("", session.election_id);

  /* Still usable: a second attempt that succeeds admits the voter. */
  fake_client.send_result = BB_OK;
  fake_client.response.status = BB_OK;
  fake_client.response.election.state = BB_STATE_OPEN;
  TEST_ASSERT_EQUAL_INT(BU_JOIN_ADMITTED, bu_join(ctx, &session, "E-100", "alice"));
  TEST_ASSERT_EQUAL_INT(1, session.joined);
}

/* Regression: ballotu.c's real call site is bu_join(ctx, &session, id,
 * session.cert_name) - the cert_name argument aliases the very struct
 * bu_join's ADMITTED case memsets. A join that reads its own source string
 * after clearing it must still end up with the right value, not "". */
void test_join_survives_cert_name_aliasing_session(void) {
  snprintf(session.cert_name, BB_CERT_LEN, "alice");
  fake_client.response.status = BB_OK;
  fake_client.response.election.state = BB_STATE_OPEN;

  TEST_ASSERT_EQUAL_INT(BU_JOIN_ADMITTED, bu_join(ctx, &session, "E-100", session.cert_name));
  TEST_ASSERT_EQUAL_STRING("alice", session.cert_name);
}

/* U-36: a non-open election is reported as not-open and is still remembered
 * locally (UC-2 alt flow 4a), but the voter is not joined to it. */
void test_U36_not_open_election_recorded_locally(void) {
  fake_client.response.status = BB_OK;
  fake_client.response.election.state = BB_STATE_PUBLISHED;

  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_OPEN, bu_join(ctx, &session, "E-042", "alice"));
  TEST_ASSERT_EQUAL_INT(0, session.joined);
  TEST_ASSERT_EQUAL_STRING("E-042", session.election_id);
}

/* A refused voter gets no session at all, and the request that was sent
 * carried the id and cert the caller asked for. */
void test_join_refusals_leave_no_session(void) {
  const bb_result_t refusals[] = {BB_ERR_NOT_FOUND, BB_ERR_NOT_ELIGIBLE, BB_ERR_CERT_EXPIRED};
  const bu_join_outcome_t expected[] = {BU_JOIN_NOT_FOUND, BU_JOIN_NOT_ELIGIBLE,
                                        BU_JOIN_NOT_ELIGIBLE};

  for (int i = 0; i < 3; i++) {
    fake_client_reset();
    memset(&session, 0, sizeof(session));
    fake_client.response.status = refusals[i];

    TEST_ASSERT_EQUAL_INT(expected[i], bu_join(ctx, &session, "E-100", "mallory"));
    TEST_ASSERT_EQUAL_INT(0, session.joined);
    TEST_ASSERT_EQUAL_STRING("", session.election_id);

    const bcl_request_t *sent = fake_client_last_send();
    TEST_ASSERT_NOT_NULL(sent);
    TEST_ASSERT_EQUAL_INT(BCL_JOIN, sent->op);
    TEST_ASSERT_EQUAL_STRING("E-100", sent->election_id);
    TEST_ASSERT_EQUAL_STRING("mallory", sent->cert_name);
  }
}

/* Admission starts a fresh session: joining a second election does not carry
 * the previous election's ballot state over. */
void test_admission_resets_the_session(void) {
  joined_session();
  session.has_ballot = 1;
  session.ballot_version = 3;
  snprintf(session.my_hash, BB_HASH_LEN, "stale-hash");

  fake_client.response.status = BB_OK;
  fake_client.response.election.state = BB_STATE_OPEN;
  TEST_ASSERT_EQUAL_INT(BU_JOIN_ADMITTED, bu_join(ctx, &session, "E-200", "alice"));

  TEST_ASSERT_EQUAL_STRING("E-200", session.election_id);
  TEST_ASSERT_EQUAL_INT(0, session.has_ballot);
  TEST_ASSERT_EQUAL_INT(0, session.ballot_version);
  TEST_ASSERT_EQUAL_STRING("", session.my_hash);
}

/* Regression: a rejoin (new process, or Join picked again mid-session) used
 * to always leave has_ballot/ballot_version at the ADMITTED case's own
 * memset, regardless of whether this cert already had a ballot - so
 * bu_route_vote always chose BU_CAST and a returning voter's next vote
 * silently overwrote their receipt instead of superseding it (UC-4). The
 * server now reports this at JOIN time (has_prior_ballot/prior_ballot_
 * version on bcl_response_t, populated from the store's own GET_PRIOR_
 * BALLOT record - see bb_join, handlers.c); this is bu_join's half of the
 * fix, that the session actually picks the report up. The no-prior-ballot
 * case is covered by test_admission_resets_the_session above, since
 * fake_client_reset() leaves has_prior_ballot at its zero default. */
void test_join_reports_prior_ballot_into_session(void) {
  fake_client.response.status = BB_OK;
  fake_client.response.election.state = BB_STATE_OPEN;
  fake_client.response.has_prior_ballot = 1;
  fake_client.response.prior_ballot_version = 3;

  TEST_ASSERT_EQUAL_INT(BU_JOIN_ADMITTED, bu_join(ctx, &session, "E-100", "alice"));
  TEST_ASSERT_EQUAL_INT(1, session.has_ballot);
  TEST_ASSERT_EQUAL_INT(3, session.ballot_version);
}

/* ---- UC-3 / UC-4: submit ----------------------------------------------- */

/* U-37 (rule 1): voting before joining is refused locally - nothing is
 * encrypted and nothing is sent. */
void test_U37_vote_before_join_sends_nothing(void) {
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_JOINED, bu_submit_vote(ctx, &session, 1, "nonce-1", NULL));
  TEST_ASSERT_EQUAL_INT(0, fake_client.send_count);
  TEST_ASSERT_EQUAL_INT(0, fake_client.encrypt_calls);
}

/* U-38 (rule 3): joined with no prior ballot takes the cast flow, and the
 * receipt lands in the session. */
void test_U38_cast_flow_records_receipt(void) {
  joined_session();
  fake_client.response.status = BB_OK;
  snprintf(fake_client.response.receipt.hash, BB_HASH_LEN, "receipt-v1");

  bb_receipt_t receipt;
  memset(&receipt, 0, sizeof(receipt));
  TEST_ASSERT_EQUAL_INT(BB_OK, bu_submit_vote(ctx, &session, 2, "nonce-1", &receipt));

  const bcl_request_t *sent = fake_client_last_send();
  TEST_ASSERT_NOT_NULL(sent);
  TEST_ASSERT_EQUAL_INT(BCL_CAST, sent->op);
  TEST_ASSERT_EQUAL_STRING("E-100", sent->election_id);
  /* The selection was encrypted before it was sent - the raw option never
   * appears as plaintext in the request beyond the ciphertext buffer. */
  TEST_ASSERT_EQUAL_INT(2, fake_client.last_encrypt_option);
  TEST_ASSERT_EQUAL_STRING("nonce-1", sent->ballot.nonce);

  TEST_ASSERT_EQUAL_INT(1, session.has_ballot);
  TEST_ASSERT_EQUAL_INT(1, session.ballot_version);
  TEST_ASSERT_EQUAL_STRING("receipt-v1", session.my_hash);
  TEST_ASSERT_EQUAL_STRING("receipt-v1", receipt.hash);
}

/* U-39 (rule 5): joined with a prior ballot takes the update flow, and the
 * version advances with a fresh receipt. */
void test_U39_update_flow_advances_version(void) {
  joined_session();
  session.has_ballot = 1;
  session.ballot_version = 1;
  snprintf(session.my_hash, BB_HASH_LEN, "receipt-v1");

  fake_client.response.status = BB_OK;
  snprintf(fake_client.response.receipt.hash, BB_HASH_LEN, "receipt-v2");

  TEST_ASSERT_EQUAL_INT(BB_OK, bu_submit_vote(ctx, &session, 0, "nonce-2", NULL));

  TEST_ASSERT_EQUAL_INT(BCL_UPDATE, fake_client_last_send()->op);
  TEST_ASSERT_EQUAL_INT(2, session.ballot_version);
  TEST_ASSERT_EQUAL_STRING("receipt-v2", session.my_hash);
}

/* A refused submission (the election closed under the voter) leaves the local
 * session exactly as it was. */
void test_refused_submission_does_not_move_the_session(void) {
  joined_session();
  fake_client.response.status = BB_ERR_CLOSED;

  TEST_ASSERT_EQUAL_INT(BB_ERR_CLOSED, bu_submit_vote(ctx, &session, 1, "nonce-1", NULL));
  TEST_ASSERT_EQUAL_INT(0, session.has_ballot);
  TEST_ASSERT_EQUAL_INT(0, session.ballot_version);
  TEST_ASSERT_EQUAL_STRING("", session.my_hash);
}

/* An encryption failure stops the submission before the transport. */
void test_encrypt_failure_sends_nothing(void) {
  joined_session();
  fake_client.encrypt_result = BB_ERR_BAD_OPTION;

  TEST_ASSERT_EQUAL_INT(BB_ERR_BAD_OPTION, bu_submit_vote(ctx, &session, 99, "nonce-1", NULL));
  TEST_ASSERT_EQUAL_INT(0, fake_client.send_count);
  TEST_ASSERT_EQUAL_INT(0, session.has_ballot);
}

static int barrier_calls;

static void observe_encrypted_before_send(void *arg) {
  int *observed = arg;
  barrier_calls++;
  *observed = fake_client.encrypt_calls == 1 && fake_client.send_count == 0;
}

void test_submission_barrier_runs_after_encryption_before_transport(void) {
  joined_session();
  int observed = 0;
  bu_set_before_submit(ctx, observe_encrypted_before_send, &observed);

  TEST_ASSERT_EQUAL_INT(BB_OK, bu_submit_vote(ctx, &session, 1, "nonce-race", NULL));
  TEST_ASSERT_EQUAL_INT(1, barrier_calls);
  TEST_ASSERT_TRUE(observed);
  TEST_ASSERT_EQUAL_INT(1, fake_client.send_count);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U35_join_timeout_creates_no_session);
  RUN_TEST(test_join_survives_cert_name_aliasing_session);
  RUN_TEST(test_U36_not_open_election_recorded_locally);
  RUN_TEST(test_join_refusals_leave_no_session);
  RUN_TEST(test_admission_resets_the_session);
  RUN_TEST(test_join_reports_prior_ballot_into_session);
  RUN_TEST(test_U37_vote_before_join_sends_nothing);
  RUN_TEST(test_U38_cast_flow_records_receipt);
  RUN_TEST(test_U39_update_flow_advances_version);
  RUN_TEST(test_refused_submission_does_not_move_the_session);
  RUN_TEST(test_encrypt_failure_sends_nothing);
  RUN_TEST(test_submission_barrier_runs_after_encryption_before_transport);
  return UNITY_END();
}
