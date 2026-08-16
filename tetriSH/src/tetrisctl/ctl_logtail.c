#include "tetrisctl/ctl_logtail.h"

#include "libtetrisutil/rc.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_LOG_PATH "var/log/tetrisd.log"

/* Read at most this much past the end of the file that existed at open time. */
#define LOGTAIL_SEEK_BACK (64 * 1024)

/* One read() per poll is capped to this many bytes. */
#define LOGTAIL_READ_CHUNK 8192

struct logtail
{
    char path[PATH_MAX];
    int fd;       /* -1 when not open (missing, or not yet tried) */
    off_t offset; /* bytes already consumed from the current fd */
    dev_t dev;    /* identity of the fd's inode, so a same-size replacement */
    ino_t ino;    /* at the same path (rotation) is still detected */
    bool missing;

    /* A line seen only up to its first '\n' this poll is held here rather
     * than emitted, so it is picked up whole on a later poll instead of
     * appearing twice or truncated. */
    char partial[LOGTAIL_LINE_LEN];
    size_t partial_len;

    char lines[LOGTAIL_RING_LINES][LOGTAIL_LINE_LEN];
    int count; /* populated ring slots, saturates at LOGTAIL_RING_LINES */
    int next;  /* ring slot the next completed line writes to */
};

/** Appends one completed line to the ring, evicting the oldest once full.
 * Called by logtail_poll() at every '\n' it finds. */
static void ring_push(logtail_t *t, const char *line)
{
    snprintf(t->lines[t->next], LOGTAIL_LINE_LEN, "%s", line);
    t->next = (t->next + 1) % LOGTAIL_RING_LINES;
    if (t->count < LOGTAIL_RING_LINES)
        t->count++;
}

static int open_at(logtail_t *t, off_t start_offset)
{
    int fd = open(t->path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0)
    {
        close(fd);
        return -1;
    }

    t->fd = fd;
    t->offset = start_offset;
    t->dev = st.st_dev;
    t->ino = st.st_ino;
    t->partial_len = 0;
    t->missing = false;
    return 0;
}

static void try_open_log_file(logtail_t *t)
{
    if (t->fd >= 0)
        return;

    struct stat st;
    off_t size = (stat(t->path, &st) == 0) ? st.st_size : 0;
    off_t start = size > LOGTAIL_SEEK_BACK ? size - LOGTAIL_SEEK_BACK : 0;

    if (open_at(t, start) != 0)
        t->missing = true;
}

logtail_t *logtail_init_at(const char *path)
{
    logtail_t *t = calloc(1, sizeof *t);
    if (t == NULL)
        return NULL;

    snprintf(t->path, sizeof t->path, "%s", path);
    t->fd = -1;
    t->missing = true; /* corrected by the first logtail_poll() */
    return t;
}

logtail_t *logtail_init(void)
{
    char path[PATH_MAX];
    (void)rc_get("log_path", DEFAULT_LOG_PATH, path, sizeof path);
    return logtail_init_at(path);
}

void logtail_poll(logtail_t *t)
{
    try_open_log_file(t);
    if (t->fd < 0)
        return;

    struct stat fd_st;
    if (fstat(t->fd, &fd_st) != 0)
    {
        close(t->fd);
        t->fd = -1;
        t->missing = true;
        return;
    }

    /*
     * t->offset is the number of bytes that the log tail has already read.
     *
     * If the open file is now smaller than that offset, another process erased
     * or shortened the file. The log tail must start again at byte zero.
     */
    bool file_truncated = fd_st.st_size < t->offset;

    /*
     * t->dev and t->ino identify the open log file.
     * path_st identifies the file that is now at the configured log path.
     *
     * If these identities differ, log rotation replaced the old file with a new
     * file. The existing file descriptor does not automatically change to the
     * new file. Therefore, the log tail must close the old file and open the
     * current file at the path.
     */
    struct stat path_st;
    bool file_rotated = stat(t->path, &path_st) == 0 &&
                        (path_st.st_dev != t->dev || path_st.st_ino != t->ino);

    if (file_truncated || file_rotated)
    {
        close(t->fd);
        t->fd = -1;

        if (open_at(t, 0) != 0)
        {
            t->missing = true;
            return;
        }
        if (fstat(t->fd, &fd_st) != 0)
        {
            close(t->fd);
            t->fd = -1;
            t->missing = true;
            return;
        }
    }

    if (fd_st.st_size <= t->offset)
        return; /* nothing new */

    char chunk[LOGTAIL_READ_CHUNK];
    ssize_t r = pread(t->fd, chunk, sizeof chunk, t->offset);
    if (r < 0)
    {
        close(t->fd);
        t->fd = -1;
        t->missing = true;
        return;
    }
    if (r == 0)
        return; /* size check raced a truncation; try again next poll
                 */

    t->offset += r;

    for (ssize_t i = 0; i < r; i++)
    {
        char c = chunk[i];
        if (c == '\n')
        {
            t->partial[t->partial_len] = '\0';
            ring_push(t, t->partial);
            t->partial_len = 0;
        }
        else if (t->partial_len + 1 < sizeof t->partial)
        {
            t->partial[t->partial_len++] = c;
        }
        /* else: this line is longer than LOGTAIL_LINE_LEN - 1. truncate itƒ. */
    }
}

bool is_logtail_missing(const logtail_t *t)
{
    return t->missing;
}

int logtail_lines(const logtail_t *t, const char *out[], int max)
{
    int n = t->count < max ? t->count : max;

    /* overwrite oldest first. */
    int skip = t->count - n;
    int start = t->count < LOGTAIL_RING_LINES ? 0 : t->next;

    for (int i = 0; i < n; i++)
    {
        int idx = (start + skip + i) % LOGTAIL_RING_LINES;
        out[i] = t->lines[idx];
    }
    return n;
}

void logtail_close(logtail_t *t)
{
    if (t == NULL)
        return;
    if (t->fd >= 0)
        close(t->fd);
    free(t);
}
