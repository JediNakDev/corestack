#ifndef LIBTETRISUTIL_RC_H
#define LIBTETRISUTIL_RC_H

/**
 * @file rc.h
 * @brief Reading one directive out of .tetrishrc.
 */

#define RC_PATH ".tetrishrc"

#include <stddef.h>

typedef enum
{
    RC_VALUE_FOUND = 1,
    RC_VALUE_ABSENT = 0,
    RC_VALUE_INVALID = -1,
    RC_NO_FILE = -2,
    RC_OUT_NULL_POINTER = -3
} rc_get_status_t;

/**
 * Reads one directive as a string, with the caller's default applied.
 *
 * @param key       Directive name, matched exactly.
 * @param fallback  Written to out whenever the return is not 1. NULL leaves
 *                  out untouched, for a reader that has already seeded it or
 *                  that only wants the return value.
 * @param out       Receives the trimmed value, or fallback.
 * @param cap       sizeof out.
 * @returns rc get status
 */
rc_get_status_t rc_get(const char *key, const char *fallback, char *out,
                       size_t cap);

/**
 * Reads one directive as a whole number and check if inside [lo, hi].
 *
 * @param key  Directive name.
 * @param fallback  Written to out whenever the return is not 1. NULL leaves
 *                  out untouched, for a reader that has already seeded it or
 *                  that only wants the return value.
 * @param lo   Smallest accepted value, inclusive.
 * @param hi   Largest accepted value, inclusive.
 * @param out  Written only when 1 is returned, so whatever the caller seeded
 *             it with is the default. May be NULL to validate a key this
 *             reader owns but does not consume.
 * @returns rc get status
 */
rc_get_status_t rc_get_int(const char *key, int fallback, long lo, long hi,
                           int *out);

/**
 * Reads one directive as on/yes/true/1 or off/no/false/0, case-insensitively.
 *
 * @param key  Directive name.
 * @param fallback  Written to out whenever the return is not 1. NULL leaves
 *                  out untouched, for a reader that has already seeded it or
 *                  that only wants the return value.
 * @param out  Written only when 1 is returned, so whatever the caller seeded
 *             it with is the default.
 * @returns rc get status
 */
rc_get_status_t rc_get_bool(const char *key, int fallback, int *out);

/**
 * Drops the snapshot, so the next rc_get* call reads the file again.
 *
 * For a SIGHUP reload, and for a test that points TETRISH_ROOT somewhere new
 * mid-process. Not async-signal-safe: set a flag in the handler and call this
 * from the loop, like every other reload step.
 */
void rc_reload(void);

#endif /* LIBTETRISUTIL_RC_H */
