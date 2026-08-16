/*
 * fuzz_rows.c - tdb_row_count() / tdb_row_fields() on an arbitrary reply body.
 *
 * These two parse SimpleDB's human-readable select output: a header line, a
 * dashes rule, tab-separated rows, and - the part rows.c is written around -
 * arbitrary parser narration before and after the table that no schema
 * describes. Input is "whatever the JVM printed", which under load, on an
 * aborted transaction, or with a wedged runner is not what the happy path
 * looks like. Every credential check in libtetrisauth reads its salt and
 * digest through these functions, so a misparse here is an auth bug.
 *
 * tdb_row_fields returns SLICES into body and NUL-terminates nothing, so the
 * central check is that every slice it hands back is inside the buffer it was
 * given. An off-by-one there is an over-read in the caller, which is exactly
 * the accident rows.c's header warns about ("nothing is copied ... body must
 * outlive the use of f").
 */

#include "libtetrisdb/socket/db.h"
#include "fuzz_support.h"

#include <stdlib.h>
#include <string.h>

/* Deliberately larger than any row the schema defines: a return greater than
 * max is documented as a schema disagreement, and this target must be able to
 * observe that case rather than truncating it away. */
#define MAX_FIELDS 32

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 64 * 1024) return 0;

  /* NUL-terminated on the heap: the API takes a C string, and the redzone is
   * what turns "scanned one byte past the terminator" into a report. */
  char *body = (char *)malloc(size + 1);
  if (!body) return 0;
  memcpy(body, data, size);
  body[size] = '\0';

  int rows = tdb_row_count(body);

  /* db.h: the row count is a count or -1. Zero and -1 are documented as
   * different answers ("no such user" vs "this is not an answer"), and any
   * other negative would sail past a `if (n < 0)` check written as `== -1`. */
  FUZZ_CHECK(rows >= -1);

  if (rows < 0) {
    free(body);
    return 0;
  }

  for (int r = 0; r < rows; r++) {
    const char *f[MAX_FIELDS];
    size_t len[MAX_FIELDS];
    memset(f, 0, sizeof f);
    memset(len, 0, sizeof len);

    int n = tdb_row_fields(body, r, f, len, MAX_FIELDS);

    /* A row the count promised exists must be readable. -1 here means the two
     * functions disagree about the same body, and a caller looping to
     * tdb_row_count() would read uninitialised f[]/len[] on the strength of
     * a count that turned out to be a lie. */
    FUZZ_CHECK(n >= 0);

    int filled = n < MAX_FIELDS ? n : MAX_FIELDS;
    for (int i = 0; i < filled; i++) {
      FUZZ_CHECK(f[i] != NULL);
      FUZZ_CHECK(fuzz_slice_inside(f[i], len[i], body, size));
      /* Fields are tab-separated and the table is line-oriented, so neither
       * separator can appear INSIDE a field - if one does, the split ran past
       * its row and the caller reads column N of row R as column N of row
       * R+1. Positional fields make that silent: a salt read as a digest. */
      FUZZ_CHECK(memchr(f[i], '\t', len[i]) == NULL);
      FUZZ_CHECK(memchr(f[i], '\n', len[i]) == NULL);
    }
  }

  /* Out-of-range rows are an error, never a read. Both ends: one past the
   * last row, and a negative index that a signed multiply could turn into a
   * backwards offset. */
  {
    const char *f[MAX_FIELDS];
    size_t len[MAX_FIELDS];
    FUZZ_CHECK(tdb_row_fields(body, rows, f, len, MAX_FIELDS) == -1);
    FUZZ_CHECK(tdb_row_fields(body, -1, f, len, MAX_FIELDS) == -1);
  }

  free(body);
  return 0;
}
