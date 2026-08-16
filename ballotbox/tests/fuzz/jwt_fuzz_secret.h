#ifndef BALLOTBOX_FUZZ_JWT_SECRET_H
#define BALLOTBOX_FUZZ_JWT_SECRET_H

/*
 * The signing key and clock the JWT fuzz target verifies against, shared with
 * the seed generator so a seeded token is a token that actually verifies.
 *
 * It lives in a header rather than in both files because the target's whole
 * oracle is "no fuzzer-invented token may verify". If the generator minted
 * under a different key, every seed would be a forgery the target correctly
 * rejects, the corpus would carry no valid structure to mutate from, and the
 * strongest check in the directory would be exercising the reject path only.
 *
 * NOT A CREDENTIAL. It signs nothing outside bin/fuzz_jwt_verify.
 */

/* JWT_SECRET_MIN_LEN is 32 (RFC 7518 3.2: at least the hash output size). */
static const unsigned char JWT_FUZZ_SECRET[32] = {
    'f', 'u', 'z', 'z', '-', 'o', 'n', 'l', 'y', '-', 's', 'e', 'c', 'r', 'e',
    't', '-', 'n', 'o', 't', '-', 'a', '-', 'r', 'e', 'a', 'l', '-', 'k', 'e',
    'y', '.'};

/* A fixed "now", so a corpus entry's verdict never changes with the wall
 * clock. A time-dependent oracle turns every seeded valid token into a crash
 * on the day it expires, which reads as a fuzzer finding and is not one. */
#define JWT_FUZZ_NOW 1700000000LL

/* Comfortably inside JWT_FUZZ_NOW's future, so seeds stay verifiable. */
#define JWT_FUZZ_EXP (JWT_FUZZ_NOW + 3600)

#endif /* BALLOTBOX_FUZZ_JWT_SECRET_H */
