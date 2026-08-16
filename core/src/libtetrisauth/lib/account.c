#include "auth.h"
#include "hex.h"
#include "libtetrisauth/auth.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/db.h"
#include "libtetrisdb/status.h"
#include "libtetrisutil/limits.h"
#include "libtetrisutil/logmsg.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BODY_MAX 4096

#define CRED_PASS_MIN 8
#define CRED_PASS_MAX 128

#define REG_SEM_NAME "/tetrish_register"
#define REG_WAIT_MS 5000

#define REG_POLL_MIN_US 1000
#define REG_POLL_SPAN_US 4000

#define TXN_ATTEMPTS 3
#define TXN_BACKOFF_US 5000

#define SALT_HEX_LEN 32
#define DIGEST_HEX_LEN 64

#define HASH_SALT_BYTES 16
#define HASH_DIGEST_BYTES 32

/** A quoted username: two quotes, a NUL, and room for the doubling
 *db_quote() applies to any inner quote. */
#define QUOTED_MAX (MAX_USER_NAME * 2 + 4)

int password_hash(const acc_t *c, const char *salt_hex, size_t salt_hex_len,
                  int iters, char *out_hex, size_t cap)
{
    log_send(LOG_DEBUG, "deriving credential hash salt_len=%zu iterations=%d",
             salt_hex_len, iters);
    unsigned char salt[HASH_SALT_BYTES];
    unsigned char digest[HASH_DIGEST_BYTES];
    int rc = -1;

    if (out_hex == NULL || cap < HASH_DIGEST_BYTES * 2 + 1 || iters < 1)
    {
        (void)log_send(LOG_INFO,
                       "operation=password_hash phase=complete status=-1");
        return -1;
    }

    int salt_len = hex_decode(salt_hex, salt_hex_len, salt, sizeof salt);
    if (salt_len <= 0)
    {
        (void)log_send(LOG_INFO,
                       "operation=password_hash phase=complete status=-1");
        return -1;
    }

    if (PKCS5_PBKDF2_HMAC(c->pass, (int)c->pass_len, salt, salt_len, iters,
                          EVP_sha256(), (int)sizeof digest, digest) != 1)
        goto done;

    hex_encode(digest, sizeof digest, out_hex);
    rc = 0;

done:
    /* The digest is not the password, but it is the thing an attacker with the
     * stack wants next, and this frame is about to be reused by the reply
     * path. */
    OPENSSL_cleanse(digest, sizeof digest);
    OPENSSL_cleanse(salt, sizeof salt);
    (void)log_send(LOG_INFO, "operation=password_hash phase=complete status=%d",
                   rc);
    return rc;
}

static unsigned poll_delay_us(void)
{
    return REG_POLL_MIN_US + (unsigned)getpid() % REG_POLL_SPAN_US;
}

static db_socket_t *conn_open(void)
{
    db_socket_opts_t opts;
    const auth_conf_t *conf = auth_conf();

    db_socket_opts_load(&opts);
    snprintf(opts.sock, sizeof opts.sock, "%s", conf->db_sock);
    opts.timeout_ms = conf->db_timeout_ms;

    return db_socket_open(&opts);
}

static int copy_text(const char *src, size_t len, char *dst, size_t cap)
{
    if (len == 0 || len >= cap)
        return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

int parse_login_db_result(const db_status_t st, const char *body,
                          const char **f, size_t *l, int *iters, long long *uid)
{
    size_t len[4];
    int rows, fields;
    char text[32];

    if (st != DB_OK)
    {
        log_send(LOG_DEBUG, "auth: login query failed (tdb status %d)",
                 (int)st);
        return 500;
    }

    rows = db_row_count(body);
    if (rows < 0)
    {
        log_send(LOG_DEBUG, "auth: the login reply carried no table");
        return 500;
    }
    if (rows == 0)
    {
        return 404;
    }

    fields = db_row_fields(body, 0, f, len, 4);
    if (fields != 4)
    {
        log_send(LOG_DEBUG, "auth: a user row has %d fields, expected 4",
                 fields);
        return 500;
    }
    if (len[1] != SALT_HEX_LEN || len[2] != DIGEST_HEX_LEN)
    {
        log_send(LOG_DEBUG, "auth: a user row has a malformed salt or digest");
        return 500;
    }

    if (copy_text(f[0], len[0], text, sizeof text) != 0)
    {
        log_send(LOG_DEBUG, "auth: a user row has an unreadable id");
        return 500;
    }
    *uid = strtoll(text, NULL, 10);

    if (copy_text(f[3], len[3], text, sizeof text) != 0)
    {
        log_send(LOG_DEBUG,
                 "auth: a user row has an unreadable iteration count");
        return 500;
    }

    *iters = (int)strtol(text, NULL, 10);
    if (*iters < 1)
    {
        log_send(LOG_DEBUG, "auth: a user row has an unusable iteration count");
        return 500;
    }
    *l = len[1];
    return 200;
}

int account_login(const char *name, const acc_t *c, long long *id)
{
    log_send(LOG_DEBUG, "account login lookup user=%s", name);
    char quoted[QUOTED_MAX];
    char sql[256];
    char body[BODY_MAX];
    char want[DIGEST_HEX_LEN + 1];
    const char *f[4];
    int status = 500;
    db_socket_t *conn;
    db_status_t st;
    int match;
    int iters;
    long long uid;
    size_t l;

    db_quote(quoted, sizeof quoted, name);
    snprintf(sql, sizeof sql,
             "select id, salt, digest, iters from " TETRISAUTH_DB_TABLE
             " where name = %s;",
             quoted);

    conn = conn_open();
    if (conn == NULL)
        goto done;

    st = db_socket_exec(conn, sql, body, sizeof body);
    if (st == DB_RETRY)
        st = db_socket_exec(conn, sql, body, sizeof body);
    db_socket_close(conn);

    status = parse_login_db_result(st, body, f, &l, &iters, &uid);
    if (status != 200)
        goto done;

    if (password_hash(c, f[1], l, iters, want, sizeof want) != 0)
    {
        log_send(LOG_DEBUG, "auth: PBKDF2 failed on the login path");
        return 500;
    }
    match = CRYPTO_memcmp(want, f[2], DIGEST_HEX_LEN) == 0;
    OPENSSL_cleanse(want, sizeof want);

    if (!match)
    {
        status = 401;
        goto done;
    }

    *id = uid;
    status = 200;

done:
    (void)log_send(LOG_INFO, "operation=account_login phase=complete status=%d",
                   status);
    return status;
}

static db_status_t register_txn(db_socket_t *conn, const char *quoted,
                                const char *salt_hex, const char *digest_hex,
                                int iters, int *taken, long long *id)
{
    log_send(LOG_DEBUG, "account registration transaction iterations=%d",
             iters);
    char sql[512];
    char body[BODY_MAX];
    db_status_t st;
    int rows;
    long long next;

    *taken = 0;

    st = db_socket_exec(conn, "set transaction read write;", NULL, 0);
    if (st != DB_OK)
        goto done;

    st = db_socket_exec(conn, "select max(id) from " TETRISAUTH_DB_TABLE ";",
                        body, sizeof body);
    log_send(LOG_DEBUG, "registration transaction allocated-id query status=%d",
             st);
    if (st != DB_OK)
        goto done;

    next = db_next_id(body);
    if (next < 0)
    {
        log_send(LOG_DEBUG,
                 "auth: could not read max(id) from the registration "
                 "reply");
        st = DB_ERROR;
        goto done;
    }

    snprintf(sql, sizeof sql,
             "select id from " TETRISAUTH_DB_TABLE " where name = %s;", quoted);
    st = db_socket_exec(conn, sql, body, sizeof body);
    if (st != DB_OK)
        goto done;

    rows = db_row_count(body);
    if (rows < 0)
    {
        log_send(LOG_DEBUG, "auth: the existence check carried no table");
        st = DB_ERROR;
        goto done;
    }
    if (rows > 0)
    {
        (void)db_socket_exec(conn, "rollback;", NULL, 0);
        *taken = 1;
        st = DB_OK;
        goto done;
    }

    snprintf(sql, sizeof sql,
             "insert into " TETRISAUTH_DB_TABLE
             " values (%lld, %s, '%s', '%s', %d, %lld);",
             next, quoted, salt_hex, digest_hex, iters, (long long)time(NULL));
    st = db_socket_exec(conn, sql, NULL, 0);
    if (st != DB_OK)
        goto done;

    st = db_socket_exec(conn, "commit;", NULL, 0);
    if (st != DB_OK)
        goto done;

    *id = next;

done:
    (void)log_send(LOG_INFO, "operation=register_txn phase=complete status=%d",
                   st);
    return st;
}

static sem_t *reg_acquire(void)
{
    sem_t *sem = sem_open(REG_SEM_NAME, O_CREAT, 0600, 1);
    if (sem == SEM_FAILED)
    {
        log_send(LOG_DEBUG, "auth: sem_open(" REG_SEM_NAME ") failed: %s",
                 strerror(errno));
        return NULL;
    }

    for (int waited_us = 0; waited_us < REG_WAIT_MS * 1000;)
    {
        if (sem_trywait(sem) == 0)
            return sem;

        unsigned nap = poll_delay_us();
        usleep(nap);
        waited_us += (int)nap;
    }

    log_send(LOG_DEBUG,
             "auth: no turn at " REG_SEM_NAME " within %d ms; registration "
             "answered 500",
             REG_WAIT_MS);
    sem_close(sem);
    return NULL;
}

static void reg_release(sem_t *sem)
{
    if (sem == NULL)
        return;
    sem_post(sem);
    sem_close(sem);
}

int gen_new_salt(char *out_hex, size_t cap)
{
    unsigned char salt[HASH_SALT_BYTES];

    if (out_hex == NULL || cap < HASH_SALT_BYTES * 2 + 1)
        return -1;
    if (RAND_bytes(salt, sizeof salt) != 1)
        return -1;

    hex_encode(salt, sizeof salt, out_hex);
    return 0;
}

int account_register(const char *name, const acc_t *c, long long *id)
{
    log_send(LOG_DEBUG, "account registration requested user=%s", name);
    if ((c->pass_len < CRED_PASS_MIN || c->pass_len > CRED_PASS_MAX))
    {
        return 400;
    }
    const auth_conf_t *conf = auth_conf();
    char quoted[QUOTED_MAX];
    char salt_hex[SALT_HEX_LEN + 1];
    char digest_hex[DIGEST_HEX_LEN + 1];
    int status = 500;
    sem_t *sem = NULL;
    db_socket_t *conn = NULL;

    db_quote(quoted, sizeof quoted, name);

    if (gen_new_salt(salt_hex, sizeof salt_hex) != 0)
    {
        log_send(LOG_DEBUG, "auth: RAND_bytes failed while making a salt");
        goto done;
    }

    if (password_hash(c, salt_hex, strlen(salt_hex), conf->pbkdf2_iters,
                      digest_hex, sizeof digest_hex) != 0)
    {
        log_send(LOG_DEBUG, "auth: PBKDF2 failed on the registration path");
        goto done;
    }

    sem = reg_acquire();
    if (sem == NULL)
        goto done;

    conn = conn_open();
    if (conn == NULL)
        goto done;

    for (int attempt = 0; attempt < TXN_ATTEMPTS; attempt++)
    {
        int taken = 0;
        db_status_t st = register_txn(conn, quoted, salt_hex, digest_hex,
                                      conf->pbkdf2_iters, &taken, id);

        if (st == DB_RETRY)
        {
            usleep(TXN_BACKOFF_US);
            continue;
        }
        if (st != DB_OK)
        {
            log_send(LOG_DEBUG, "auth: registration failed (tdb status %d)",
                     (int)st);
            break;
        }
        status = taken ? 409 : 200;
        break;
    }

done:
    if (conn != NULL)
        db_socket_close(conn);
    reg_release(sem);
    (void)log_send(LOG_INFO,
                   "operation=account_register phase=complete status=%d",
                   status);
    return status;
}
