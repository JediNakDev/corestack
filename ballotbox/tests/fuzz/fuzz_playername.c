/*
 * fuzz_playername.c - user_name_ok() / user_name_fold() on arbitrary bytes.
 *
 * Thirty-six lines of implementation, and it guards a SQL string, a
 * tab-separated wire format and a credential lookup all at once: the header
 * explains that the allowlist is what lets tdb_quote, the credential body's
 * field split and the select-reply parser each skip an escaping step. So the
 * property that matters is not "does it crash" but "does anything it accepts
 * carry a byte that would break one of those three" - LF, TAB, quote.
 *
 * Cheap enough (no allocation in the code under test) that it runs at
 * millions of execs/sec and costs nothing to keep in the nightly set.
 */

#include "fuzz_support.h"
#include "libtetrisutil/name.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 1024)
    return 0;

  /* Not NUL-terminated on purpose: the API takes (s, len) precisely because
   * the wire hands it a slice, and an implementation that reached for a
   * terminator would read past this allocation - which ASan will catch here
   * and would not catch on a padded stack buffer. */
  char *name = (char *)malloc(size ? size : 1);
  if (!name)
    return 0;
  memcpy(name, data, size);

  bool ok = user_name_ok(name, size);

  if (ok) {
    /* The three characters the rest of the system is allowed to assume are
     * absent, per the header's rationale. */
    FUZZ_CHECK(memchr(name, '\n', size) == NULL);
    FUZZ_CHECK(memchr(name, '\t', size) == NULL);
    FUZZ_CHECK(memchr(name, '\'', size) == NULL);
    FUZZ_CHECK(size >= 1 && size <= PLAYER_NAME_MAX);

    /* Accepted names must fold, and folding must be the total ASCII lowercase
     * of the input - the storage form is permanent (no UPDATE in SimpleDB),
     * so a fold that drops or mangles a byte is unrecoverable. */
    char dst[MAX_USER_NAME];
    memset(dst, 0x7F, sizeof dst);
    FUZZ_CHECK(user_name_fold(dst, sizeof dst, name, size) == 0);
    FUZZ_CHECK(fuzz_is_cstr(dst, sizeof dst));
    FUZZ_CHECK(strlen(dst) == size);
    for (size_t i = 0; i < size; i++) {
      unsigned char c = (unsigned char)name[i];
      unsigned char want =
          (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
      FUZZ_CHECK((unsigned char)dst[i] == want);
    }

    /* Folding is idempotent: the stored form of a stored name is itself. */
    char twice[MAX_USER_NAME];
    FUZZ_CHECK(user_name_fold(twice, sizeof twice, dst, strlen(dst)) == 0);
    FUZZ_CHECK(strcmp(twice, dst) == 0);
  } else {
    /* A rejected name must not be written anywhere: "untouched on failure". */
    char dst[MAX_USER_NAME];
    memset(dst, 0x7F, sizeof dst);
    FUZZ_CHECK(user_name_fold(dst, sizeof dst, name, size) == -1);
    for (size_t i = 0; i < sizeof dst; i++)
      FUZZ_CHECK(dst[i] == 0x7F);
  }

  /* An undersized destination is refused whatever the name - the cap check
   * must not depend on the name being legal first. */
  if (size >= 1) {
    char small[MAX_USER_NAME];
    FUZZ_CHECK(user_name_fold(small, MAX_USER_NAME - 1, name, size) == -1);
  }

  free(name);
  return 0;
}
