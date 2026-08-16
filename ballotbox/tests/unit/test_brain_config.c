/*
 * Unit tests for bb_validate_config (UC-1) - the pure config validator.
 * Covers TEST.md U-01..U-05.
 *
 * bb_validate_config is pure, so these assert on its return value alone. The
 * matching postconditions - "election exists in DRAFT" / "no election created"
 * - belong to bb_create_election and are asserted in test_brain_create.c.
 *
 * Caveat (U-04/U-05): bb_validate_config compares open_time/close_time with
 * strcmp - lexicographic ordering. Fixtures MUST use fixed-width ISO-8601 or
 * the comparison is meaningless. This is intentional until the daemon gets a
 * real clock/parse.
 */

#include "libballotbrain/ballotbrain.h"
#include "unity.h"

#include <string.h>

/* Fixed-width ISO-8601 instants, one second / one hour apart, so strcmp orders
 * them the same way a real clock would. */
#define T_OPEN      "2026-01-01T00:00:00Z"
#define T_PLUS_1S   "2026-01-01T00:00:01Z"
#define T_PLUS_1H   "2026-01-01T01:00:00Z"
#define T_MINUS_1S  "2025-12-31T23:59:59Z"

/* A minimal otherwise-valid config: title, 2 options, close = open + 1h.
 * Individual tests mutate one field to probe one boundary. */
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

void setUp(void) {}
void tearDown(void) {}

/* U-01: valid config - 2 options (min valid), close = open + 1h -> BB_OK. */
void test_U01_valid_config_accepted(void) {
  bb_config_t c = base_config();
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_validate_config(&c));
}

/* U-02: option count below the min of 2 - 1 option, then 0 options. */
void test_U02_option_count_below_boundary(void) {
  bb_config_t c = base_config();
  c.option_count = 1;
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_OPTIONS, bb_validate_config(&c));
  c.option_count = 0;
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_OPTIONS, bb_validate_config(&c));
}

/* U-03: empty title rejected. */
void test_U03_empty_title_rejected(void) {
  bb_config_t c = base_config();
  c.title[0] = '\0';
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TITLE, bb_validate_config(&c));
}

/* U-04: time-window lower boundary - close == open, then close == open - 1s.
 * Both are non-positive windows and must be rejected. */
void test_U04_time_window_boundary_rejected(void) {
  bb_config_t c = base_config();
  snprintf(c.close_time, BB_TIME_LEN, T_OPEN); /* close == open */
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TIME, bb_validate_config(&c));
  snprintf(c.close_time, BB_TIME_LEN, T_MINUS_1S); /* close == open - 1s */
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TIME, bb_validate_config(&c));
}

/* U-05: minimum valid window - close == open + 1s -> BB_OK. */
void test_U05_minimum_valid_window_accepted(void) {
  bb_config_t c = base_config();
  snprintf(c.close_time, BB_TIME_LEN, T_PLUS_1S);
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_validate_config(&c));
}

/* Validation order guard: title is checked before options before time, so a
 * config that fails multiple rules reports the first (title). Keeps the
 * specific-error contract honest. */
void test_validation_order_title_first(void) {
  bb_config_t c = base_config();
  c.title[0] = '\0';
  c.option_count = 0;
  snprintf(c.close_time, BB_TIME_LEN, T_MINUS_1S);
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TITLE, bb_validate_config(&c));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U01_valid_config_accepted);
  RUN_TEST(test_U02_option_count_below_boundary);
  RUN_TEST(test_U03_empty_title_rejected);
  RUN_TEST(test_U04_time_window_boundary_rejected);
  RUN_TEST(test_U05_minimum_valid_window_accepted);
  RUN_TEST(test_validation_order_title_first);
  return UNITY_END();
}
