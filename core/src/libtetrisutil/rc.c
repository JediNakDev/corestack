#include "libtetrisutil/rc.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* pointer to cached rc file */
static char *content_ptr;
static int file_not_found;

/* Skip leading whitespace, returning a pointer into s. */
static char *lstrip(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Trim trailing whitespace from s in place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

static const char *root(void)
{
    const char *env = getenv("TETRISH_ROOT");
    return (env != NULL && env[0] != '\0') ? env : ".";
}

/* Read the whole file save to heap and add content_ptr as a pointer */
static void snapshot(void)
{
    if (content_ptr != NULL || file_not_found)
        return;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", root(), RC_PATH);

    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        file_not_found = 1;
        return;
    }

    size_t len = 0, cap = 4096;
    char *content = malloc(cap);
    while (content != NULL)
    {
        len += fread(content + len, 1, cap - len - 1, f);
        if (len + 1 < cap)
            break; /* short read: that was the end of the file */

        cap *= 2;
        char *grown = realloc(content, cap);
        if (grown == NULL)
        {
            free(content);
            content = NULL;
            break;
        }
        content = grown;
    }
    fclose(f);

    /* Out of memory lands = file that could not be opened */
    if (content == NULL)
        file_not_found = 1;
    else
        content[len] = '\0';
    content_ptr = content;
}

void rc_reload(void)
{
    free(content_ptr);
    content_ptr = NULL;
    file_not_found = 0;
}

/* The lookup itself, with no fallback applied. Called by rc_get(). */
static int lookup(const char *key, char *out, size_t cap)
{
    snapshot();
    if (content_ptr == NULL)
        return RC_NO_FILE;

    int found = 0;
    const char *at = content_ptr;
    while (*at != '\0')
    {
        const char *nl = strchr(at, '\n');
        size_t n = nl != NULL ? (size_t)(nl - at) : strlen(at);

        char line[PATH_MAX + 64];
        if (n < sizeof line)
        {
            memcpy(line, at, n);
            line[n] = '\0';
            line[strcspn(line, "\r")] = '\0';

            char *name = lstrip(line);
            char *eq = strchr(name, '=');
            /* Skipped: blank lines, comments, and anything that is not
             * key=value - a bare shell command, or PATH=, both of which belong
             * to the shell and not to us. */
            if (*name != '\0' && *name != '#' && eq != NULL)
            {
                *eq = '\0';
                rstrip(name);
                if (strcmp(name, key) == 0)
                {
                    char *value = lstrip(eq + 1);
                    rstrip(value);

                    /* Last one wins, including over an earlier bad value:
                     * the bottom line of the file is the one in force. */
                    if (value[0] == '\0' || strlen(value) >= cap)
                    {
                        found = -1;
                    }
                    else
                    {
                        memcpy(out, value, strlen(value) + 1);
                        found = 1;
                    }
                }
            }
        }

        if (nl == NULL)
            break;
        at = nl + 1;
    }

    return found;
}

rc_get_status_t rc_get(const char *key, const char *fallback, char *out,
                       size_t cap)
{
    if (out == NULL)
    {
        return RC_OUT_NULL_POINTER;
    }

    int found = lookup(key, out, cap);

    if (found != 1 && fallback != NULL)
        snprintf(out, cap, "%s", fallback);
    return found;
}

rc_get_status_t rc_get_int(const char *key, int fallback, long lo, long hi,
                           int *out)
{
    if (out == NULL)
    {
        return RC_OUT_NULL_POINTER;
    }

    char value[64];

    int found = lookup(key, value, sizeof value);
    if (found != 1)
    {
        *out = fallback;
        return found;
    }

    char *end;
    long n = strtol(value, &end, 10);
    /* Whole-string: "30s" is a typo, not the number 30. */
    if (end == value || *end != '\0' || n < lo || n > hi)
    {
        *out = fallback;
        return RC_VALUE_INVALID;
    }

    *out = (int)n;
    return RC_VALUE_FOUND;
}

static const char *const yes[] = {"on", "yes", "true", "1"};
static const char *const no[] = {"off", "no", "false", "0"};

rc_get_status_t rc_get_bool(const char *key, int fallback, int *out)
{
    if (out == NULL)
    {
        return RC_OUT_NULL_POINTER;
    }

    char value[16];

    int found = lookup(key, value, sizeof value);
    if (found != 1)
    {
        *out = fallback;
        return found;
    }

    for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++)
    {
        if (strcasecmp(value, yes[i]) == 0)
        {
            *out = 1;
            return RC_VALUE_FOUND;
        }
        if (strcasecmp(value, no[i]) == 0)
        {
            *out = 0;
            return RC_VALUE_FOUND;
        }
    }
    *out = fallback;
    return RC_VALUE_INVALID;
}
