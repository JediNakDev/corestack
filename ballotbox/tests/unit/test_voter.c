/*
 * Unit tests for the pure voter decision functions (ballotu): vote routing,
 * the join classifier, the check classifier, and the placeholder receipt KDF.
 * Covers TEST.md U-33, U-34 (receipt KDF), U-37, U-38, U-39 (vote routing),
 * U-40 (dropped-ballot classification), plus the full bu_classify_join
 * partition. The session-flow halves of U-35..U-39 - what actually goes on the
 * wire and what the session ends up holding - are in test_voter_flow.c.
 *
 * Nothing here needs a seam except bu_derive_receipt, which is exercised as
 * itself. Caveats:
 *   - U-33/U-34 are placeholder-grade: bu_derive_receipt is a djb2-seeded
 *     placeholder, so determinism and distinctness hold, but carry NO
 *     cryptographic collision resistance. Strong guarantees are a crypto-
 *     milestone progression test, not a pass criterion now.
 */

#include "libballotclient/voter.h"
#include "unity.h"

#include <string.h>

static bcl_ctx *ctx;

void setUp(void) {
  ctx = bcl_create();
  bcl_set_log(ctx, NULL); /* silence the crypto/transport placeholder log */
}

void tearDown(void) {
  bcl_destroy(ctx);
}

/* ---- vote routing (decision table, rules 1/3/5) ----------------------- */

/* U-37 (rule 1): not joined -> must join first. */
void test_U37_vote_before_join_blocked(void) {
  bu_session_t s;
  memset(&s, 0, sizeof(s));
  s.joined = 0;
  TEST_ASSERT_EQUAL_INT(BU_MUST_JOIN, bu_route_vote(&s));
  /* A NULL session is treated the same way (defensive). */
  TEST_ASSERT_EQUAL_INT(BU_MUST_JOIN, bu_route_vote(NULL));
}

/* U-38 (rule 3): joined, no prior ballot -> cast. */
void test_U38_cast_flow_selected(void) {
  bu_session_t s;
  memset(&s, 0, sizeof(s));
  s.joined = 1;
  s.has_ballot = 0;
  TEST_ASSERT_EQUAL_INT(BU_CAST, bu_route_vote(&s));
}

/* U-39 (rule 5): joined, prior ballot exists -> update. */
void test_U39_update_flow_selected(void) {
  bu_session_t s;
  memset(&s, 0, sizeof(s));
  s.joined = 1;
  s.has_ballot = 1;
  TEST_ASSERT_EQUAL_INT(BU_UPDATE, bu_route_vote(&s));
}

/* ---- join classifier (UC-2 partitions) -------------------------------- */

/* Full partition table for bu_classify_join (all five UC-2 outcomes). */
void test_classify_join_partition_table(void) {
  bb_election_t open_el;
  memset(&open_el, 0, sizeof(open_el));
  open_el.state = BB_STATE_OPEN;

  bb_election_t closed_el;
  memset(&closed_el, 0, sizeof(closed_el));
  closed_el.state = BB_STATE_CLOSED;

  /* timeout: any transport-level / unmapped failure */
  TEST_ASSERT_EQUAL_INT(BU_JOIN_TIMEOUT, bu_classify_join(BB_ERR_NOT_IMPLEMENTED, NULL));
  TEST_ASSERT_EQUAL_INT(BU_JOIN_TIMEOUT, bu_classify_join(BB_ERR_DB, NULL));

  /* not found */
  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_FOUND, bu_classify_join(BB_ERR_NOT_FOUND, NULL));

  /* not eligible: three refusal codes fold into one outcome */
  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_ELIGIBLE, bu_classify_join(BB_ERR_NOT_ELIGIBLE, NULL));
  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_ELIGIBLE, bu_classify_join(BB_ERR_CERT_INVALID, NULL));
  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_ELIGIBLE, bu_classify_join(BB_ERR_CERT_EXPIRED, NULL));

  /* not open: explicit status, or OK with a non-OPEN election */
  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_OPEN, bu_classify_join(BB_ERR_NOT_OPEN, NULL));
  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_OPEN, bu_classify_join(BB_OK, &closed_el));
  /* OK with a NULL election is not admittable either */
  TEST_ASSERT_EQUAL_INT(BU_JOIN_NOT_OPEN, bu_classify_join(BB_OK, NULL));

  /* admitted: OK and the election is OPEN */
  TEST_ASSERT_EQUAL_INT(BU_JOIN_ADMITTED, bu_classify_join(BB_OK, &open_el));
}

/* ---- check-your-vote classifier (UC-6) -------------------------------- */

/* U-40: a valid key whose hash the daemon cannot find is a dropped ballot, so
 * the voter is sent down the admin escalation path. */
void test_U40_dropped_ballot_flagged(void) {
  /* Both shapes of "not in the tally": an explicit not-found status, and an
   * OK answer that simply did not match. */
  TEST_ASSERT_EQUAL_INT(BU_CHECK_DROPPED, bu_classify_check(BB_ERR_NOT_FOUND, 0));
  TEST_ASSERT_EQUAL_INT(BU_CHECK_DROPPED, bu_classify_check(BB_OK, 0));
}

/* Full partition table for bu_classify_check. */
void test_classify_check_partition_table(void) {
  /* counted */
  TEST_ASSERT_EQUAL_INT(BU_CHECK_COUNTED, bu_classify_check(BB_OK, 1));
  /* dropped */
  TEST_ASSERT_EQUAL_INT(BU_CHECK_DROPPED, bu_classify_check(BB_OK, 0));
  TEST_ASSERT_EQUAL_INT(BU_CHECK_DROPPED, bu_classify_check(BB_ERR_NOT_FOUND, 0));
  /* unavailable: a failed lookup must never be shown as a lost ballot, even if
   * the response buffer happens to say found. */
  TEST_ASSERT_EQUAL_INT(BU_CHECK_UNAVAILABLE, bu_classify_check(BB_ERR_NOT_IMPLEMENTED, 0));
  TEST_ASSERT_EQUAL_INT(BU_CHECK_UNAVAILABLE, bu_classify_check(BB_ERR_NOT_PUBLISHED, 0));
  TEST_ASSERT_EQUAL_INT(BU_CHECK_UNAVAILABLE, bu_classify_check(BB_ERR_DB, 1));
}

/* ---- receipt KDF (placeholder-grade) ---------------------------------- */

/* U-33: the same secret key derives the same receipt hash both times. */
void test_U33_receipt_kdf_deterministic(void) {
  char h1[BB_HASH_LEN];
  char h2[BB_HASH_LEN];
  TEST_ASSERT_EQUAL_INT(BB_OK, bu_derive_receipt(ctx, "voter-secret-key-1", h1));
  TEST_ASSERT_EQUAL_INT(BB_OK, bu_derive_receipt(ctx, "voter-secret-key-1", h2));
  TEST_ASSERT_EQUAL_STRING(h1, h2);
}

/* U-34: two different secret keys derive different hashes (no collision on this
 * small test corpus; placeholder-grade, not a crypto guarantee). */
void test_U34_distinct_keys_distinct_hashes(void) {
  char ha[BB_HASH_LEN];
  char hb[BB_HASH_LEN];
  TEST_ASSERT_EQUAL_INT(BB_OK, bu_derive_receipt(ctx, "key-alpha", ha));
  TEST_ASSERT_EQUAL_INT(BB_OK, bu_derive_receipt(ctx, "key-beta", hb));
  TEST_ASSERT_TRUE_MESSAGE(strcmp(ha, hb) != 0, "distinct keys must derive distinct hashes");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U37_vote_before_join_blocked);
  RUN_TEST(test_U38_cast_flow_selected);
  RUN_TEST(test_U39_update_flow_selected);
  RUN_TEST(test_classify_join_partition_table);
  RUN_TEST(test_U40_dropped_ballot_flagged);
  RUN_TEST(test_classify_check_partition_table);
  RUN_TEST(test_U33_receipt_kdf_deterministic);
  RUN_TEST(test_U34_distinct_keys_distinct_hashes);
  return UNITY_END();
}
