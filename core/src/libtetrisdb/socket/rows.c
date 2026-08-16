#include "libtetrisdb/socket/db.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Start of the line after p, or NULL if p was the last one. */
static const char *next_line(const char *p)
{
    const char *nl = strchr(p, '\n');
    return nl != NULL ? nl + 1 : NULL;
}

/* Length of the line at p, excluding its newline. */
static size_t line_len(const char *p)
{
    const char *nl = strchr(p, '\n');
    return nl != NULL ? (size_t)(nl - p) : strlen(p);
}

/* Is this line nothing but dashes? That is the rule under a header, and it is
 * the one line in the output whose shape is unmistakable. */
static int is_rule(const char *p)
{
    size_t n = line_len(p);

    if (n == 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] != '-')
            return 0;
    return 1;
}

/** The first data row of a select reply, or NULL if body carries no table. */
static const char *rows_begin(const char *body)
{
    if (body == NULL)
        return NULL;

    /* From the second line on: a rule needs a header above it, and a body that
     * opens with dashes is not a table. */
    for (const char *p = next_line(body); p != NULL && *p != '\0';
         p = next_line(p))
        if (is_rule(p))
            return next_line(p);

    return NULL;
}

/** How many rows the reply says it has */
static int block_count(const char *rows)
{
    int n = 0;

    for (const char *p = rows; p != NULL && *p != '\0'; p = next_line(p), n++)
    {
        if (line_len(p) > 0)
            continue;

        /* The blank line: the trailer is next. */
        const char *t = next_line(p);
        if (t == NULL)
            return -1;
        while (*t == ' ')
            t++;

        int said = 0, digits = 0;
        while (*t >= '0' && *t <= '9')
        {
            said = said * 10 + (*t++ - '0');
            digits++;
        }
        if (digits == 0 || strncmp(t, " rows.", 6) != 0)
            return -1;

        /* The runner's count and the lines it printed disagreeing means this is
         * not the output it looks like, and guessing which one is right is how
         * a salt gets read as a digest. */
        return said == n ? n : -1;
    }
    return -1; /* no blank line: the body was cut off mid-table */
}

int db_row_count(const char *body)
{
    const char *rows = rows_begin(body);

    return rows != NULL ? block_count(rows) : -1;
}

int db_row_fields(const char *body, int row, const char *f[], size_t len[],
                  int max)
{
    int count = db_row_count(body);
    if (count < 0 || row < 0 || row >= count)
        return -1;

    const char *p = rows_begin(body);
    for (int i = 0; i < row; i++)
    {
        p = next_line(p);
        if (p == NULL)
            return -1; /* the trailer promised more rows than the body carries
                        */
    }

    size_t left = line_len(p);
    int n = 0;

    /* Fields are what tabs separate, including empty ones: a stored empty
     * string is a real value and dropping it would shift every field after it,
     * which is the failure this whole file is arranged to avoid. */
    for (;;)
    {
        const char *tab = memchr(p, '\t', left);
        size_t flen = tab != NULL ? (size_t)(tab - p) : left;

        if (n < max)
        {
            f[n] = p;
            len[n] = flen;
        }
        n++;

        if (tab == NULL)
            break;
        left -= flen + 1;
        p = tab + 1;
    }
    return n;
}

long long db_next_id(const char *body)
{
    int rows = db_row_count(body);
    if (rows < 0)
        return -1;
    if (rows == 0)
        return 1;

    const char *f[1];
    size_t len[1];
    char text[32];

    if (db_row_fields(body, 0, f, len, 1) < 1 || len[0] == 0 ||
        len[0] >= sizeof text)
        return -1;
    memcpy(text, f[0], len[0]);
    text[len[0]] = '\0';

    char *end;
    long long v = strtoll(text, &end, 10);
    if (*end != '\0')
        return -1;
    return v < 0 ? 1 : v + 1;
}
