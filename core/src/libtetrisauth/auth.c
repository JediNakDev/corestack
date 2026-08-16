#include "libtetrisauth/auth.h"
#include "lib/auth.h"
#include "lib/secret.h"
#include "libtetrisutil/limits.h"
#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/name.h"
#include <openssl/crypto.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SECRET_MAX 64
#define REPLY_MAX 1024

static session_t *glb_sh;
static char glb_name[MAX_USER_NAME];

static void reply(int status, const char *body)
{
    htttp_response_t res;
    uint8_t out[REPLY_MAX];
    uint32_t out_len = sizeof out;

    memset(&res, 0, sizeof res);
    res.status = status;
    if (body != NULL && body[0] != '\0')
    {
        res.body = (const uint8_t *)body;
        res.body_len = (uint32_t)strlen(body);
    }

    if (htttp_serialize_response(&res, out, &out_len) == HTTTP_OK)
        session_send(glb_sh, out, out_len);
    else
        log_send(LOG_ERROR, "auth: could not serialize a %d response", status);

    OPENSSL_cleanse(out, sizeof out);
}

static void accept_guest(void)
{
    glb_name[0] = '\0';
    log_send(LOG_INFO, "operation=accept_guest phase=complete status=200");
}

typedef enum
{
    AUTH_NONE = 0,
    AUTH_GUEST,
    AUTH_LOGIN,
    AUTH_REGISTER
} auth_method_t;

static auth_method_t auth_method_of(const htttp_request_t *req)
{
    if (strcmp(req->method, "GUEST") == 0)
        return AUTH_GUEST;
    if (strcmp(req->method, "LOGIN") == 0)
        return AUTH_LOGIN;
    if (strcmp(req->method, "REGISTER") == 0)
        return AUTH_REGISTER;
    return AUTH_NONE;
}

int req_body_split(const uint8_t *body, uint32_t body_len, acc_t *out)
{
    if (body == NULL || body_len == 0)
        return -1;

    /* The FIRST \n */
    const uint8_t *lf = memchr(body, '\n', body_len);
    if (lf == NULL)
        return -1;

    out->user = (const char *)body;
    out->user_len = (size_t)(lf - body);
    out->pass = (const char *)(lf + 1);
    out->pass_len = body_len - out->user_len - 1;

    if (out->user_len == 0 || out->pass_len == 0)
        return -1;
    return 0;
}

static int handle_guest_and_invalid_method(int method)
{
    switch (method)
    {
    case AUTH_GUEST:
        accept_guest();
        return 200;
    case AUTH_NONE:
        (void)log_send(LOG_WARN, "authentication failed status=401");
        return 401;
    default:
        return 0;
    }
}

static int auth_session_parser(session_t *sh, uint8_t *buf, uint32_t cap,
                               htttp_request_t *req, int *method, acc_t *c)
{

    int rc = session_recv(sh, buf, &cap);
    if (rc == SESSION_ERR_IO || rc == SESSION_ERR_TOOBIG)
    {
        return -1;
    }
    if (rc != SESSION_OK || htttp_parse_request(buf, cap, req) != 0)
    {
        return 400;
    }

    *method = auth_method_of(req);
    int status = handle_guest_and_invalid_method(*method);
    if (status != 0)
        return status;

    if (auth_conf_load() != 0)
    {
        return 500;
    }
    if (req_body_split(req->body, req->body_len, c) != 0)
    {
        return 400;
    }
    return 0;
}

static int token_handler(char *token, size_t token_cap, char *name,
                         long long id)
{
    unsigned char secret[SECRET_MAX];
    int secret_len;
    token_claims_t claims;

    memset(secret, 0, sizeof secret);
    memset(&claims, 0, sizeof claims);
    secret_len = auth_secret_load(".", secret, sizeof secret);
    if (secret_len < 0)
    {
        OPENSSL_cleanse(secret, sizeof secret);
        return 500;
    }
    claims.sub = id;
    snprintf(claims.name, sizeof claims.name, "%s", name);
    claims.iat = (long long)time(NULL);
    claims.exp = claims.iat + auth_conf()->token_ttl;

    if (token_mint(token, token_cap, &claims, secret, (size_t)secret_len) != 0)
    {
        log_send(LOG_DEBUG, "auth: could not mint a token for '%s'", name);
        OPENSSL_cleanse(secret, sizeof secret);
        return 500;
    }

    snprintf(glb_name, sizeof glb_name, "%s", name);
    OPENSSL_cleanse(secret, sizeof secret);
    return 200;
}

static int auth_handler(session_t *sh)
{
    log_send(LOG_DEBUG, "authentication gate opened fd=%d", sh->fd);
    glb_sh = sh;
    uint8_t buf[SESSION_MAX_FRAME];
    htttp_request_t req;

    char token[TOKEN_MAX_LEN];
    char name[MAX_USER_NAME];
    acc_t c;
    long long id = 0;
    int method;
    int status;

    memset(&req, 0, sizeof req);
    memset(token, 0, sizeof token);

    status = auth_session_parser(sh, buf, sizeof buf, &req, &method, &c);
    if (status != 0)
        goto done;
    if (user_name_fold(name, sizeof name, c.user, c.user_len) != 0)
    {
        status = 400;
        goto done;
    }

    switch (method)
    {
    case AUTH_LOGIN:
        status = account_login(name, &c, &id);
        break;
    case AUTH_REGISTER:
        status = account_register(name, &c, &id);
        break;
    default:
        status = 500;
        goto done;
    }

    if (status != 200)
        goto done;

    status = token_handler(token, sizeof token, name, id);

done:
    (void)log_send(status >= 500 || status < 0 ? LOG_ERROR
                   : status >= 400             ? LOG_WARN
                                               : LOG_INFO,
                   "operation=auth_handler phase=complete method=%s "
                   "status=%d",
                   req.method, status);
    if (status > 0)
        reply(status, status == 200 ? token : NULL);
    OPENSSL_cleanse(buf, sizeof buf);
    OPENSSL_cleanse(token, sizeof token);
    return status;
}

int auth_retry_handler(session_t *sh)
{
    for (int i = 0; i < auth_conf()->max_attempts;)
    {
        int status = auth_handler(sh);
        if (status == 200)
            return AUTH_OK;
        if (status < 0)
            return AUTH_DROP; /* dead socket, stop spinning */
        /* A 400 never reached account_login()/account_register(): nothing
         * was checked, so nothing was attempted. Every other refusal - a
         * real wrong-password 401, an unknown-user 404, a 500 - reflects an
         * actual check and spends one. */
        if (status != 400)
            i++;
    }
    return AUTH_DROP;
}

bool auth_offer(const htttp_request_t *req, const SessionState *st)
{
    (void)st;

    if (auth_method_of(req) == AUTH_NONE)
        return false;

    if (glb_sh == NULL)
        return false;

    reply(409, NULL);
    (void)log_send(LOG_WARN,
                   "operation=auth_offer phase=complete method=%s status=409",
                   req->method);

    if (req->body != NULL && req->body_len > 0)
        OPENSSL_cleanse((void *)(uintptr_t)req->body, req->body_len);

    return true;
}

void auth_get_name(char *dst, size_t cap)
{
    if (dst == NULL || cap == 0)
        return;

    snprintf(dst, cap, "%s", glb_name);
}
