/*
 * Unit tests for the election lifecycle: the pure transition table
 * (bb_is_legal_transition) and bb_transition_state, which reads the current
 * state back before deciding.
 *
 * Covers TEST.md U-06 (legal chain, including the stored-state postcondition)
 * and U-07 (illegal pairs rejected, state unchanged). The store is substituted,
 * so "the election is currently CLOSED" is simply programmed into the fake.
 */

#include "fake_brain_seams.h"
#include "unity.h"

static bb_ctx *ctx;

void setUp(void) {
  fake_reset();
  ctx = bb_create();
  bb_set_log(ctx, NULL);
}

void tearDown(void) {
  bb_destroy(ctx);
}

/* Program the store with an election sitting in `state`. */
static void stored_in(bb_state_t state) {
  fake_seed_election("E-100", state, 2, NULL, 0);
}

/* U-06: DRAFT->OPEN->CLOSED->PUBLISHED. Each step is legal, is written, and
 * writes exactly the state that was asked for. */
void test_U06_legal_transition_chain(void) {
  const bb_state_t from[] = {BB_STATE_DRAFT, BB_STATE_OPEN, BB_STATE_CLOSED};
  const bb_state_t to[] = {BB_STATE_OPEN, BB_STATE_CLOSED, BB_STATE_PUBLISHED};

  for (int i = 0; i < 3; i++) {
    fake_reset();
    stored_in(from[i]);

    TEST_ASSERT_TRUE(bb_is_legal_transition(from[i], to[i]));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_transition_state(ctx, "E-100", to[i]));

    /* Postcondition: exactly one state write, carrying the target state. */
    TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_UPDATE_STATE));
    const fake_call_t *upd = fake_last(BB_DB_UPDATE_STATE);
    TEST_ASSERT_NOT_NULL(upd);
    TEST_ASSERT_EQUAL_INT(to[i], upd->new_state);
    TEST_ASSERT_EQUAL_STRING("E-100", upd->election_id);
  }
}

/* U-07: one representative per illegal pair is rejected, and nothing is
 * written - the stored state is left exactly as it was. */
void test_U07_illegal_transitions_rejected(void) {
  const bb_state_t from[] = {BB_STATE_PUBLISHED, BB_STATE_DRAFT, BB_STATE_OPEN, BB_STATE_CLOSED};
  const bb_state_t to[] = {BB_STATE_OPEN, BB_STATE_CLOSED, BB_STATE_DRAFT, BB_STATE_OPEN};

  for (int i = 0; i < 4; i++) {
    fake_reset();
    stored_in(from[i]);

    TEST_ASSERT_FALSE(bb_is_legal_transition(from[i], to[i]));
    TEST_ASSERT_EQUAL_INT(BB_ERR_ILLEGAL_TRANSITION, bb_transition_state(ctx, "E-100", to[i]));
    TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  }
}

/* The caller does not get to assert the current state: the transition is
 * decided from what the store holds, closing the check-then-act gap. */
void test_current_state_comes_from_the_store(void) {
  stored_in(BB_STATE_PUBLISHED);
  /* An OPEN request would be legal from DRAFT, but the election is PUBLISHED. */
  TEST_ASSERT_EQUAL_INT(BB_ERR_ILLEGAL_TRANSITION,
                        bb_transition_state(ctx, "E-100", BB_STATE_OPEN));
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_GET_ELECTION));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
}

/* An unknown election is not found, and nothing is written. */
void test_unknown_election_not_found(void) {
  fake.election_present = 0;
  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_transition_state(ctx, "E-999", BB_STATE_OPEN));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
}

/* Guard: PUBLISHED is terminal - no successor is legal. */
void test_published_is_terminal(void) {
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_DRAFT));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_OPEN));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_CLOSED));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_PUBLISHED));
}

/* Guard: a state is never a legal successor of itself (no self-loops). */
void test_no_self_transitions(void) {
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_DRAFT, BB_STATE_DRAFT));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_OPEN, BB_STATE_OPEN));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_CLOSED, BB_STATE_CLOSED));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U06_legal_transition_chain);
  RUN_TEST(test_U07_illegal_transitions_rejected);
  RUN_TEST(test_current_state_comes_from_the_store);
  RUN_TEST(test_unknown_election_not_found);
  RUN_TEST(test_published_is_terminal);
  RUN_TEST(test_no_self_transitions);
  return UNITY_END();
}
