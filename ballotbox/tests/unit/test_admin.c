/*
 * Unit tests for libballotclient admin logic (ballotctl).
 * Not in the TEST.md U-* numbering, but cheap and seam-free:
 *   - bc_build_transition accepts OPEN/CLOSE/PUBLISH and rejects a non-lifecycle
 *     op (e.g. BCL_JOIN) with BB_ERR_ILLEGAL_TRANSITION.
 *   - bc_prevalidate_config / bc_build_create return the SAME specific
 *     BB_ERR_CONFIG_* as the authoritative bb_validate_config, proving the
 *     client pre-check does not drift from the daemon's rules.
 *   - bc_fold_eligible folds an eligible-voter list to lowercase the same
 *     way libtetrisauth folds every real username, dedupes it case-
 *     insensitively, and rejects (without partially mutating) an illegal
 *     entry - the ballotctl UI's own logic, moved here so it is testable at
 *     all (it was a `static` function in ballotctl.c before this).
 */

#include "libballotclient/admin.h"
#include "libballotbrain/ballotbrain.h"
#include "unity.h"

#include <stdarg.h>
#include <string.h>

static bb_config_t valid_config(void) {
  bb_config_t c;
  memset(&c, 0, sizeof(c));
  snprintf(c.title, BB_TITLE_LEN, "Officers 2026");
  snprintf(c.options[0], BB_OPTION_LEN, "Alice");
  snprintf(c.options[1], BB_OPTION_LEN, "Bob");
  c.option_count = 2;
  snprintf(c.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(c.close_time, BB_TIME_LEN, "2026-01-01T01:00:00Z");
  return c;
}

void setUp(void) {}
void tearDown(void) {}

/* ---- bc_build_transition ---------------------------------------------- */

void test_build_transition_accepts_lifecycle_ops(void) {
  const bcl_op_t ops[] = {BCL_OPEN, BCL_CLOSE, BCL_PUBLISH};
  for (int i = 0; i < 3; i++) {
    bcl_request_t req;
    memset(&req, 0xAA, sizeof(req)); /* poison to prove the builder writes it */
    TEST_ASSERT_EQUAL_INT(BB_OK, bc_build_transition(ops[i], "E-100", &req));
    TEST_ASSERT_EQUAL_INT(ops[i], req.op);
    TEST_ASSERT_EQUAL_STRING("E-100", req.election_id);
  }
}

void test_build_transition_rejects_non_lifecycle_op(void) {
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  /* JOIN/CAST/CREATE etc. are not lifecycle transitions. */
  TEST_ASSERT_EQUAL_INT(BB_ERR_ILLEGAL_TRANSITION, bc_build_transition(BCL_JOIN, "E-100", &req));
  TEST_ASSERT_EQUAL_INT(BB_ERR_ILLEGAL_TRANSITION, bc_build_transition(BCL_CREATE, "E-100", &req));
}

/* ---- no rule drift between client pre-check and daemon validator ------- */

void test_prevalidate_matches_brain_validator(void) {
  /* Valid */
  bb_config_t c = valid_config();
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_OK, bc_prevalidate_config(&c));

  /* Empty title */
  c = valid_config();
  c.title[0] = '\0';
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TITLE, bc_prevalidate_config(&c));

  /* Too few options */
  c = valid_config();
  c.option_count = 1;
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_OPTIONS, bc_prevalidate_config(&c));

  /* Non-positive time window (close <= open, lexicographic) */
  c = valid_config();
  snprintf(c.close_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TIME, bc_prevalidate_config(&c));
}

/* ---- bc_build_create --------------------------------------------------- */

void test_build_create_valid_config(void) {
  bb_config_t c = valid_config();
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  TEST_ASSERT_EQUAL_INT(BB_OK, bc_build_create(&c, &req));
  TEST_ASSERT_EQUAL_INT(BCL_CREATE, req.op);
  TEST_ASSERT_EQUAL_INT(2, req.config.option_count);
  TEST_ASSERT_EQUAL_STRING("Officers 2026", req.config.title);
}

void test_build_create_rejects_invalid_with_same_error(void) {
  bb_config_t c = valid_config();
  c.option_count = 0;
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  /* Same specific error as the authoritative validator; no request built. */
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_build_create(&c, &req));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_OPTIONS, bc_build_create(&c, &req));
}

/* ---- bc_fold_eligible --------------------------------------------------- */

static void set_eligible(char out[][BB_CERT_LEN], int *count, ...) {
  /* Small variadic helper so each case reads as its list of names, not a
   * wall of snprintf calls. Terminated by a NULL sentinel. */
  va_list ap;
  va_start(ap, count);
  *count = 0;
  for (;;) {
    const char *name = va_arg(ap, const char *);
    if (name == NULL) break;
    snprintf(out[*count], BB_CERT_LEN, "%s", name);
    (*count)++;
  }
  va_end(ap);
}

void test_fold_eligible_lowercases_every_entry(void) {
  char eligible[BB_MAX_VOTERS][BB_CERT_LEN];
  int count;
  set_eligible(eligible, &count, "Alice", "BOB", "carol", NULL);

  TEST_ASSERT_EQUAL_INT(0, bc_fold_eligible(eligible, &count, NULL, 0));
  TEST_ASSERT_EQUAL_INT(3, count);
  TEST_ASSERT_EQUAL_STRING("alice", eligible[0]);
  TEST_ASSERT_EQUAL_STRING("bob", eligible[1]);
  TEST_ASSERT_EQUAL_STRING("carol", eligible[2]);
}

/* The bug this covers: "Alice, alice, ALICE" used to fold to three identical
 * rows nothing rejected, each silently eating a slot of the BB_MAX_VOTERS
 * cap this same list is parsed under. */
void test_fold_eligible_dedupes_case_insensitively_keeping_first_order(void) {
  char eligible[BB_MAX_VOTERS][BB_CERT_LEN];
  int count;
  set_eligible(eligible, &count, "Alice", "alice", "ALICE", "bob", "Alice", NULL);

  TEST_ASSERT_EQUAL_INT(0, bc_fold_eligible(eligible, &count, NULL, 0));
  TEST_ASSERT_EQUAL_INT(2, count);
  TEST_ASSERT_EQUAL_STRING("alice", eligible[0]);
  TEST_ASSERT_EQUAL_STRING("bob", eligible[1]);
}

void test_fold_eligible_rejects_illegal_name_and_reports_it(void) {
  char eligible[BB_MAX_VOTERS][BB_CERT_LEN];
  int count;
  set_eligible(eligible, &count, "alice", "not a name!", "bob", NULL);

  char bad[BB_CERT_LEN];
  TEST_ASSERT_EQUAL_INT(-1, bc_fold_eligible(eligible, &count, bad, sizeof bad));
  TEST_ASSERT_EQUAL_STRING("not a name!", bad);
  /* Rejected: the list is left exactly as it was typed, not partially
   * folded - the caller re-shows the whole form, not a half-fixed one. */
  TEST_ASSERT_EQUAL_INT(3, count);
  TEST_ASSERT_EQUAL_STRING("alice", eligible[0]);
}

/* bad_entry is documented as optional (may be NULL) independently of
 * whether the fold succeeds - a caller that does not care why must not
 * crash finding out that it failed. */
void test_fold_eligible_rejection_with_null_bad_entry_does_not_crash(void) {
  char eligible[BB_MAX_VOTERS][BB_CERT_LEN];
  int count;
  set_eligible(eligible, &count, "way-too-long-to-be-a-legal-player-name", NULL);

  TEST_ASSERT_EQUAL_INT(-1, bc_fold_eligible(eligible, &count, NULL, 0));
}

void test_fold_eligible_empty_list_is_a_noop(void) {
  char eligible[BB_MAX_VOTERS][BB_CERT_LEN];
  int count = 0;
  TEST_ASSERT_EQUAL_INT(0, bc_fold_eligible(eligible, &count, NULL, 0));
  TEST_ASSERT_EQUAL_INT(0, count);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_build_transition_accepts_lifecycle_ops);
  RUN_TEST(test_build_transition_rejects_non_lifecycle_op);
  RUN_TEST(test_prevalidate_matches_brain_validator);
  RUN_TEST(test_build_create_valid_config);
  RUN_TEST(test_build_create_rejects_invalid_with_same_error);
  RUN_TEST(test_fold_eligible_lowercases_every_entry);
  RUN_TEST(test_fold_eligible_dedupes_case_insensitively_keeping_first_order);
  RUN_TEST(test_fold_eligible_rejects_illegal_name_and_reports_it);
  RUN_TEST(test_fold_eligible_rejection_with_null_bad_entry_does_not_crash);
  RUN_TEST(test_fold_eligible_empty_list_is_a_noop);
  return UNITY_END();
}
