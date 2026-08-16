/**
 * @file schema.c
 * @brief Creating a table, and getting text safely into one.
 */

#include "libtetrisdb/schema.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DB_PAGE_SIZE 4096

void db_catalog_path(char *dst, size_t cap, const char *dir)
{
    snprintf(dst, cap, "%s/catalog.txt", dir != NULL ? dir : "");
}

static int catalog_has(const char *catalog, const char *name)
{
    FILE *f = fopen(catalog, "r");
    if (f == NULL)
        return 0; /* no catalog yet: nothing is defined */

    char line[1024];
    size_t want = strlen(name);
    int found = 0;

    while (!found && fgets(line, sizeof(line), f) != NULL)
    {
        char *paren = strchr(line, '(');
        if (paren == NULL)
            continue;

        char *start = line;
        while (*start == ' ' || *start == '\t')
            start++;
        char *end = paren;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        found =
            (size_t)(end - start) == want && strncmp(start, name, want) == 0;
    }

    fclose(f);
    return found;
}

/* Create every missing component of a directory path, like mkdir -p. */
int db_mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    struct stat st;

    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
    {
        fprintf(stderr, "tetrisdb: path too long: %s\n", path);
        return -1;
    }

    for (char *p = buf + 1; *p != '\0'; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) < 0 && errno != EEXIST)
            goto fail;
        *p = '/';
    }
    if (mkdir(buf, 0755) < 0 && errno != EEXIST)
        goto fail;

    if (stat(buf, &st) < 0 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "tetrisdb: not a directory: %s\n", buf);
        return -1;
    }
    return 0;

fail:
    fprintf(stderr, "tetrisdb: mkdir %s: %s\n", buf, strerror(errno));
    return -1;
}

/* Give a freshly created heap file one empty page. */
static int write_empty_page(int fd, const char *path)
{
    static const char zeros[DB_PAGE_SIZE];
    size_t left = sizeof(zeros);
    const char *p = zeros;

    while (left > 0)
    {
        ssize_t w = write(fd, p, left);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "tetrisdb: write %s: %s\n", path, strerror(errno));
            return -1;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }
    return 0;
}

int db_ensure_table(const char *dir, const char *name, const char *schema)
{
    char catalog[PATH_MAX + 16];
    char dat[PATH_MAX + 16];

    if (dir == NULL || dir[0] == '\0')
    {
        fprintf(stderr,
                "tetrisdb: no data directory set for table '%s'"
                " (each daemon must own one)\n",
                name);
        return -1;
    }

    if (db_mkdir_p(dir) < 0)
        return -1;

    db_catalog_path(catalog, sizeof(catalog), dir);
    snprintf(dat, sizeof(dat), "%s/%s.dat", dir, name);

    int fd = open(dat, O_WRONLY | O_CREAT | O_EXCL, 0640);
    if (fd >= 0)
    {
        int rc = write_empty_page(fd, dat);
        close(fd);
        if (rc < 0)
        {
            unlink(dat); /* half a page is worse than no table at all */
            return -1;
        }
    }
    else if (errno != EEXIST)
    {
        fprintf(stderr, "tetrisdb: create %s: %s\n", dat, strerror(errno));
        return -1;
    }

    if (catalog_has(catalog, name))
        return 0; /* already declared - leave the existing schema alone */

    FILE *f = fopen(catalog, "a");
    if (f == NULL)
    {
        fprintf(stderr, "tetrisdb: open %s: %s\n", catalog, strerror(errno));
        return -1;
    }
    if (fprintf(f, "%s (%s)\n", name, schema) < 0 || fclose(f) != 0)
    {
        fprintf(stderr, "tetrisdb: write %s: %s\n", catalog, strerror(errno));
        return -1;
    }
    return 0;
}

char *db_quote(char *dst, size_t cap, const char *src)
{
    size_t w = 0;

    if (cap == 0)
        return dst;

    /* Reserve room for the closing quote and NUL up front */
    if (cap < 3)
    {
        snprintf(dst, cap, "%s", "");
        return dst;
    }

    dst[w++] = '\'';
    /* w = buffer write, '->'' so 2 and final 2 for ' and \0 */
    while (*src && w + (*src == '\'' ? 2 : 1) + 2 <= cap)
    {
        if (*src == '\'')
            dst[w++] = '\'';
        dst[w++] = *src++;
    }
    dst[w++] = '\'';
    dst[w] = '\0';
    return dst;
}
