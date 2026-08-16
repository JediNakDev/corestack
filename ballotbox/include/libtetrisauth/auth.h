#ifndef LIBTETRISAUTH_H
#define LIBTETRISAUTH_H

#include <stdbool.h>
#include <stddef.h>

#include "libhtttp/htttp.h"
#include "libtetrissh/tetrissh.h"
#include "libtetrisutil/sessionstate.h"

#define TETRISAUTH_DB_TABLE "user"
#define TETRISAUTH_DB_SCHEMA                                                   \
    "id int, name string, salt string, digest string, iters int, created_at "  \
    "int"

enum
{
    AUTH_OK = 0,
    AUTH_DROP = 1
};

/* Runs the pre-auth exchange to completion. */
int auth_retry_handler(session_t *sh);

/* Creates auth/jwt_secret under root if nothing is there yet, then validates
 * it; bin/tetrisdb's only reason to reach into this library. Returns 0 once
 * a usable secret is on disk, -1 otherwise (see log). */
int auth_secret_provision(const char *root);

/* a LOGIN, REGISTER or GUEST arriving after auth_retry_handler(). */
bool auth_offer(const htttp_request_t *req, const SessionState *st);

/* This client's roster display name, or "" for a guest. */
void auth_get_name(char *dst, size_t cap);

#endif /* LIBTETRISAUTH_H */
