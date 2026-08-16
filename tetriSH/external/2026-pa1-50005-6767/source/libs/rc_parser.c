/*
 * rc_parser.c
 *
 * Stub implementation. The current code returns RC_LINE_EMPTY for every
 * input, which makes the unit tests in tests/unit/test_rc_parser.c fail.
 * Your task (if you choose to use this helper) is to make the tests pass.
 *
 * The expected behaviour is documented in rc_parser.h. Run:
 *
 *   make unit
 *
 * to see which tests fail, then fix the implementation until they pass.
 *
 * This file is OPTIONAL. See the comment at the top of rc_parser.h.
 */

#include "libs/rc_parser.h"

#include <ctype.h>
#include <string.h>

rc_line_type_t classify_rc_line(const char *line, const char **value) {
  *value = NULL;
  if (line == NULL)
    return RC_LINE_EMPTY;

  /* Skip leading whitespace. */
  const char *p = line;
  while (*p != '\0' && isspace((unsigned char)*p))
    p++;

  if (*p == '\0')
    return RC_LINE_EMPTY;

  /* A PATH directive must start with the literal "PATH=". A line like
   * "PATHETIC" (PATH not immediately followed by '=') is a command. */
  if (strncmp(p, "PATH=", 5) == 0) {
    *value = p + 5; /* substring after "PATH=" (may be empty) */
    return RC_LINE_PATH;
  }

  *value = p;
  return RC_LINE_COMMAND;
}
