/*
 * seedgen_jwt.c - mints the token seed corpus.
 *
 * A separate program because a valid HMAC-SHA256 token cannot be written by
 * hand into a shell script, and a coverage-guided mutator will never produce
 * one: without a real token to mutate from, fuzz_jwt_verify explores the "wrong
 * field count" and "MAC of the wrong width" branches forever and never reaches
 * claim validation or expiry, which is where the interesting logic is.
 *
 * "jwt" in the name is historical - see fuzz_jwt_verify.c.
 *
 * Usage: seedgen_jwt <corpus-dir>
 */

#include "jwt_fuzz_secret.h"
#include "auth.h" /* the token layer's private header; -I set in the Makefile */

#include <stdio.h>
#include <string.h>

static int write_seed(const char *dir, const char *name, const char *text) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "seedgen_jwt: cannot write %s\n", path);
    return -1;
  }
  fputs(text, f);
  fclose(f);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
    return 2;
  }
  const char *dir = argv[1];

  struct {
    const char *name;
    token_claims_t claims;
  } cases[] = {
      {"valid", {1, "alice", JWT_FUZZ_NOW - 60, JWT_FUZZ_EXP}},
      /* Boundary: exp exactly at now. token_verify rejects now >= exp, so
       * this one must be rejected - a seed that walks the fuzzer straight to
       * the comparison rather than to the parser. */
      {"exp_at_now", {2, "bob", JWT_FUZZ_NOW - 60, JWT_FUZZ_NOW}},
      {"expired", {3, "carol", JWT_FUZZ_NOW - 7200, JWT_FUZZ_NOW - 3600}},
      /* The name cap is 15 characters plus NUL (#47). */
      {"max_name", {4, "abcdefghijklmno", JWT_FUZZ_NOW - 60, JWT_FUZZ_EXP}},
      {"min_name", {5, "a", JWT_FUZZ_NOW - 60, JWT_FUZZ_EXP}},
      {"big_sub",
       {9223372036854775807LL, "dave", JWT_FUZZ_NOW - 60, JWT_FUZZ_EXP}},
  };

  int failures = 0;
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    char token[TOKEN_MAX_LEN];
    if (token_mint(token, sizeof token, &cases[i].claims, JWT_FUZZ_SECRET,
                 sizeof JWT_FUZZ_SECRET) != 0) {
      fprintf(stderr, "seedgen_jwt: mint failed for %s\n", cases[i].name);
      failures++;
      continue;
    }
    if (write_seed(dir, cases[i].name, token) != 0)
      failures++;

    /* One tampered copy per token: same structure, last character bumped, so
     * the corpus carries a signature-mismatch case that is otherwise a
     * 1-in-2^256 mutation away. */
    if (i == 0) {
      size_t n = strlen(token);
      if (n > 0) {
        token[n - 1] = (token[n - 1] == 'A') ? 'B' : 'A';
        if (write_seed(dir, "valid_bad_sig", token) != 0)
          failures++;
      }
    }
  }

  return failures == 0 ? 0 : 1;
}
