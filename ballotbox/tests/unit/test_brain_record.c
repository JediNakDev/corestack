/*
 * Unit tests for bb_record_ballot (UC-3 / UC-4) - the daemon's cast-and-update
 * path. Covers TEST.md U-14 (fresh nonce), U-15 (replay), U-16 (option index
 * boundaries), U-17 (malformed ballot), U-18 (ineligible at record time),
 * U-19 (submission after close), U-20 (no double vote), U-23 (supersede on
 * update) and U-24 (repeated updates).
 *
 * Every collaborator is substituted: the store (GET_ELECTION, NONCE_SEEN,
 * GET_PRIOR_BALLOT and the writes) and the crypto seam (decrypt, receipt).
 * "The nonce was already used", "this voter already has a v2 ballot" and
 * "decryption failed" are all just programmed answers, so nothing here waits on
 * SimpleDB or on real RSA-OAEP.
 *
 * Postconditions are asserted as the exact commands the function issued, since
 * the commands are what a real store would act on.
 */

#include "fake_brain_seams.h"
#include "unity.h"

#include <string.h>

static bb_ctx *ctx;

static const char *const ELIGIBLE[] = {"alice", "bob"};

void setUp(void) {
  fake_reset();
  /* An OPEN 3-option election with alice and bob eligible: the setting for a
   * normal cast. Individual tests change one condition. */
  fake_seed_election("E-100", BB_STATE_OPEN, 3, ELIGIBLE, 2);
  ctx = bb_create();
  bb_set_log(ctx, NULL);
}

void tearDown(void) {
  bb_destroy(ctx);
}

static bb_ballot_t ballot_for(const char *cert, const char *nonce, int option) {
  bb_ballot_t b;
  memset(&b, 0, sizeof(b));
  snprintf(b.cert_name, BB_CERT_LEN, "%s", cert);
  snprintf(b.nonce, BB_NONCE_LEN, "%s", nonce);
  b.payload[0] = (uint8_t)option;
  b.payload_len = 1;
  return b;
}

/* U-14: a ballot with an unused nonce from an eligible voter is accepted, the
 * hash row is appended, and the nonce is consumed. */
void test_U14_fresh_nonce_accepted(void) {
  bb_ballot_t b = ballot_for("alice", "nonce-1", 1);
  bb_receipt_t r;
  memset(&r, 0, sizeof(r));

  TEST_ASSERT_EQUAL_INT(BB_OK, bb_record_ballot(ctx, "E-100", &b, &r));

  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_APPEND_BALLOT));
  const fake_call_t *append = fake_last(BB_DB_APPEND_BALLOT);
  TEST_ASSERT_NOT_NULL(append);
  TEST_ASSERT_EQUAL_STRING("E-100", append->election_id);
  TEST_ASSERT_EQUAL_INT(1, append->row.option_index);
  TEST_ASSERT_EQUAL_INT(1, append->row.version);
  TEST_ASSERT_EQUAL_INT(0, append->row.superseded);
  TEST_ASSERT_EQUAL_STRING(r.hash, append->row.hash);

  /* Nonce consumed, and it is the nonce that was submitted. */
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_NONCE_MARK));
  TEST_ASSERT_EQUAL_STRING("nonce-1", fake_last(BB_DB_NONCE_MARK)->nonce);

  /* A first ballot supersedes nothing. */
  TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_MARK_SUPERSEDED));
}

/* U-15: the same nonce a second time is a replay - rejected, nothing stored. */
void test_U15_replayed_nonce_rejected(void) {
  bb_ballot_t b = ballot_for("alice", "nonce-1", 1);
  bb_receipt_t r;
  memset(&r, 0, sizeof(r));

  /* First submission goes through. */
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_record_ballot(ctx, "E-100", &b, &r));
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_APPEND_BALLOT));

  /* The store has now seen that nonce; the identical resubmission is refused. */
  fake.nonce_seen = 1;
  TEST_ASSERT_EQUAL_INT(BB_ERR_REPLAY, bb_record_ballot(ctx, "E-100", &b, &r));
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_APPEND_BALLOT)); /* still exactly one */
}

/* U-16: option index boundaries for a 3-option election - -1 and 3 are
 * rejected, 0 and 2 are accepted. */
void test_U16_option_index_boundaries(void) {
  const int index[] = {-1, 0, 2, 3};
  const bb_result_t expected[] = {BB_ERR_BAD_OPTION, BB_OK, BB_OK, BB_ERR_BAD_OPTION};

  for (int i = 0; i < 4; i++) {
    fake_reset();
    fake_seed_election("E-100", BB_STATE_OPEN, 3, ELIGIBLE, 2);
    /* Drive the index through the decrypt seam: a real ciphertext could carry
     * any value, including an out-of-range one. */
    fake.decrypt_option_set = 1;
    fake.decrypt_option = index[i];

    bb_ballot_t b = ballot_for("alice", "nonce-x", 0);
    bb_receipt_t r;
    memset(&r, 0, sizeof(r));

    TEST_ASSERT_EQUAL_INT(expected[i], bb_record_ballot(ctx, "E-100", &b, &r));
    TEST_ASSERT_EQUAL_INT(expected[i] == BB_OK ? 1 : 0, fake_count(BB_DB_APPEND_BALLOT));
  }
}

/* U-17: a payload that fails to decrypt is rejected with the decryption error,
 * the store is untouched and the nonce is NOT consumed (so the voter can retry
 * with the same nonce). */
void test_U17_malformed_ballot_rejected(void) {
  fake.decrypt_result = BB_ERR_DECRYPT;

  bb_ballot_t b = ballot_for("alice", "nonce-1", 1);
  bb_receipt_t r;
  memset(&r, 0, sizeof(r));

  TEST_ASSERT_EQUAL_INT(BB_ERR_DECRYPT, bb_record_ballot(ctx, "E-100", &b, &r));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_NONCE_MARK));
  TEST_ASSERT_EQUAL_INT(0, fake.receipt_calls);
}

/* U-18: eligibility is re-checked at record time, so a well-formed ballot from
 * a cert that is not on the list is refused even though it reached this far. */
void test_U18_ineligible_ballot_rejected_at_record(void) {
  bb_ballot_t b = ballot_for("mallory", "nonce-1", 1);
  bb_receipt_t r;
  memset(&r, 0, sizeof(r));

  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_ELIGIBLE, bb_record_ballot(ctx, "E-100", &b, &r));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  /* Refused before decryption: an ineligible ciphertext is never opened. */
  TEST_ASSERT_EQUAL_INT(0, fake.decrypt_calls);
}

/* U-19 (decision-table rules 2 and 4): a ballot arriving after close is
 * rejected, both for a first-time voter and for one with a prior ballot. The
 * prior ballot is left alone. */
void test_U19_submit_after_close_rejected(void) {
  for (int has_prior = 0; has_prior <= 1; has_prior++) {
    fake_reset();
    fake_seed_election("E-100", BB_STATE_CLOSED, 3, ELIGIBLE, 2);
    if (has_prior) {
      fake.prior_found = 1;
      fake.prior_row.version = 1;
      snprintf(fake.prior_row.hash, BB_HASH_LEN, "hash-v1");
    }

    bb_ballot_t b = ballot_for("alice", "nonce-late", 1);
    bb_receipt_t r;
    memset(&r, 0, sizeof(r));

    TEST_ASSERT_EQUAL_INT(BB_ERR_CLOSED, bb_record_ballot(ctx, "E-100", &b, &r));
    TEST_ASSERT_EQUAL_INT(0, fake_write_count());
    /* The prior ballot is neither superseded nor replaced. */
    TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_MARK_SUPERSEDED));
  }
}

/* U-20: two distinct ballots from the same cert do not produce two live
 * ballots - the second is version 2 and supersedes the first, so exactly one
 * counts for that voter. */
void test_U20_no_double_vote(void) {
  bb_ballot_t first = ballot_for("alice", "nonce-1", 1);
  bb_receipt_t r1;
  memset(&r1, 0, sizeof(r1));
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_record_ballot(ctx, "E-100", &first, &r1));

  /* The store now holds alice's v1 ballot. */
  fake.prior_found = 1;
  fake.prior_row.version = 1;
  snprintf(fake.prior_row.hash, BB_HASH_LEN, "%s", r1.hash);

  bb_ballot_t second = ballot_for("alice", "nonce-2", 2);
  bb_receipt_t r2;
  memset(&r2, 0, sizeof(r2));
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_record_ballot(ctx, "E-100", &second, &r2));

  /* Two rows appended, but the first was superseded: one live ballot. */
  TEST_ASSERT_EQUAL_INT(2, fake_count(BB_DB_APPEND_BALLOT));
  TEST_ASSERT_EQUAL_INT(2, fake_last(BB_DB_APPEND_BALLOT)->row.version);
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_MARK_SUPERSEDED));
  TEST_ASSERT_EQUAL_STRING(r1.hash, fake_last(BB_DB_MARK_SUPERSEDED)->hash);
}

/* U-23: an update issues a fresh receipt, distinct from the one it replaces. */
void test_U23_supersede_on_update(void) {
  fake.prior_found = 1;
  fake.prior_row.version = 1;
  snprintf(fake.prior_row.hash, BB_HASH_LEN, "hash-v1");

  bb_ballot_t b = ballot_for("alice", "nonce-2", 0);
  bb_receipt_t r;
  memset(&r, 0, sizeof(r));

  TEST_ASSERT_EQUAL_INT(BB_OK, bb_record_ballot(ctx, "E-100", &b, &r));
  TEST_ASSERT_EQUAL_INT(2, fake_last(BB_DB_APPEND_BALLOT)->row.version);
  TEST_ASSERT_TRUE_MESSAGE(strcmp(r.hash, "hash-v1") != 0, "update must issue a fresh receipt");
  TEST_ASSERT_EQUAL_STRING("hash-v1", fake_last(BB_DB_MARK_SUPERSEDED)->hash);
}

/* U-24: repeated updates keep climbing the version chain, each superseding the
 * version before it, so only the newest ballot is ever live. */
void test_U24_repeated_updates_latest_counts(void) {
  char hashes[3][BB_HASH_LEN];

  for (int v = 1; v <= 3; v++) {
    char nonce[BB_NONCE_LEN];
    snprintf(nonce, BB_NONCE_LEN, "nonce-%d", v);
    bb_ballot_t b = ballot_for("alice", nonce, v % 3);
    bb_receipt_t r;
    memset(&r, 0, sizeof(r));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_record_ballot(ctx, "E-100", &b, &r));
    TEST_ASSERT_EQUAL_INT(v, fake_last(BB_DB_APPEND_BALLOT)->row.version);
    snprintf(hashes[v - 1], BB_HASH_LEN, "%s", r.hash);

    /* Feed this version back as the store's live ballot for the next round. */
    fake.prior_found = 1;
    fake.prior_row.version = v;
    snprintf(fake.prior_row.hash, BB_HASH_LEN, "%s", r.hash);
  }

  /* Every receipt is distinct, and v1 and v2 were each superseded in turn. */
  TEST_ASSERT_TRUE(strcmp(hashes[0], hashes[1]) != 0);
  TEST_ASSERT_TRUE(strcmp(hashes[1], hashes[2]) != 0);
  TEST_ASSERT_EQUAL_INT(2, fake_count(BB_DB_MARK_SUPERSEDED));
  TEST_ASSERT_EQUAL_STRING(hashes[1], fake_last(BB_DB_MARK_SUPERSEDED)->hash);
  TEST_ASSERT_EQUAL_INT(3, fake_count(BB_DB_APPEND_BALLOT));
}

/* A ballot for an election that does not exist is refused, nothing written. */
void test_unknown_election_rejected(void) {
  fake.election_present = 0;
  bb_ballot_t b = ballot_for("alice", "nonce-1", 1);
  bb_receipt_t r;
  memset(&r, 0, sizeof(r));

  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_record_ballot(ctx, "E-999", &b, &r));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U14_fresh_nonce_accepted);
  RUN_TEST(test_U15_replayed_nonce_rejected);
  RUN_TEST(test_U16_option_index_boundaries);
  RUN_TEST(test_U17_malformed_ballot_rejected);
  RUN_TEST(test_U18_ineligible_ballot_rejected_at_record);
  RUN_TEST(test_U19_submit_after_close_rejected);
  RUN_TEST(test_U20_no_double_vote);
  RUN_TEST(test_U23_supersede_on_update);
  RUN_TEST(test_U24_repeated_updates_latest_counts);
  RUN_TEST(test_unknown_election_rejected);
  return UNITY_END();
}
