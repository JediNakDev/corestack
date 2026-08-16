/*
 * Unit tests for bb_join (UC-2) - the daemon's admission decision.
 * Covers TEST.md U-09 (not found), U-10 (not open), U-11 (unlisted cert),
 * U-12 (invalid / expired cert) and U-13 (eligible voter admitted).
 *
 * Both of bb_join's collaborators are substituted: the store answers
 * GET_ELECTION with whatever this test programmed, and the PKI seam answers
 * bb_verify_cert with whatever status the case needs. That is what makes U-12
 * - an EXPIRED and an INVALID certificate - testable with no PKI in the repo:
 * the unit under test is bb_join's decision, not X.509 verification.
 *
 * "No session created" is asserted as "no write reached the store": a session
 * is the only thing join would persist, so zero writes is the postcondition.
 */

#include "fake_brain_seams.h"
#include "unity.h"

#include <string.h>

static bb_ctx *ctx;

static const char *const ELIGIBLE[] = {"alice", "bob"};

void setUp(void) {
  fake_reset();
  ctx = bb_create();
  bb_set_log(ctx, NULL);
}

void tearDown(void) {
  bb_destroy(ctx);
}

/* An election in `state` with alice and bob on the eligible list. */
static void stored_election(bb_state_t state) {
  fake_seed_election("E-100", state, 3, ELIGIBLE, 2);
}

/* U-09: an unknown election id is refused as not found. */
void test_U09_election_not_found(void) {
  fake.election_present = 0;

  bb_election_t out;
  memset(&out, 0xAA, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_join(ctx, "E-999", "alice", &out, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
}

/* U-10: an eligible voter is still refused when the election is not OPEN -
 * DRAFT, CLOSED and PUBLISHED each get the same specific refusal. */
void test_U10_election_not_open(void) {
  const bb_state_t states[] = {BB_STATE_DRAFT, BB_STATE_CLOSED, BB_STATE_PUBLISHED};

  for (int i = 0; i < 3; i++) {
    fake_reset();
    stored_election(states[i]);

    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_NOT_OPEN, bb_join(ctx, "E-100", "alice", NULL, NULL, NULL),
                                  bb_state_str(states[i]));
    TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  }
}

/* U-11: a valid cert that is not on the eligible list is refused. */
void test_U11_unlisted_cert_refused(void) {
  stored_election(BB_STATE_OPEN);
  fake.cert_status = BB_CERT_VALID;

  bb_election_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_ELIGIBLE, bb_join(ctx, "E-100", "mallory", &out, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  /* Nothing about the election is handed back to a refused voter. */
  TEST_ASSERT_EQUAL_STRING("", out.id);
}

/* U-12: the cert seam's verdict is authoritative - EXPIRED and INVALID are
 * refused with their own codes, even for a cert on the eligible list. */
void test_U12_invalid_or_expired_cert_refused(void) {
  const bb_cert_status_t status[] = {BB_CERT_EXPIRED, BB_CERT_INVALID, BB_CERT_NOT_ELIGIBLE};
  const bb_result_t expected[] = {BB_ERR_CERT_EXPIRED, BB_ERR_CERT_INVALID, BB_ERR_NOT_ELIGIBLE};

  for (int i = 0; i < 3; i++) {
    fake_reset();
    stored_election(BB_STATE_OPEN);
    fake.cert_status = status[i];

    TEST_ASSERT_EQUAL_INT(expected[i], bb_join(ctx, "E-100", "alice", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, fake.verify_cert_calls);
    TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  }
}

/* U-13: a verified, listed cert on an OPEN election is admitted and gets the
 * election config back. */
void test_U13_eligible_voter_admitted(void) {
  stored_election(BB_STATE_OPEN);

  bb_election_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_join(ctx, "E-100", "alice", &out, NULL, NULL));
  TEST_ASSERT_EQUAL_STRING("E-100", out.id);
  TEST_ASSERT_EQUAL_INT(BB_STATE_OPEN, out.state);
  TEST_ASSERT_EQUAL_INT(3, out.option_count);

  /* bob, the other listed voter, is admitted too. */
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_join(ctx, "E-100", "bob", NULL, NULL, NULL));
}

/* A rejoin (new process, or Join picked again mid-session) must report an
 * existing ballot from the store rather than let the caller assume it is
 * this voter's first vote - the bug this covers: a voter who logged out and
 * back in, then joined the same election again, saw Cast Vote silently
 * behave like a first-time vote instead of being routed to Update. */
void test_join_reports_prior_ballot_from_store(void) {
  stored_election(BB_STATE_OPEN);

  int has_ballot = -1, version = -1;
  fake.prior_found = 0;
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_join(ctx, "E-100", "alice", NULL, &has_ballot, &version));
  TEST_ASSERT_EQUAL_INT(0, has_ballot);
  TEST_ASSERT_EQUAL_INT(0, version);
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_GET_PRIOR_BALLOT));

  fake.prior_found = 1;
  fake.prior_row.version = 2;
  has_ballot = -1;
  version = -1;
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_join(ctx, "E-100", "alice", NULL, &has_ballot, &version));
  TEST_ASSERT_EQUAL_INT(1, has_ballot);
  TEST_ASSERT_EQUAL_INT(2, version);

  /* Both out-params are independently optional - callers that pass NULL for
   * one must not crash and must not skip the lookup for the other. */
  version = -1;
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_join(ctx, "E-100", "alice", NULL, NULL, &version));
  TEST_ASSERT_EQUAL_INT(2, version);

  /* A refused join (not eligible, not open, ...) never reaches the prior-
   * ballot lookup - nothing to report to a voter who was not admitted. */
  fake_reset();
  stored_election(BB_STATE_OPEN);
  has_ballot = -1;
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_ELIGIBLE,
                        bb_join(ctx, "E-100", "mallory", NULL, &has_ballot, NULL));
  TEST_ASSERT_EQUAL_INT(-1, has_ballot);
  TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_GET_PRIOR_BALLOT));
}

/* A bad cert is refused before the election's state is considered, so a
 * refused voter cannot tell an unopened election from a closed one. */
void test_cert_is_checked_before_state(void) {
  stored_election(BB_STATE_CLOSED);
  fake.cert_status = BB_CERT_INVALID;
  TEST_ASSERT_EQUAL_INT(BB_ERR_CERT_INVALID, bb_join(ctx, "E-100", "alice", NULL, NULL, NULL));
}

/* A store failure surfaces as itself, never as "not found". */
void test_store_failure_is_propagated(void) {
  fake.fail_armed = 1;
  fake.fail_op = BB_DB_GET_ELECTION;
  fake.fail_code = BB_ERR_DB;
  TEST_ASSERT_EQUAL_INT(BB_ERR_DB, bb_join(ctx, "E-100", "alice", NULL, NULL, NULL));
}

/* A store failure in the prior-ballot lookup itself (distinct from
 * GET_ELECTION failing) also surfaces as itself - admission already
 * succeeded, so this is the one way BB_OK's usual "nothing left to fail"
 * assumption does not hold for bb_join. */
void test_store_failure_in_prior_ballot_lookup_is_propagated(void) {
  stored_election(BB_STATE_OPEN);
  fake.fail_armed = 1;
  fake.fail_op = BB_DB_GET_PRIOR_BALLOT;
  fake.fail_code = BB_ERR_DB;
  int has_ballot = -1;
  TEST_ASSERT_EQUAL_INT(BB_ERR_DB, bb_join(ctx, "E-100", "alice", NULL, &has_ballot, NULL));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U09_election_not_found);
  RUN_TEST(test_U10_election_not_open);
  RUN_TEST(test_U11_unlisted_cert_refused);
  RUN_TEST(test_U12_invalid_or_expired_cert_refused);
  RUN_TEST(test_U13_eligible_voter_admitted);
  RUN_TEST(test_join_reports_prior_ballot_from_store);
  RUN_TEST(test_cert_is_checked_before_state);
  RUN_TEST(test_store_failure_is_propagated);
  RUN_TEST(test_store_failure_in_prior_ballot_lookup_is_propagated);
  return UNITY_END();
}
