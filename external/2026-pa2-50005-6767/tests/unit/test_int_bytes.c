/**
 * tests/unit/test_int_bytes.c
 *
 * Unit tests for the integer <-> 8-byte big-endian helpers in
 * source/common.c:
 *   void     int_to_bytes(uint64_t x, unsigned char buf[INT_BYTES])
 *   uint64_t bytes_to_int(const unsigned char buf[INT_BYTES])
 *
 * These are pure functions, so they are tested directly without any
 * socket setup. Link against source/common.c and tests/unity/unity.c.
 */

#include "unity.h"
#include "common.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---------- int_to_bytes ---------- */

static void test_int_to_bytes_zero(void)
{
    unsigned char buf[INT_BYTES];
    unsigned char zero[INT_BYTES] = {0, 0, 0, 0, 0, 0, 0, 0};
    int_to_bytes(0, buf);
    TEST_ASSERT_EQUAL_MEMORY(zero, buf, INT_BYTES);
}

static void test_int_to_bytes_one_is_big_endian(void)
{
    /* Big-endian: the LSB sits at the LAST byte. */
    unsigned char buf[INT_BYTES];
    unsigned char expected[INT_BYTES] = {0, 0, 0, 0, 0, 0, 0, 1};
    int_to_bytes(1, buf);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, INT_BYTES);
}

static void test_int_to_bytes_high_byte(void)
{
    /* 2^56 sets only the MSB. */
    unsigned char buf[INT_BYTES];
    unsigned char expected[INT_BYTES] = {0x01, 0, 0, 0, 0, 0, 0, 0};
    int_to_bytes((uint64_t)1 << 56, buf);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, INT_BYTES);
}

static void test_int_to_bytes_max(void)
{
    unsigned char buf[INT_BYTES];
    unsigned char expected[INT_BYTES] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int_to_bytes(UINT64_MAX, buf);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, INT_BYTES);
}

/* ---------- bytes_to_int ---------- */

static void test_bytes_to_int_zero(void)
{
    unsigned char buf[INT_BYTES] = {0, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT64(0, bytes_to_int(buf));
}

static void test_bytes_to_int_big_endian(void)
{
    unsigned char one[INT_BYTES] = {0, 0, 0, 0, 0, 0, 0, 1};
    unsigned char high[INT_BYTES] = {0x01, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT64(1, bytes_to_int(one));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)1 << 56, bytes_to_int(high));
}

static void test_bytes_to_int_max(void)
{
    unsigned char buf[INT_BYTES] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, bytes_to_int(buf));
}

/* ---------- round trip ---------- */

static void test_roundtrip(void)
{
    const uint64_t values[] = {
        0, 1, 7, 255, 256, 1024, 65535,
        (uint64_t)1 << 32,
        (uint64_t)1 << 56,
        ((uint64_t)1 << 63) - 1,
        UINT64_MAX};
    const size_t n = sizeof(values) / sizeof(values[0]);

    for (size_t i = 0; i < n; i++)
    {
        unsigned char buf[INT_BYTES];
        int_to_bytes(values[i], buf);
        TEST_ASSERT_EQUAL_UINT64(values[i], bytes_to_int(buf));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_int_to_bytes_zero);
    RUN_TEST(test_int_to_bytes_one_is_big_endian);
    RUN_TEST(test_int_to_bytes_high_byte);
    RUN_TEST(test_int_to_bytes_max);
    RUN_TEST(test_bytes_to_int_zero);
    RUN_TEST(test_bytes_to_int_big_endian);
    RUN_TEST(test_bytes_to_int_max);
    RUN_TEST(test_roundtrip);
    return UNITY_END();
}
