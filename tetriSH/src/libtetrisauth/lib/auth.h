#ifndef LIBTETRISAUTH_AUTH_PRIV_H
#define LIBTETRISAUTH_AUTH_PRIV_H

#include <limits.h>
#include <time.h>
#include "libtetrisutil/limits.h"
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int max_attempts;           /**< auth_max_attempts, 1..100. */
    int token_ttl;              /**< auth_token_ttl, 60..31536000 seconds. */
    int pbkdf2_iters;           /**< auth_pbkdf2_iters, 1..10000000. */
    int db_timeout_ms;          /**< db_timeout, in milliseconds. */
    char db_sock[MAX_IPC_PATH]; /**< db_ipc, used as written. */
} auth_conf_t;

typedef struct
{
    const char *user;
    size_t user_len;
    const char *pass;
    size_t pass_len;
} acc_t;

const auth_conf_t *auth_conf(void);

int auth_conf_load(void);

int account_login(const char *name, const acc_t *c, long long *id);

int account_register(const char *name, const acc_t *c,
                               long long *id);

#define TOKEN_SECRET_MIN_LEN 32

#define TOKEN_NAME_MAX 15
#define TOKEN_SIG_LEN 32
#define TOKEN_SIG_HEX_LEN 64
#define TOKEN_MAX_LEN 256

typedef struct
{
    long long sub;                 /**< User id. */
    char name[TOKEN_NAME_MAX + 1]; /**< 15 characters plus NUL, #47's cap. */
    long long iat; /**< Issued at; an input to token_mint(), not computed. */
    long long exp; /**< Expiry; likewise. */
} token_claims_t;

typedef enum
{
    TOKEN_OK = 0,
    TOKEN_E_MALFORMED, /**< Not five fields, or a MAC of the wrong width. */
    TOKEN_E_SIGNATURE, /**< MAC mismatch. */
    TOKEN_E_CLAIMS,    /**< A claim missing, malformed or out of range. */
    TOKEN_E_EXPIRED
} token_result_t;

int token_mint(char *out, size_t out_len, const token_claims_t *claims,
               const unsigned char *secret, size_t secret_len);

token_result_t token_verify(const char *token, const unsigned char *secret,
                            size_t secret_len, time_t now,
                            token_claims_t *out);

#endif /* LIBTETRISAUTH_AUTH_PRIV_H */
