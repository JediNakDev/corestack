/*
 * Unit tests for bb_create_election (UC-1) - the postcondition half of
 * TEST.md U-01..U-05: does a valid config actually reach the store as a DRAFT
 * election, and does an invalid one leave the store untouched?
 *
 * The store is a substitute (fake_brain_seams.h), so these assert on the exact
 * command the function issued rather than on any real database.
 */

#include "fake_brain_seams.h"
#include "unity.h"

#include <string.h>

#define T_OPEN     "2026-01-01T00:00:00Z"
#define T_PLUS_1S  "2026-01-01T00:00:01Z"
#define T_PLUS_1H  "2026-01-01T01:00:00Z"
#define T_MINUS_1S "2025-12-31T23:59:59Z"

static bb_ctx *ctx;

static bb_config_t base_config(void) {
  bb_config_t c;
  memset(&c, 0, sizeof(c));
  snprintf(c.title, BB_TITLE_LEN, "Officers 2026");
  snprintf(c.options[0], BB_OPTION_LEN, "Alice");
  snprintf(c.options[1], BB_OPTION_LEN, "Bob");
  c.option_count = 2;
  snprintf(c.open_time, BB_TIME_LEN, T_OPEN);
  snprintf(c.close_time, BB_TIME_LEN, T_PLUS_1H);
  return c;
}

void setUp(void) {
  fake_reset();
  ctx = bb_create();
  bb_set_log(ctx, NULL);
}

void tearDown(void) {
  bb_destroy(ctx);
}

/* U-01: a valid config is accepted and persisted as one DRAFT election. */
void test_U01_valid_config_creates_draft_election(void) {
  bb_config_t c = base_config();
  char id[BB_ID_LEN] = {0};

  TEST_ASSERT_EQUAL_INT(BB_OK, bb_create_election(ctx, &c, NULL, id));

  /* Exactly one write, and it is the election insert. */
  TEST_ASSERT_EQUAL_INT(1, fake_write_count());
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_INSERT_ELECTION));

  /* The id handed back is the id that was stored. INSERT_ELECTION is DRAFT by
   * definition of the op - there is no other state an election is created in. */
  const fake_call_t *insert = fake_last(BB_DB_INSERT_ELECTION);
  TEST_ASSERT_NOT_NULL(insert);
  TEST_ASSERT_EQUAL_STRING(id, insert->election_id);
  TEST_ASSERT_GREATER_THAN(0, (int)strlen(id));
}

/* U-05: the minimum valid window (close = open + 1s) is created too. */
void test_U05_minimum_window_creates_election(void) {
  bb_config_t c = base_config();
  snprintf(c.close_time, BB_TIME_LEN, T_PLUS_1S);
  char id[BB_ID_LEN] = {0};

  TEST_ASSERT_EQUAL_INT(BB_OK, bb_create_election(ctx, &c, NULL, id));
  TEST_ASSERT_EQUAL_INT(1, fake_count(BB_DB_INSERT_ELECTION));
}

/* U-02 / U-03 / U-04: every invalid config is refused with its specific error
 * and nothing is written. */
void test_U02_U03_U04_invalid_config_creates_nothing(void) {
  struct {
    const char *label;
    bb_config_t config;
    bb_result_t expected;
  } cases[4];

  cases[0].label = "one option";
  cases[0].config = base_config();
  cases[0].config.option_count = 1;
  cases[0].expected = BB_ERR_CONFIG_OPTIONS;

  cases[1].label = "zero options";
  cases[1].config = base_config();
  cases[1].config.option_count = 0;
  cases[1].expected = BB_ERR_CONFIG_OPTIONS;

  cases[2].label = "empty title";
  cases[2].config = base_config();
  cases[2].config.title[0] = '\0';
  cases[2].expected = BB_ERR_CONFIG_TITLE;

  cases[3].label = "close before open";
  cases[3].config = base_config();
  snprintf(cases[3].config.close_time, BB_TIME_LEN, T_MINUS_1S);
  cases[3].expected = BB_ERR_CONFIG_TIME;

  for (int i = 0; i < 4; i++) {
    fake_reset();
    char id[BB_ID_LEN] = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].expected,
                                  bb_create_election(ctx, &cases[i].config, NULL, id),
                                  cases[i].label);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_write_count(), cases[i].label);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", id, cases[i].label);
  }
}

/* A caller-supplied desired_id is used verbatim when free. */
void test_desired_id_used_when_free(void) {
  bb_config_t c = base_config();
  char id[BB_ID_LEN] = {0};

  TEST_ASSERT_EQUAL_INT(BB_OK, bb_create_election(ctx, &c, "E-777", id));
  TEST_ASSERT_EQUAL_STRING("E-777", id);

  const fake_call_t *insert = fake_last(BB_DB_INSERT_ELECTION);
  TEST_ASSERT_NOT_NULL(insert);
  TEST_ASSERT_EQUAL_STRING("E-777", insert->election_id);
}

/* A caller-supplied desired_id already in use is refused, and nothing is
 * written - same "gate before write" shape as every other config check. */
void test_desired_id_refused_when_taken(void) {
  bb_config_t c = base_config();
  fake_seed_election("E-777", BB_STATE_DRAFT, 2, NULL, 0);
  char id[BB_ID_LEN] = {0};

  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_ID_TAKEN, bb_create_election(ctx, &c, "E-777", id));
  TEST_ASSERT_EQUAL_INT(0, fake_write_count());
  TEST_ASSERT_EQUAL_STRING("", id);
}

/* A store failure on the insert is reported, not swallowed. */
void test_store_failure_is_propagated(void) {
  bb_config_t c = base_config();
  fake.fail_armed = 1;
  fake.fail_op = BB_DB_INSERT_ELECTION;
  fake.fail_code = BB_ERR_DB;

  char id[BB_ID_LEN] = {0};
  TEST_ASSERT_EQUAL_INT(BB_ERR_DB, bb_create_election(ctx, &c, NULL, id));
  TEST_ASSERT_EQUAL_STRING("", id);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U01_valid_config_creates_draft_election);
  RUN_TEST(test_U05_minimum_window_creates_election);
  RUN_TEST(test_U02_U03_U04_invalid_config_creates_nothing);
  RUN_TEST(test_desired_id_used_when_free);
  RUN_TEST(test_desired_id_refused_when_taken);
  RUN_TEST(test_store_failure_is_propagated);
  return UNITY_END();
}
