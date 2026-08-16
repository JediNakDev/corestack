#include "libtetrisutil/logmsg.h"

#include "libtetrisutil/rc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Process-wide sender state. -1 fd means "not open": every send is a drop. */
static int glb_fd = -1;
static char glb_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static _Atomic unsigned long glb_dropped;    /**< Lifetime drops */
static _Atomic unsigned long glb_unreported; /**< Current unreported drops */
static int glb_max_attempts; /* overridable via log_send_attempts in rc */
/* trylock-only on send paths: contention drops a record instead of making a
 * game-critical caller wait. It protects fd reconnect/close and glb_path. */
static pthread_mutex_t glb_sender_lock = PTHREAD_MUTEX_INITIALIZER;

int log_level_rank(log_level_t level)
{
    switch (level)
    {
    case LOG_DEBUG:
        return 0;
    case LOG_INFO:
        return 1;
    case LOG_WARN:
        return 2;
    case LOG_ERROR:
        return 3;
    }
    return 4; /* unknown: ranks above ERROR so filtering never hides it */
}

int log_level_parse(const char *name, log_level_t *out)
{
    static const struct
    {
        const char *name;
        log_level_t level;
    } names[] = {
        {"info", LOG_INFO},
        {"warn", LOG_WARN},
        {"error", LOG_ERROR},
        {"debug", LOG_DEBUG},
    };

    if (name == NULL || out == NULL)
        return -1;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strcasecmp(name, names[i].name) == 0)
        {
            *out = names[i].level;
            return 0;
        }
    }
    return -1;
}

static int sender_connect_socket(void)
{
    struct sockaddr_un addr;

    if (glb_path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        /* O_NONBLOCK => a send() on this socket never blocks the caller */
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    /* FD_CLOEXEC => the fd is automatically closed if this process later
     * execs a child */
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, glb_path, strlen(glb_path) + 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    glb_fd = fd;
    return 0;
}

static void sender_disconnect_socket(void)
{
    if (glb_fd >= 0)
    {
        close(glb_fd);
        glb_fd = -1;
    }
}

int log_open(const char *socket_path)
{
    if (socket_path == NULL || socket_path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }
    if (strlen(socket_path) >= sizeof(glb_path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    /* Tear down existing connection first. */
    log_close();
    pthread_mutex_lock(&glb_sender_lock);
    memcpy(glb_path, socket_path, strlen(socket_path) + 1);
    (void)rc_get_int("log_send_attempts", 2, 1, 1000, &glb_max_attempts);
    int rc = sender_connect_socket();
    pthread_mutex_unlock(&glb_sender_lock);
    return rc;
}

void log_close(void)
{
    unsigned long unreported = atomic_load(&glb_unreported);
    if (unreported > 0)
        (void)log_send(LOG_WARN, "sender exiting with %lu dropped records",
                       unreported);
    pthread_mutex_lock(&glb_sender_lock);
    sender_disconnect_socket();
    pthread_mutex_unlock(&glb_sender_lock);
}

unsigned long get_log_dropped(void)
{
    return atomic_load(&glb_dropped);
}

static int drop_one(void)
{
    atomic_fetch_add(&glb_dropped, 1);
    unsigned long old = atomic_load(&glb_unreported);
    while (old < ULONG_MAX &&
           !atomic_compare_exchange_weak(&glb_unreported, &old, old + 1))
        ;
    return -1;
}

int log_vsend(log_level_t level, const char *fmt, va_list ap)
{
    log_msg_t m;

    if (fmt == NULL)
        return drop_one();

    memset(&m, 0, sizeof(m));
    m.pid = getpid();
    m.level = level;
    /* Piggyback the outstanding drop report. */
    /* vsnprintf always NUL-terminates and truncates */
    (void)vsnprintf(m.msg, sizeof(m.msg), fmt, ap);

    /* Never wait behind another sender. A contended logger is handled exactly
     * like a full non-blocking socket: account the drop and return. */
    if (pthread_mutex_trylock(&glb_sender_lock) != 0)
        return drop_one();

    m.dropped = atomic_load(&glb_unreported);

    if (glb_fd < 0 && sender_connect_socket() < 0)
    {
        pthread_mutex_unlock(&glb_sender_lock);
        return drop_one();
    }

    for (int attempt = 0; attempt < glb_max_attempts; attempt++)
    {
        if (send(glb_fd, &m, sizeof(m), 0) == (ssize_t)sizeof(m))
        {
            atomic_fetch_sub(&glb_unreported, m.dropped);
            pthread_mutex_unlock(&glb_sender_lock);
            return 0;
        }

        /* This whitelist is "the peer/socket went away" errors */
        if (errno != ECONNRESET && errno != ECONNREFUSED && errno != ENOENT &&
            errno != ENOTCONN && errno != EPIPE)
            break;

        sender_disconnect_socket();
        if (sender_connect_socket() < 0)
            break;
    }

    pthread_mutex_unlock(&glb_sender_lock);
    return drop_one();
}

int log_send(log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = log_vsend(level, fmt, ap);
    va_end(ap);
    return rc;
}
