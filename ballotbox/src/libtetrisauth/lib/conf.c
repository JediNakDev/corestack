#include "libtetrisdb/socket/conf.h"
#include "auth.h"
#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#define AUTH_DEFAULT_MAX_ATTEMPTS 5      /**< auth_max_attempts, 1..100. */
#define AUTH_DEFAULT_TOKEN_TTL 604800    /**< auth_token_ttl, in seconds. */
#define AUTH_DEFAULT_PBKDF2_ITERS 600000 /**< auth_pbkdf2_iters. */

static auth_conf_t glb_conf = {
    .max_attempts = AUTH_DEFAULT_MAX_ATTEMPTS,
    .token_ttl = AUTH_DEFAULT_TOKEN_TTL,
    .pbkdf2_iters = AUTH_DEFAULT_PBKDF2_ITERS,
    .db_timeout_ms = DB_DEFAULT_TIMEOUT_MS,
    .db_sock = DB_DEFAULT_IPC,
};

static int glb_frozen;

const auth_conf_t *auth_conf(void)
{
    return &glb_conf;
}

int auth_conf_load(void)
{
    if (glb_frozen)
        return 0;

    auth_conf_t scratch = glb_conf;

    rc_get_int("auth_max_attempts", AUTH_DEFAULT_MAX_ATTEMPTS, 1, 100,
               &scratch.max_attempts);
    rc_get_int("auth_token_ttl", AUTH_DEFAULT_TOKEN_TTL, 60, 31536000,
               &scratch.token_ttl);
    rc_get_int("auth_pbkdf2_iters", AUTH_DEFAULT_PBKDF2_ITERS, 1, 10000000,
               &scratch.pbkdf2_iters);
    rc_get_int("db_timeout", DB_DEFAULT_TIMEOUT_MS, DB_TIMEOUT_MIN_MS,
               DB_TIMEOUT_MAX_MS, &scratch.db_timeout_ms);
    rc_get("db_ipc", DB_DEFAULT_IPC, scratch.db_sock, sizeof scratch.db_sock);

    glb_conf = scratch;
    glb_frozen = 1;

    log_send(LOG_INFO,
             "auth: %s: max_attempts=%d token_ttl=%d "
             "pbkdf2_iters=%d db_ipc=%s db_timeout=%d",
             RC_PATH, glb_conf.max_attempts, glb_conf.token_ttl,
             glb_conf.pbkdf2_iters, glb_conf.db_sock, glb_conf.db_timeout_ms);
    return 0;
}
