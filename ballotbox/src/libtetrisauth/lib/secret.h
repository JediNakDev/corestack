#ifndef LIBTETRISAUTH_PROVISION_H
#define LIBTETRISAUTH_PROVISION_H

#include <stddef.h>

int auth_secret_load(const char *root, unsigned char *out, size_t outlen);

/* Creates auth/jwt_secret under root if nothing is there, then validates it.
 * Returns 0 once a usable secret is on disk, -1 otherwise (see log). */
int auth_secret_provision(const char *root);

#endif /* LIBTETRISAUTH_PROVISION_H */
