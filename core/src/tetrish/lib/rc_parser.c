/*
 * rc_parser.c
 *
 * Classifies one line of .tetrishrc for the shell's reader in
 * core/src/tetrish/main.c. The behaviour is documented in rc_parser.h; the
 * cases are covered end to end by tests/test_shell.c, which drives the real
 * binary over a pipe rather than calling this function directly.
 *
 *   make bin/tests/test_shell && ./bin/tests/test_shell
 */

#include "rc_parser.h"

#include <ctype.h>
#include <string.h>

/* Is key = value ? */
static int is_declare_var(const char *p)
{
    const char *eq = strchr(p, '=');
    if (eq == NULL)
        return 0;

    for (const char *q = p; q < eq; q++)
    {
        if (isspace((unsigned char)*q))
        {
            /* Trailing space before '=' is still a directive ("key = v"); an
             * embedded one means a multi-word command. */
            while (q < eq && isspace((unsigned char)*q))
                q++;
            return q == eq;
        }
    }
    return eq != p; /* "=value" with no key is not a directive */
}

rc_line_type_t classify_rc_line(const char *line, const char **value)
{
    *value = NULL;
    if (line == NULL)
        return RC_LINE_EMPTY;

    /* Skip leading whitespace. */
    const char *p = line;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;

    if (*p == '\0')
        return RC_LINE_EMPTY;

    if (strncmp(p, "PATH", 4) == 0)
    {
        const char *q = p + 4;
        while (isspace((unsigned char)*q))
            q++;
        if (*q == '=')
        {
            q++;
            while (isspace((unsigned char)*q))
                q++;
            *value = q; /* substring after "PATH=" (may be empty) */
            return RC_LINE_PATH;
        }
    }

    /* Comments and other readers' directives are not ours to run. */
    if (*p == '#')
        return RC_LINE_EMPTY;

    if (is_declare_var(p))
    {
        *value = p;
        return RC_LINE_DIRECTIVE;
    }

    *value = p;
    return RC_LINE_COMMAND;
}
