/*
 * tests/unit/test_perms.c
 *
 * this unit test directly test under libs/perms.c
 *
 * Run with:
 *   make unit
 */

#include "unity.h"
#include "perms.h"
#include <string.h>
#include <sys/stat.h>

void setUp(void) {}
void tearDown(void) {}

static void test_empty_mode(void)
{
    char buf[16];
    perms_to_string(0, buf);
    TEST_ASSERT_EQUAL_STRING("----------", buf);
}

static void test_regular_file_0644(void)
{
    char buf[16];
    perms_to_string(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, buf);
    TEST_ASSERT_EQUAL_STRING("-rw-r--r--", buf);
}

static void test_directory_0755(void)
{
    char buf[16];
    mode_t m = S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    perms_to_string(m, buf);
    TEST_ASSERT_EQUAL_STRING("drwxr-xr-x", buf);
}

static void test_all_owner_perms_only(void)
{
    char buf[16];
    perms_to_string(S_IRUSR | S_IWUSR | S_IXUSR, buf);
    TEST_ASSERT_EQUAL_STRING("-rwx------", buf);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_mode);
    RUN_TEST(test_regular_file_0644);
    RUN_TEST(test_directory_0755);
    RUN_TEST(test_all_owner_perms_only);
    return UNITY_END();
}
