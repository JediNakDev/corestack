#ifndef LIBTETRISUTIL_PLAYERNAME_H
#define LIBTETRISUTIL_PLAYERNAME_H

/**
 * @file name.h
 * @brief What a tetriSH player name is, in one place.
 *
 * Called by libtetrisauth's credential reader on the way from the wire to
 * storage, and by the client's form validator before anything is sent. The
 * rule was written three ways before this header existed and the three
 * disagreed: the client used isalnum(), which is locale-sensitive, so it could
 * accept a name the server then refused.
 *
 * One deliberate duplicate remains. core/src/libtetrisauth/lib/token.c keeps
 * its own static copy, because it may include nothing outside OpenSSL - the
 * assertion bin/tests/test_token exists to enforce.
 */

#include "libtetrisutil/limits.h"

#include <stdbool.h>
#include <stddef.h>

/** #47 decision 2's cap, and MAX_USER_NAME - 1: the username IS the roster
 * display name, so the two move together. */
#define USER_NAME_MAX (MAX_USER_NAME - 1)

/**
 * Is this character legal in a player name: [A-Za-z0-9_-].
 *
 * One predicate rather than three exclusions. LF would break the credential
 * body's field split, TAB would shift every field of a tab-separated select
 * reply, and a quote would survive db_quote()'s doubling into storage as
 * o''brien - but a reader need not hold those three facts.
 *
 * @param c  Character to test.
 * @returns true if the character is in the allowlist.
 */
bool user_name_char_ok(unsigned char c);

/**
 * Is this a legal player name: the allowlist, 1..USER_NAME_MAX characters.
 *
 * Case is not folded and nothing is written, so a name this rejects would only
 * ever earn a 400 the server was always going to send.
 *
 * @param s    Name; need not be NUL-terminated.
 * @param len  Length in bytes.
 * @returns true if the name is acceptable.
 */
bool user_name_ok(const char *s, size_t len);

/**
 * Validates a name and writes its folded form.
 *
 * The ASCII fold is total over the allowlist. `JediNakDev` is stored and
 * displayed as `jedinakdev` forever: SimpleDB has no UPDATE, so there is no
 * later repair and no second column holding the typed casing.
 *
 * @param dst  Receives the folded name, NUL-terminated; untouched on failure.
 * @param cap  Capacity of dst; must be at least MAX_USER_NAME.
 * @param s    Name; need not be NUL-terminated.
 * @param len  Length in bytes.
 * @returns 0 on success, -1 if the name is illegal or dst is too small.
 */
int user_name_fold(char *dst, size_t cap, const char *s, size_t len);

#endif /* LIBTETRISUTIL_PLAYERNAME_H */
