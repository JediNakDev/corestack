/*
 * Unit tests for bb_check_eligibility (UC-2) - the pure eligible-list scan.
 * Covers TEST.md U-11 (partial) and U-13 (partial), plus the full eligibility
 * partition as a decision-table sweep.
 *
 * Only the pure eligibility unit is exercised here. The surrounding join-refusal
 * / admission paths (U-11/U-13 full form) need the stored election via DB
 * readback and are deferred (TEST.md section 4).
 */

#include "libballotbrain/ballotbrain.h"
#include "unity.h"

#include <string.h>

/* An OPEN election with two eligible certs. */
static bb_election_t election_with_eligibles(void) {
  bb_election_t e;
  memset(&e, 0, sizeof(e));
  e.state = BB_STATE_OPEN;
  snprintf(e.eligible[0], BB_CERT_LEN, "alice");
  snprintf(e.eligible[1], BB_CERT_LEN, "bob");
  e.eligible_count = 2;
  return e;
}

void setUp(void) {}
void tearDown(void) {}

/* U-11: a cert not on the eligible list is refused (0). */
void test_U11_unlisted_cert_not_eligible(void) {
  bb_election_t e = election_with_eligibles();
  TEST_ASSERT_EQUAL_INT(0, bb_check_eligibility(&e, "carol"));
}

/* U-13: a cert on the eligible list is admitted (1). */
void test_U13_listed_cert_eligible(void) {
  bb_election_t e = election_with_eligibles();
  TEST_ASSERT_EQUAL_INT(1, bb_check_eligibility(&e, "alice"));
  TEST_ASSERT_EQUAL_INT(1, bb_check_eligibility(&e, "bob"));
}

/* Full partition sweep of the eligible-list predicate. */
void test_eligibility_partition_table(void) {
  bb_election_t e = election_with_eligibles();

  /* present (first and last entry) */
  TEST_ASSERT_EQUAL_INT(1, bb_check_eligibility(&e, "alice"));
  TEST_ASSERT_EQUAL_INT(1, bb_check_eligibility(&e, "bob"));
  /* absent */
  TEST_ASSERT_EQUAL_INT(0, bb_check_eligibility(&e, "mallory"));
  /* empty string is not a listed cert */
  TEST_ASSERT_EQUAL_INT(0, bb_check_eligibility(&e, ""));
  /* NULL cert / NULL election are safely non-eligible */
  TEST_ASSERT_EQUAL_INT(0, bb_check_eligibility(&e, NULL));
  TEST_ASSERT_EQUAL_INT(0, bb_check_eligibility(NULL, "alice"));
}

/* An empty eligible list admits no one. */
void test_empty_eligible_list_admits_nobody(void) {
  bb_election_t e;
  memset(&e, 0, sizeof(e));
  e.state = BB_STATE_OPEN;
  e.eligible_count = 0;
  TEST_ASSERT_EQUAL_INT(0, bb_check_eligibility(&e, "alice"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U11_unlisted_cert_not_eligible);
  RUN_TEST(test_U13_listed_cert_eligible);
  RUN_TEST(test_eligibility_partition_table);
  RUN_TEST(test_empty_eligible_list_admits_nobody);
  return UNITY_END();
}
