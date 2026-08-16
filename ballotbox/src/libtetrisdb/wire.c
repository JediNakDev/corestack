/**
 * @file wire.c
 * @brief The line-oriented protocol both runners speak.
 *
 * "Write a line, read until the end marker" is the whole of it
 * (db/docs/c-daemon-integration.md section 4), and it is identical over
 * PipeRunner's pipes and SocketRunner's socket. This file is that, over a
 * plain fd, with a deadline that callers who have one can pass and callers who
 * do not can leave off.
 *
 * The deadline is threaded through every wait rather than armed once with a
 * timer, because it must bound the SUM of the waits, not each one: a runner
 * that dribbles out a line every second would satisfy any per-read timeout
 * forever.
 */

#include "wire.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Response markers, exactly as the runners write them. */
#define DB_MARK_END "<<END "
#define DB_MARK_OK "<<END ok>>"
#define DB_MARK_RETRY "<<END retry>>"

long long db_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wait until fd is ready for events, or the deadline passes. */
static int wait_ready(int fd, short events, long long deadline)
{
    for (;;)
    {
        int timeout = -1;

        if (deadline != DB_NO_DEADLINE)
        {
            long long left = deadline - db_now_ms();
            if (left <= 0)
                return DB_WIRE_LATE;
            timeout = (int)left;
        }

        struct pollfd p;
        p.fd = fd;
        p.events = events;
        p.revents = 0;

        int r = poll(&p, 1, timeout);
        if (r > 0)
            return 0;
        if (r == 0)
            return DB_WIRE_LATE;
        if (errno == EINTR)
            continue;
        return DB_WIRE_IO;
    }
}

int db_wire_line(db_wire_t *w, char *out, size_t cap, long long deadline)
{
    size_t n = 0;

    for (;;)
    {
        if (w->pos == w->len)
        {
            int ready = wait_ready(w->fd, POLLIN, deadline);
            if (ready != 0)
                return ready;

            ssize_t r = read(w->fd, w->buf, sizeof(w->buf));
            if (r < 0)
            {
                /*
                 * EINTR        interrupted by signal
                 * EAGAIN       no data right
                 * EWOULDBLOCK  same general idea for nonblocking fd
                 */
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return DB_WIRE_IO;
            }
            if (r == 0)
            {
                /* EOF mid-line is not a line: this reads '\n'-terminated
                 * lines only, and a peer that closes before the terminator
                 * leaves nothing well-formed to hand back - the caller must
                 * see this as the connection ending, not as one more line. */
                return DB_WIRE_EOF;
            }
            w->len = (size_t)r;
            w->pos = 0;
        }

        while (w->pos < w->len)
        {
            char c = w->buf[w->pos++];
            if (c == '\n')
            {
                out[n] = '\0';
                return DB_WIRE_LINE;
            }
            if (n + 1 < cap)
                out[n++] = c;
        }
    }
}

int db_wire_write(int fd, const char *data, size_t len, long long deadline)
{
    while (len > 0)
    {
        ssize_t w = write(fd, data, len);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                int ready = wait_ready(fd, POLLOUT, deadline);
                if (ready != 0)
                    return ready;
                continue;
            }
            return DB_WIRE_IO;
        }
        data += (size_t)w;
        len -= (size_t)w;
    }
    return 0;
}

/* Which marker is this line? Called only on a line already known to start with
 * "<<END ", so anything unrecognised is a failure the caller must not retry. */
static db_status_t classify(const char *line)
{
    if (strcmp(line, DB_MARK_OK) == 0)
        return DB_OK;
    if (strcmp(line, DB_MARK_RETRY) == 0)
        return DB_RETRY;
    return DB_ERROR;
}

db_status_t db_wire_response(db_wire_t *w, char *body, size_t cap,
                             long long deadline)
{
    char line[DB_BODY_MAX];
    size_t used = 0;

    if (body != NULL && cap > 0)
        body[0] = '\0';

    for (;;)
    {
        int r = db_wire_line(w, line, sizeof(line), deadline);
        if (r == DB_WIRE_LATE)
            return DB_TIMEOUT;
        if (r != DB_WIRE_LINE)
            return DB_IO; /* the runner died mid-statement */

        if (strncmp(line, DB_MARK_END, sizeof(DB_MARK_END) - 1) == 0)
            return classify(line);

        /* Lines are kept as lines: a select reply's row structure is what
         * db_row_fields() reads, and flattening would destroy it. */
        if (body != NULL && used + 1 < cap)
        {
            size_t room = cap - used;
            int n =
                snprintf(body + used, room, "%s%s", used > 0 ? "\n" : "", line);
            used += (n < 0 || (size_t)n >= room) ? room - 1 : (size_t)n;
        }
    }
}
