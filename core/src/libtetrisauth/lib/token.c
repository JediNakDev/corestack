#include "auth.h"
#include "hex.h"
#include "libtetrisutil/logmsg.h"
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <string.h>

#define TOKEN_FIELD_SEP '\n'
#define TOKEN_FIELD_COUNT 5 /**< sub, name, iat, exp, mac. */

static int hmac_sha256(unsigned char mac[TOKEN_SIG_LEN],
                       const unsigned char *secret, size_t secret_len,
                       const void *msg, size_t msg_len)
{
    unsigned int mac_len = 0;
    if (HMAC(EVP_sha256(), secret, (int)secret_len, (const unsigned char *)msg,
             msg_len, mac, &mac_len) == NULL)
        return -1;
    return mac_len == TOKEN_SIG_LEN ? 0 : -1;
}

static int is_name_ok(const char *s, size_t len)
{
    if (len == 0 || len > TOKEN_NAME_MAX)
        return 0;
    for (size_t i = 0; i < len; i++)
    {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

static int secret_ok(const unsigned char *secret, size_t secret_len)
{
    return secret != NULL && secret_len >= TOKEN_SECRET_MIN_LEN &&
           secret_len <= (size_t)INT_MAX;
}

/* ====================================================================== *
 * Minting                                                                *
   ====================================================================== */

static int write_claims(char *buf, size_t cap, const token_claims_t *claims)
{
    if (memchr(claims->name, '\0', sizeof claims->name) == NULL)
        return -1;
    if (!is_name_ok(claims->name, strlen(claims->name)))
        return -1;

    int n = snprintf(buf, cap, "%lld\n%s\n%lld\n%lld\n", claims->sub,
                     claims->name, claims->iat, claims->exp);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static int write_token(char *tok, size_t cap, const token_claims_t *claims,
                       const unsigned char *secret, size_t secret_len)
{
    int k = write_claims(tok, cap, claims);
    if (k < 0)
        return -1;
    size_t n = (size_t)k;

    if (n + TOKEN_SIG_HEX_LEN + 1 > cap)
        return -1;

    unsigned char mac[TOKEN_SIG_LEN];
    if (hmac_sha256(mac, secret, secret_len, tok, n) != 0)
        return -1;

    hex_encode(mac, sizeof mac, tok + n); /* writes the NUL too */
    return (int)(n + TOKEN_SIG_HEX_LEN);
}

int token_mint(char *out, size_t out_len, const token_claims_t *claims,
               const unsigned char *secret, size_t secret_len)
{
    if (out == NULL || out_len <= 0 || claims == NULL ||
        !secret_ok(secret, secret_len))
    {
        (void)log_send(LOG_INFO,
                       "operation=token_mint phase=complete status=-1");
        return -1;
    }
    log_send(LOG_DEBUG, "minting authentication token subject=%lld user=%s",
             claims->sub, claims->name);
    out[0] = '\0';

    char tok[TOKEN_MAX_LEN];
    int tok_len = write_token(tok, sizeof tok, claims, secret, secret_len);
    if (tok_len < 0 || (size_t)tok_len + 1 > out_len)
    {
        (void)log_send(LOG_INFO,
                       "operation=token_mint phase=complete status=-1");
        return -1;
    }

    memcpy(out, tok, (size_t)tok_len + 1);
    (void)log_send(LOG_INFO, "operation=token_mint phase=complete status=0");
    return 0;
}

/* ====================================================================== *
 * Verifying                                                              *
   ====================================================================== */

/* A slice of the token. Pointer and length into the CALLER'S buffer; nothing
 * here is ever copied. */
typedef struct
{
    const char *p;
    size_t len;
} seg_t;

typedef struct
{
    seg_t sub, name, iat, exp, sig;
    seg_t signing; /* the claims and their final separator: what the MAC covers
                    */
} token_parts_t;

/* Exactly four separators, five fields, and a MAC of exactly 32 char. */
static token_result_t split_token(const char *token, size_t len,
                                  token_parts_t *out)
{
    seg_t field[TOKEN_FIELD_COUNT];
    const char *p = token;
    const char *end = token + len;
    size_t n = 0;

    for (;;)
    {
        const char *sep = memchr(p, TOKEN_FIELD_SEP, (size_t)(end - p));
        if (sep == NULL)
            break;
        if (n == TOKEN_FIELD_COUNT - 1)
            return TOKEN_E_MALFORMED; /* a separator inside the MAC field */
        field[n].p = p;
        field[n].len = (size_t)(sep - p);
        n++;
        p = sep + 1;
    }
    if (n != TOKEN_FIELD_COUNT - 1)
        return TOKEN_E_MALFORMED;

    field[n].p = p;
    field[n].len = (size_t)(end - p);
    if (field[n].len != TOKEN_SIG_HEX_LEN)
        return TOKEN_E_MALFORMED;

    out->sub = field[0];
    out->name = field[1];
    out->iat = field[2];
    out->exp = field[3];
    out->sig = field[4];

    /* The received bytes, verbatim, through the separator that ends the claims.
     * This is the only place a signing input is ever constructed. */
    out->signing.p = token;
    out->signing.len = (size_t)(field[4].p - token);
    return TOKEN_OK;
}

static token_result_t check_signature(seg_t signing, seg_t sig,
                                      const unsigned char *secret,
                                      size_t secret_len)
{
    unsigned char mac[TOKEN_SIG_LEN];
    char want[TOKEN_SIG_HEX_LEN + 1];

    if (hmac_sha256(mac, secret, secret_len, signing.p, signing.len) != 0)
        return TOKEN_E_SIGNATURE;
    hex_encode(mac, sizeof mac, want);
    return CRYPTO_memcmp(want, sig.p, TOKEN_SIG_HEX_LEN) == 0
               ? TOKEN_OK
               : TOKEN_E_SIGNATURE;
}

static int parse_pos_ll(const char *s, size_t len, long long *out)
{
    size_t i = 0;
    unsigned long long acc = 0;

    if (len == 0)
        return -1;
    if (s[i] == '0' && len - i > 1)
        return -1;

    for (; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        unsigned d = (unsigned)(s[i] - '0');
        if (acc > ((unsigned long long)LLONG_MAX - d) / 10)
            return -1;
        acc = acc * 10 + d;
    }
    *out = (long long)acc;
    return 0;
}

static token_result_t read_claims(const token_parts_t *parts,
                                  token_claims_t *out)
{
    if (parse_pos_ll(parts->sub.p, parts->sub.len, &out->sub) != 0)
        return TOKEN_E_CLAIMS;
    if (parse_pos_ll(parts->iat.p, parts->iat.len, &out->iat) != 0)
        return TOKEN_E_CLAIMS;
    if (parse_pos_ll(parts->exp.p, parts->exp.len, &out->exp) != 0)
        return TOKEN_E_CLAIMS;
    if (!is_name_ok(parts->name.p, parts->name.len))
        return TOKEN_E_CLAIMS;

    memcpy(out->name, parts->name.p, parts->name.len);
    out->name[parts->name.len] = '\0';
    return TOKEN_OK;
}

token_result_t token_verify(const char *token, const unsigned char *secret,
                            size_t secret_len, time_t now, token_claims_t *out)
{
    if (token == NULL || out == NULL || !secret_ok(secret, secret_len))
        return TOKEN_E_MALFORMED;
    size_t len = strnlen(token, TOKEN_MAX_LEN);
    if (len == TOKEN_MAX_LEN)
        return TOKEN_E_MALFORMED;

    memset(out, 0, sizeof *out);

    token_parts_t parts;
    token_result_t rc = split_token(token, len, &parts);
    if (rc != TOKEN_OK)
        return rc;

    rc = check_signature(parts.signing, parts.sig, secret, secret_len);
    if (rc != TOKEN_OK)
        return rc;

    rc = read_claims(&parts, out);
    if (rc != TOKEN_OK)
    {
        memset(out, 0, sizeof *out);
        return rc;
    }

    if ((long long)now >= out->exp)
    {
        memset(out, 0, sizeof *out);
        return TOKEN_E_EXPIRED;
    }
    return TOKEN_OK;
}
