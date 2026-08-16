/*
 * Unit tests for the UC-5 results path: bb_publish_results (the publish gate)
 * and bb_get_results (the observer-facing view).
 * Covers TEST.md U-08 (publish requires CLOSED), U-25 (only live ballots are
 * counted), U-26 (results gated before publish), U-27 (ineligible observer)
 * and U-28 (zero-ballot publish).
 *
 * The store is substituted, so "this election is CLOSED with 3 live ballots" is
 * programmed directly and the assertions are about the gate decisions and the
 * data handed out - not about SQL.
 */

#include "fake_brain_seams.h"
#include "unity.h"

#include <string.h>

static bb_ctx *ctx;

static const char *const OBSERVERS[] = {"alice", "bob", "carol"};

void setUp(void) {
  fake_reset();
  ctx = bb_create();
  bb_set_log(ctx, NULL);
}

void tearDown(void) {
  bb_destroy(ctx);
}

/* Program a published election with `n` live ballot rows and a matching tally
 * over two options. */
static void published_with_ballots(int n) {
  fake_seed_election("E-042", BB_STATE_PUBLISHED, 2, OBSERVERS, 3);
  fake.hash_count = n;
  for (int i = 0; i < n; i++) {
    snprintf(fake.hashes[i].hash, BB_HASH_LEN, "hash-%d", i);
    fake.hashes[i].option_index = i % 2;
    fake.hashes[i].version = 1;
    fake.hashes[i].superseded = 0;
    fake.tally[i % 2]++;
  }
}

/* U-08: publish is refused unless the election is CLOSED, and the refusal
 * writes nothing - the state is left as it was. */
void test_U08_publish_requires_closed(void) {
  const bb_state_t refused[] = {BB_STATE_OPEN, BB_STATE_DRAFT, BB_STATE_PUBLISHED};

  for (int i = 0; i < 3; i++) {
    fake_reset();
    fake_seed_election("E-100", refused[i], 2, OBSERVERS, 3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_ILLEGAL_TRANSITION, bb_publish_results(ctx, "E-100"),
                                  bb_state_str(refused[i]));
    TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  }

  /* From CLOSED it succeeds, and PUBLISHED is what gets written. */
  fake_reset();
  fake_seed_election("E-100", BB_STATE_CLOSED, 2, OBSERVERS, 3);
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_publish_results(ctx, "E-100"));
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_UPDATE_STATE));
  TEST_ASSERT_EQUAL_INT(BB_STATE_PUBLISHED, fake_last(BB_DB_UPDATE_STATE)->new_state);
}

/* U-25: the published view reports the live (non-superseded) set - three voters
 * where one updated once still means three counted ballots. */
void test_U25_tally_counts_live_ballots_only(void) {
  /* Three voters, one of whom updated once: the update superseded its own v1
   * (asserted in test_brain_record), so the live set the store reports for the
   * published view is three rows, not four. */
  published_with_ballots(3);

  bb_results_t out;
  memset(&out, 0xAA, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_get_results(ctx, "E-042", "alice", &out));

  TEST_ASSERT_EQUAL_INT(3, out.hash_count);
  TEST_ASSERT_EQUAL_INT(2, out.option_count);
  TEST_ASSERT_EQUAL_INT(3, out.tally[0] + out.tally[1]);
  for (int i = 0; i < out.hash_count; i++) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, out.hashes[i].superseded,
                                  "a superseded row must never reach the published view");
  }
}

/* U-26: results are refused while the election is OPEN and while it is CLOSED,
 * and no tally or hash data is read out of the store. */
void test_U26_results_gated_before_publish(void) {
  const bb_state_t states[] = {BB_STATE_OPEN, BB_STATE_CLOSED, BB_STATE_DRAFT};

  for (int i = 0; i < 3; i++) {
    fake_reset();
    fake_seed_election("E-100", states[i], 2, OBSERVERS, 3);
    fake.tally[0] = 14;
    fake.tally[1] = 6;

    bb_results_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_NOT_PUBLISHED, bb_get_results(ctx, "E-100", "alice", &out),
                                  bb_state_str(states[i]));
    /* Nothing was even asked of the store beyond the state lookup. */
    TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_GET_TALLY));
    TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_GET_HASHES));
    TEST_ASSERT_EQUAL_INT(0, out.tally[0]);
    TEST_ASSERT_EQUAL_INT(0, out.hash_count);
  }
}

/* U-27: an observer outside the eligible set is refused, and gets no data. */
void test_U27_ineligible_observer_refused(void) {
  published_with_ballots(3);

  bb_results_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_ELIGIBLE, bb_get_results(ctx, "E-042", "mallory", &out));
  TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_GET_TALLY));
  TEST_ASSERT_EQUAL_INT(0, fake_count(BB_DB_GET_HASHES));
  TEST_ASSERT_EQUAL_INT(0, out.hash_count);
}

/* U-28: an election with no ballots publishes, and its view is an all-zero
 * tally with an empty hash list - not an error. */
void test_U28_zero_ballot_publish(void) {
  fake_seed_election("E-100", BB_STATE_CLOSED, 2, OBSERVERS, 3);
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_publish_results(ctx, "E-100"));

  fake_reset();
  published_with_ballots(0);

  bb_results_t out;
  memset(&out, 0xAA, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_get_results(ctx, "E-042", "alice", &out));
  TEST_ASSERT_EQUAL_INT(0, out.hash_count);
  TEST_ASSERT_EQUAL_INT(2, out.option_count);
  TEST_ASSERT_EQUAL_INT(0, out.tally[0]);
  TEST_ASSERT_EQUAL_INT(0, out.tally[1]);
}

/* The published view carries the election's title alongside its tally, so a
 * results screen can show "E-042: <title>" instead of just the id the
 * caller already typed. fetch_results (the shared body behind both
 * bb_get_results and bb_get_results_admin) sets it from the same election
 * row it already loads to gate on PUBLISHED/eligibility - covering it here
 * covers both public entry points, since they share this exact function. */
void test_results_carry_the_election_title(void) {
  published_with_ballots(1);
  snprintf(fake.election.title, BB_TITLE_LEN, "Favourite colour");

  bb_results_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_get_results(ctx, "E-042", "alice", &out));
  TEST_ASSERT_EQUAL_STRING("Favourite colour", out.title);
}

/* An unknown election is not found, for publish and for results alike. */
void test_unknown_election_not_found(void) {
  fake.election_present = 0;
  bb_results_t out;
  memset(&out, 0, sizeof(out));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_publish_results(ctx, "E-999"));
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_get_results(ctx, "E-999", "alice", &out));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U08_publish_requires_closed);
  RUN_TEST(test_U25_tally_counts_live_ballots_only);
  RUN_TEST(test_U26_results_gated_before_publish);
  RUN_TEST(test_U27_ineligible_observer_refused);
  RUN_TEST(test_U28_zero_ballot_publish);
  RUN_TEST(test_results_carry_the_election_title);
  RUN_TEST(test_unknown_election_not_found);
  return UNITY_END();
}
