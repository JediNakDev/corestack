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
 *   1. How to extract a pure function from your .tetrishrc reader so it
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

typedef enum
{
    RC_LINE_EMPTY,     /* blank, whitespace-only, or a '#' comment */
    RC_LINE_PATH,      /* starts with literal "PATH=" */
    RC_LINE_DIRECTIVE, /* "key = value" belonging to some other reader */
    RC_LINE_COMMAND    /* anything else, after trimming leading whitespace */
} rc_line_type_t;

/*
 * Classify one line from .tetrishrc.
 *
 *   On RC_LINE_PATH:      *value points to the substring after "PATH=".
 *   On RC_LINE_COMMAND:   *value points to the trimmed command text.
 *   On RC_LINE_DIRECTIVE: *value points to the trimmed line, key first.
 *   On RC_LINE_EMPTY:     *value is set to NULL.
 *
 * The returned pointer is into the input buffer. Do not free it. Do not
 * modify the contents of the input string.
 *
 * A line that starts with "PATH" but does NOT contain "=" immediately
 * after (for example "PATHETIC") is RC_LINE_COMMAND, not RC_LINE_PATH.
 *
 * .tetrishrc is shared with libtetrisutil's rc_load() and tetrislogd's config
 * loader, so a "key = value" line whose key is a single bare token is
 * classified RC_LINE_DIRECTIVE: it belongs to another reader and the shell
 * must not try to run it. A command containing '=' is unaffected, because its
 * pre-'=' text spans a space ("setenv FOO=bar" stays RC_LINE_COMMAND).
 *
 * A directive is reported rather than folded into RC_LINE_EMPTY so the caller
 * can say something about a key no reader claims. Every key in this file is
 * dropped by somebody: an unknown one inside a namespace is refused by
 * rc_bind()'s owned_prefix, but an unknown one OUTSIDE every namespace is
 * refused by nobody, and the shell is the only process that reads all of them.
 */
rc_line_type_t classify_rc_line(const char *line, const char **value);

#endif
