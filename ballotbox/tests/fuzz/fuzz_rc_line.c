/*
 * fuzz_rc_line.c - rc_classify_line() on an arbitrary line.
 *
 * The cheapest target here (no allocation downstream, no I/O) and one of the
 * more consequential: this function decides whether a line of .tetrishrc is a
 * comment, a PATH assignment, or A COMMAND THE SHELL WILL RUN. A line that
 * classifies as RC_LINE_COMMAND is executed. So the interesting failure is not
 * a crash but a misclassification, and the header spells out the edge the
 * fuzzer should be hunting around: "PATHETIC" is a command, "PATH = x" is not.
 *
 * The returned value is a pointer INTO the caller's buffer, so the other check
 * is containment - a *value that points outside the line, or past its
 * terminator, hands the shell a string of unrelated memory to run.
 */

#include "libtetrisutil/rc.h"
#include "fuzz_support.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 4096) return 0;

  char *line = (char *)malloc(size + 1);
  if (!line) return 0;
  memcpy(line, data, size);
  line[size] = '\0';

  const char *value = (const char *)0xDEAD; /* poison: RC_LINE_EMPTY must
                                             * overwrite this with NULL, not
                                             * merely leave it alone */
  rc_line_type_t t = rc_classify_line(line, &value);

  switch (t) {
    case RC_LINE_EMPTY:
      FUZZ_CHECK(value == NULL);
      break;

    case RC_LINE_PATH:
    case RC_LINE_COMMAND:
      FUZZ_CHECK(value != NULL);
      /* Inside the buffer, including one past the last byte (the empty
       * substring at the terminator is a legal answer for "PATH="). */
      FUZZ_CHECK(value >= line && value <= line + size);
      /* And terminated within it, so strlen/execvp on it stays in bounds. */
      FUZZ_CHECK(memchr(value, '\0', (size_t)(line + size + 1 - value)) !=
                 NULL);
      break;

    default:
      /* A fourth value would fall through every caller's switch. */
      FUZZ_CHECK(0);
  }

  /* A command's text is trimmed of leading whitespace (header contract), so
   * the shell never tries to exec " ls". */
  if (t == RC_LINE_COMMAND)
    FUZZ_CHECK(value[0] != ' ' && value[0] != '\t');

  /* Classification must not depend on the caller's buffer beyond the line
   * itself: same bytes in a differently-sized allocation, same verdict. This
   * catches a read past the terminator that happens to land on the same
   * value in one allocation and a different one in another. */
  {
    char *copy = (char *)malloc(size + 64);
    if (copy) {
      memset(copy, 'Z', size + 64);
      memcpy(copy, line, size);
      copy[size] = '\0';
      const char *v2 = NULL;
      FUZZ_CHECK(rc_classify_line(copy, &v2) == t);
      free(copy);
    }
  }

  free(line);
  return 0;
}
