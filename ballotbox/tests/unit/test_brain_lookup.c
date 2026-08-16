/*
 * Unit tests for bb_lookup_hash (UC-6) - the receipt-hash lookup behind
 * "check your vote". Covers TEST.md U-29 (counted hash found), U-30
 * (superseded hash excluded) and U-31 (unknown hash not found).
 *
 * The store is substituted. Its FIND_HASH contract is "match live rows only",
 * so a superseded hash and a hash that was never issued both come back absent -
 * which is exactly the property U-30 and U-31 are about: the two are
 * indistinguishable to the caller, and neither leaks anything about other
 * ballots.
 *
 * Unlike bb_get_results (UC-5), this is NOT gated on election state - a
 * hash is a secret only its own holder has, so "is my ballot live" leaks
 * nothing about anyone else's regardless of when it's asked. setUp seeds
 * OPEN rather than PUBLISHED specifically to prove that: every case here
 * would behave identically on DRAFT/CLOSED/PUBLISHED too (see
 * test_works_regardless_of_election_state for that claim, checked
 * directly across all four).
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
  fake_seed_election("E-042", BB_STATE_OPEN, 2, ELIGIBLE, 2);
}

void tearDown(void) {
  bb_destroy(ctx);
}

/* U-29: the hash of a live ballot is found, and the choice comes back with
 * it - well before publish (E-042 is OPEN, per setUp). */
void test_U29_counted_hash_found(void) {
  fake.find_found = 1;
  snprintf(fake.find_row.hash, BB_HASH_LEN, "fa15b8bb");
  fake.find_row.option_index = 1;
  fake.find_row.version = 2;
  fake.find_row.superseded = 0;

  bb_ballot_hash_t out;
  memset(&out, 0, sizeof(out));
  char option_name[BB_OPTION_LEN] = {0};
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_lookup_hash(ctx, "E-042", "fa15b8bb", &out, option_name));
  TEST_ASSERT_EQUAL_STRING("fa15b8bb", out.hash);
  TEST_ASSERT_EQUAL_INT(1, out.option_index);
  TEST_ASSERT_EQUAL_INT(0, out.superseded);
  /* option_index 1 on a 2-option "E-042" (fake_seed_election's own naming). */
  TEST_ASSERT_EQUAL_STRING("option-1", option_name);

  /* The query went to the store with the hash the voter asked about. */
  const fake_call_t *find = fake_last(BB_DB_FIND_HASH);
  TEST_ASSERT_NOT_NULL(find);
  TEST_ASSERT_EQUAL_STRING("fa15b8bb", find->hash);
  TEST_ASSERT_EQUAL_STRING("E-042", find->election_id);
}

/* U-30: a superseded version's hash is not in the live set, so the lookup is
 * a miss - the client's dropped-ballot path. */
void test_U30_superseded_hash_excluded(void) {
  fake.find_found = 0; /* the live-rows-only query does not match it */

  bb_ballot_hash_t out;
  memset(&out, 0xAA, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_lookup_hash(ctx, "E-042", "hash-of-v1", &out, NULL));
}

/* U-31: a hash that was never issued is a miss too, and the answer is
 * byte-for-byte the same one a superseded hash gets. */
void test_U31_unknown_hash_not_found(void) {
  fake.find_found = 0;

  bb_ballot_hash_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_lookup_hash(ctx, "E-042", "0000deadbeef", &out, NULL));
  /* Nothing was written into the caller's buffer: no other ballot is revealed. */
  TEST_ASSERT_EQUAL_STRING("", out.hash);
  TEST_ASSERT_EQUAL_INT(0, out.option_index);
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
}

/* The whole point of dropping the PUBLISHED gate: a live hash is found the
 * same way in every election state, not just OPEN (setUp's default) or
 * PUBLISHED. */
void test_works_regardless_of_election_state(void) {
  const bb_state_t states[] = {BB_STATE_DRAFT, BB_STATE_OPEN, BB_STATE_CLOSED,
                               BB_STATE_PUBLISHED};

  for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
    fake_seed_election("E-042", states[i], 2, ELIGIBLE, 2);
    fake.find_found = 1;
    snprintf(fake.find_row.hash, BB_HASH_LEN, "fa15b8bb");
    fake.find_row.option_index = 0;

    bb_ballot_hash_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, bb_lookup_hash(ctx, "E-042", "fa15b8bb", &out, NULL),
                                  "state should not gate the lookup");
  }
}

/* An election id nobody created is not found - same answer a dropped ballot
 * gets, so this cannot be used to enumerate which elections exist either. */
void test_unknown_election_not_found(void) {
  bb_ballot_hash_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_lookup_hash(ctx, "E-999", "fa15b8bb", &out, NULL));
}

/* A store failure is reported as itself, never as a dropped ballot - the voter
 * must not be told their vote is missing when the lookup simply failed. */
void test_store_failure_is_not_a_miss(void) {
  fake.fail_armed = 1;
  fake.fail_op = BB_DB_FIND_HASH;
  fake.fail_code = BB_ERR_DB;

  bb_ballot_hash_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_DB, bb_lookup_hash(ctx, "E-042", "fa15b8bb", &out, NULL));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U29_counted_hash_found);
  RUN_TEST(test_U30_superseded_hash_excluded);
  RUN_TEST(test_U31_unknown_hash_not_found);
  RUN_TEST(test_works_regardless_of_election_state);
  RUN_TEST(test_unknown_election_not_found);
  RUN_TEST(test_store_failure_is_not_a_miss);
  return UNITY_END();
}
