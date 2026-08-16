#ifndef TETRISH_TEST_OUTPUT_H
#define TETRISH_TEST_OUTPUT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static char test_output_details[8192];
static size_t test_output_details_used;

static inline void test_output_failure_detail(const char *message,
                                              const char *file, int line)
{
    size_t remaining = sizeof test_output_details - test_output_details_used;
    if (remaining <= 1)
        return;

    int written = snprintf(test_output_details + test_output_details_used,
                           remaining, "      %s (%s:%d)\n", message, file,
                           line);
    if (written < 0)
        return;
    if ((size_t)written >= remaining)
        test_output_details_used = sizeof test_output_details - 1;
    else
        test_output_details_used += (size_t)written;
}

static inline void test_output_failure_detailf(const char *file, int line,
                                               const char *format, ...)
{
    char message[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof message, format, ap);
    va_end(ap);
    test_output_failure_detail(message, file, line);
}

static inline void test_output_begin(const char *suite)
{
    test_output_details[0] = '\0';
    test_output_details_used = 0;
    printf("%s\n", suite);
    fflush(stdout);
}

static inline void test_output_pass(const char *name)
{
    printf("PASS  %s\n", name);
    fflush(stdout);
}

static inline void test_output_fail(const char *name)
{
    printf("FAIL  %s\n", name);
    if (test_output_details[0] != '\0')
    {
        fputs(test_output_details, stdout);
        test_output_details[0] = '\0';
        test_output_details_used = 0;
    }
    fflush(stdout);
}

static inline void test_output_skip(const char *name, const char *reason)
{
    printf("SKIP  %s\n      %s\n", name, reason);
    fflush(stdout);
}

static inline void test_output_summary(int total, int failed, int skipped)
{
    printf("Summary: %d passed, %d failed, %d skipped, %d total\n",
           total - failed - skipped, failed, skipped, total);
    fflush(stdout);
}

static inline void test_output_check(int *total, int *failed, const char *name,
                                     int ok)
{
    (*total)++;
    if (ok)
        test_output_pass(name);
    else
    {
        (*failed)++;
        test_output_fail(name);
    }
}

#endif
