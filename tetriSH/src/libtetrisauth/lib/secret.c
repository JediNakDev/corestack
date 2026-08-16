/**
 * @file secret.c
 * @brief Reads the JWT signing secret that `make secret` put on disk.
 */

#include "secret.h"
#include "libtetrisutil/logmsg.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_SECRET_REL "auth/jwt_secret" /**< Path under root; see @file. */
#define AUTH_SECRET_MIN 32 /**< Smallest secret accepted, in bytes. */
#define AUTH_SECRET_MAX 64 /**< Largest secret accepted, in bytes. */

static int secret_open(const char *path, long *len)
{
    struct stat st;
    const char *defect;

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
    {
        log_send(LOG_ERROR, "%s: open: %s (run `make secret`)", path,
                 strerror(errno));
        fprintf(stderr, "%s: open: %s (run `make secret`)\n", path,
                strerror(errno));
        return -1;
    }

    if (fstat(fd, &st) != 0)
    {
        log_send(LOG_ERROR, "%s: fstat: %s", path, strerror(errno));
        fprintf(stderr, "%s: fstat: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (!S_ISREG(st.st_mode))
        defect = "not a regular file";
    else if (st.st_mode & 077)
        defect = "group- or other-accessible";
    else if (st.st_uid != geteuid())
        defect = "not owned by this process";
    else if (st.st_size < AUTH_SECRET_MIN || st.st_size > AUTH_SECRET_MAX)
        defect = "wrong size";
    else
        defect = NULL;

    if (defect != NULL)
    {
        log_send(LOG_ERROR, "%s: %s (mode 0%o, uid %u, %lld bytes)", path,
                 defect, (unsigned)(st.st_mode & 0777), (unsigned)st.st_uid,
                 (long long)st.st_size);
        fprintf(stderr, "%s: %s (mode 0%o, uid %u, %lld bytes)\n", path,
                defect, (unsigned)(st.st_mode & 0777), (unsigned)st.st_uid,
                (long long)st.st_size);
        close(fd);
        return -1;
    }

    int fl = fcntl(fd, F_GETFL);
    if (fl != -1)
        (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

    *len = (long)st.st_size;
    return fd;
}

static int secret_read(int fd, const char *path, long len, unsigned char *out,
                       size_t outlen)
{
    if ((size_t)len > outlen)
    {
        log_send(LOG_ERROR,
                 "%s: %ld bytes does not fit the caller's %zu-byte buffer",
                 path, len, outlen);
        return -1;
    }

    size_t total = 0;
    while (total < (size_t)len)
    {
        ssize_t n = read(fd, out + total, (size_t)len - total);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            log_send(LOG_ERROR, "%s: read: %s", path, strerror(errno));
            return -1;
        }
        if (n == 0)
        {
            log_send(LOG_ERROR, "%s: short read (%zu of %ld bytes)", path,
                     total, len);
            return -1;
        }
        total += (size_t)n;
    }

    return (int)total;
}

static int secret_path(const char *root, char *path, size_t cap)
{
    int n = snprintf(path, cap, "%s/" AUTH_SECRET_REL, root);
    if (n <= 0 || (size_t)n >= cap)
    {
        log_send(LOG_ERROR, "auth/jwt_secret: path too long for root '%s'",
                 root);
        return -1;
    }
    return 0;
}

int auth_secret_load(const char *root, unsigned char *out, size_t outlen)
{
    char path[PATH_MAX];
    long len;

    if (secret_path(root, path, sizeof path) != 0)
        return -1;

    int fd = secret_open(path, &len);
    if (fd < 0)
        return -1;

    int rc = secret_read(fd, path, len, out, outlen);
    close(fd);
    return rc;
}

/* Creates auth/jwt_secret with AUTH_SECRET_MIN fresh random bytes when
 * nothing is there yet, then validates whatever is there (freshly written or
 * pre-existing) through the same checks auth_secret_load() applies. A secret
 * that exists but fails those checks is refused, never rewritten - repairing
 * it silently would paper over exactly the misconfiguration those checks
 * exist to catch. */
int auth_secret_provision(const char *root)
{
    char path[PATH_MAX];
    struct stat st;

    if (secret_path(root, path, sizeof path) != 0)
        return -1;

    if (stat(path, &st) != 0 && errno == ENOENT)
    {
        char dir[PATH_MAX];
        if (snprintf(dir, sizeof dir, "%s/auth", root) >= (int)sizeof dir)
        {
            log_send(LOG_ERROR, "auth/jwt_secret: path too long for root '%s'",
                     root);
            return -1;
        }
        if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        {
            log_send(LOG_ERROR, "%s: mkdir: %s", dir, strerror(errno));
            return -1;
        }

        unsigned char buf[AUTH_SECRET_MIN];
        if (RAND_bytes(buf, sizeof buf) != 1)
        {
            log_send(LOG_ERROR, "%s: could not generate a secret", path);
            return -1;
        }

        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
        {
            log_send(LOG_ERROR, "%s: open: %s", path, strerror(errno));
            OPENSSL_cleanse(buf, sizeof buf);
            return -1;
        }
        size_t written = 0;
        int write_ok = 1;
        while (written < sizeof buf)
        {
            ssize_t w = write(fd, buf + written, sizeof buf - written);
            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                log_send(LOG_ERROR, "%s: write: %s", path, strerror(errno));
                write_ok = 0;
                break;
            }
            written += (size_t)w;
        }
        OPENSSL_cleanse(buf, sizeof buf);
        close(fd);
        if (!write_ok)
            return -1;
        log_send(LOG_INFO, "generated %s (%d bytes, mode 0600)", path,
                 AUTH_SECRET_MIN);
    }

    long len;
    int fd = secret_open(path, &len);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}
