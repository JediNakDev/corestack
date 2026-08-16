/**
 * @file socket/socket.c
 * @brief One connection to the shared SocketRunner.
 */

#include "libtetrisdb/socket/conf.h"
#include "libtetrisdb/socket/db.h"
#include "libtetrisutil/rc.h"

#include "../wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DB_READY "<<READY>>"

struct db_socket
{
    db_wire_t wire;     /* the socket, and its line buffer */
    long long deadline; /* absolute monotonic ms; set at open */
    db_status_t
        failed; /* DB_OK, or the terminal status this conn is stuck on */
};

void db_socket_opts_load(db_socket_opts_t *opts)
{
    rc_get("db_ipc", DB_DEFAULT_IPC, opts->sock, sizeof(opts->sock));
    rc_get_int("db_timeout", DB_DEFAULT_TIMEOUT_MS, 0,
               DB_DEFAULT_TIMEOUT_MS * 10, &opts->timeout_ms);
}

static int connect_deadline(const char *path, long long deadline)
{
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
    {
        fprintf(stderr, "tetrisdb: socket path too long (%zu >= %zu): %s\n",
                strlen(path), sizeof(addr.sun_path), path);
        return -1;
    }
    memcpy(addr.sun_path, path, strlen(path));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        fprintf(stderr, "tetrisdb: socket: %s\n", strerror(errno));
        return -1;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    (void)fcntl(fd, F_SETFL, O_NONBLOCK);

#ifdef SO_NOSIGPIPE
    /* A runner that died between the connect and the write would otherwise
     * kill this process with SIGPIPE. The session process does ignore SIGPIPE,
     * but a library that hands its caller a dead process instead of an error
     * return is relying on the caller's signal disposition, which is not part
     * of any contract here. */
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        return fd;

    if (errno != EINPROGRESS)
    {
        fprintf(stderr, "tetrisdb: connect %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    struct pollfd p;
    int err = 0;
    socklen_t errlen = sizeof(err);

    for (;;)
    {
        long long left = deadline - db_now_ms();
        if (left <= 0)
        {
            fprintf(stderr, "tetrisdb: connect %s: deadline passed\n", path);
            close(fd);
            return -1;
        }
        p.fd = fd;
        p.events = POLLOUT;
        p.revents = 0;
        int r = poll(&p, 1, (int)left);
        if (r > 0)
            break;
        if (r == 0)
        {
            fprintf(stderr, "tetrisdb: connect %s: deadline passed\n", path);
            close(fd);
            return -1;
        }
        if (errno == EINTR)
            continue;
        fprintf(stderr, "tetrisdb: poll: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* A ready-for-write socket is not a connected one: the result of the
     * connect is in SO_ERROR, and reading it is the only way to see a refusal
     * that arrived asynchronously. */
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0)
    {
        fprintf(stderr, "tetrisdb: connect %s: %s\n", path,
                strerror(err != 0 ? err : errno));
        close(fd);
        return -1;
    }
    return fd;
}

db_socket_t *db_socket_open(const db_socket_opts_t *opts)
{
    if (opts == NULL || opts->sock[0] == '\0')
    {
        fprintf(stderr,
                "tetrisdb: no socket path set (see db_socket_opts_t.sock)\n");
        return NULL;
    }

    db_socket_t *c = calloc(1, sizeof(*c));
    if (c == NULL)
    {
        fprintf(stderr, "tetrisdb: out of memory\n");
        return NULL;
    }

    int ms = opts->timeout_ms > 0 ? opts->timeout_ms : DB_DEFAULT_TIMEOUT_MS;
    c->deadline = db_now_ms() + ms;
    c->failed = DB_OK;

    c->wire.fd = connect_deadline(opts->sock, c->deadline);
    if (c->wire.fd < 0)
    {
        free(c);
        return NULL;
    }

    /* greeting */
    char line[64];
    int r = db_wire_line(&c->wire, line, sizeof(line), c->deadline);
    if (r != DB_WIRE_LINE || strcmp(line, DB_READY) != 0)
    {
        fprintf(stderr, "tetrisdb: %s never sent %s (%s)\n", opts->sock,
                DB_READY,
                r == DB_WIRE_LATE  ? "deadline passed"
                : r == DB_WIRE_EOF ? "connection closed"
                : r == DB_WIRE_IO  ? strerror(errno)
                                   : "unexpected greeting");
        close(c->wire.fd);
        free(c);
        return NULL;
    }
    return c;
}

db_status_t db_socket_exec(db_socket_t *c, const char *sql, char *body,
                           size_t cap)
{
    if (body != NULL && cap > 0)
        body[0] = '\0';
    if (c == NULL)
        return DB_IO;
    if (c->failed != DB_OK)
        return c->failed;
    if (strpbrk(sql, "\n\r") != NULL)
        return DB_ERROR;

    int w = db_wire_write(c->wire.fd, sql, strlen(sql), c->deadline);
    if (w == 0)
        w = db_wire_write(c->wire.fd, "\n", 1, c->deadline);
    if (w != 0)
    {
        c->failed = (w == DB_WIRE_LATE) ? DB_TIMEOUT : DB_IO;
        return c->failed;
    }

    db_status_t st = db_wire_response(&c->wire, body, cap, c->deadline);
    if (st == DB_TIMEOUT || st == DB_IO)
        c->failed = st;
    return st;
}

void db_socket_close(db_socket_t *c)
{
    if (c == NULL)
        return;

    if (c->wire.fd >= 0)
        close(c->wire.fd);
    free(c);
}
