#ifndef RC_PARSER_H
#define RC_PARSER_H

/*
 * rc_parser.h
 *
 * Example pure helper for classifying lines from .cseshellrc.
 *
 * This file is OPTIONAL. You are not required to use this helper in your
 * shell. It exists as a demonstration of:
 *
 *   1. How to extract a pure function from your .cseshellrc reader so it
 *      can be unit tested without spawning the shell.
 *   2. How tests/unit/test_rc_parser.c links against this file via the
 *      makefile convention (tests/unit/test_FOO.c pairs with source/FOO.c).
 *
 * If you use this helper, classify_rc_line takes one line of text and
 * tells you whether it is empty, a PATH= directive, or a command to run.
 *
 * If you do not use it, you can delete this file (and the matching
 * source/rc_parser.c and tests/unit/test_rc_parser.c) without affecting
 * the rest of your assignment.
 */

typedef enum {
    RC_LINE_EMPTY,    /* blank or whitespace-only */
    RC_LINE_PATH,     /* starts with literal "PATH=" */
    RC_LINE_COMMAND   /* anything else, after trimming leading whitespace */
} rc_line_type_t;

/*
 * Classify one line from .cseshellrc.
 *
 *   On RC_LINE_PATH:    *value points to the substring after "PATH=".
 *   On RC_LINE_COMMAND: *value points to the trimmed command text.
 *   On RC_LINE_EMPTY:   *value is set to NULL.
 *
 * The returned pointer is into the input buffer. Do not free it. Do not
 * modify the contents of the input string.
 *
 * A line that starts with "PATH" but does NOT contain "=" immediately
 * after (for example "PATHETIC") is RC_LINE_COMMAND, not RC_LINE_PATH.
 */
rc_line_type_t classify_rc_line(const char *line, const char **value);

#endif
