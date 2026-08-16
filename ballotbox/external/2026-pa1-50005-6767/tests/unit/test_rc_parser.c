/*
 * tests/unit/test_rc_parser.c
 *
 * Unit tests for classify_rc_line, declared in source/rc_parser.h.
 *
 * These tests are written against the contract in rc_parser.h. They fail
 * with the stub implementation in source/rc_parser.c. Implement
 * classify_rc_line until they pass.
 *
 * Run with:
 *   make unit
 */

#include "unity.h"
#include "rc_parser.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_empty_string_is_empty(void) {
    const char *v = (const char *)0x1; /* poison; expect classify to set to NULL */
    TEST_ASSERT_EQUAL_INT(RC_LINE_EMPTY, classify_rc_line("", &v));
    TEST_ASSERT_NULL(v);
}

static void test_whitespace_only_is_empty(void) {
    const char *v;
    TEST_ASSERT_EQUAL_INT(RC_LINE_EMPTY, classify_rc_line("   \t  ", &v));
}

static void test_simple_path_line(void) {
    const char *v = NULL;
    TEST_ASSERT_EQUAL_INT(RC_LINE_PATH, classify_rc_line("PATH=/usr/bin:/bin", &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("/usr/bin:/bin", v);
}

static void test_path_with_empty_value(void) {
    /* "PATH=" with nothing after is still a PATH line, value is empty string. */
    const char *v = NULL;
    TEST_ASSERT_EQUAL_INT(RC_LINE_PATH, classify_rc_line("PATH=", &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("", v);
}

static void test_pathetic_is_not_a_path_line(void) {
    /* A line starting with "PATH" but no "=" is a command, not a PATH line. */
    const char *v = NULL;
    TEST_ASSERT_EQUAL_INT(RC_LINE_COMMAND, classify_rc_line("PATHETIC", &v));
}

static void test_simple_command_line(void) {
    const char *v = NULL;
    TEST_ASSERT_EQUAL_INT(RC_LINE_COMMAND, classify_rc_line("clear", &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("clear", v);
}

static void test_command_with_leading_whitespace(void) {
    /* Leading whitespace should be trimmed; the command value points after it. */
    const char *v = NULL;
    TEST_ASSERT_EQUAL_INT(RC_LINE_COMMAND, classify_rc_line("   clear", &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("clear", v);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_string_is_empty);
    RUN_TEST(test_whitespace_only_is_empty);
    RUN_TEST(test_simple_path_line);
    RUN_TEST(test_path_with_empty_value);
    RUN_TEST(test_pathetic_is_not_a_path_line);
    RUN_TEST(test_simple_command_line);
    RUN_TEST(test_command_with_leading_whitespace);
    return UNITY_END();
}
