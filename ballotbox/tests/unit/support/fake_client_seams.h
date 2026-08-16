#ifndef FAKE_CLIENT_SEAMS_H
#define FAKE_CLIENT_SEAMS_H

/*
 * Substitute seams for libballotclient unit tests.
 *
 * The client's outside dependencies are the transport (bcl_send) and the ballot
 * crypto (bu_encrypt_ballot, bu_derive_receipt). Both are replaced here, so the
 * session flows can be tested against any daemon answer - admitted, refused,
 * timed out - with nothing on the wire and no keys.
 *
 * Same link mechanics as fake_brain_seams.h: defining these symbols keeps the
 * real transport.o / voter_crypto.o members out of the binary. Include in
 * exactly one translation unit per test binary.
 */

#include "libballotclient/voter.h"

#include <string.h>

#define FAKE_MAX_SENDS 32

typedef struct {
  /* ---- the reply the daemon gives ----------------------------------- */
  bb_result_t send_result;    /* bcl_send's own return (transport level) */
  bcl_response_t response;    /* what it writes into `resp` */
  bb_result_t encrypt_result; /* bu_encrypt_ballot */

  /* ---- what the unit under test sent -------------------------------- */
  bcl_request_t sent[FAKE_MAX_SENDS];
  int send_count;
  int encrypt_calls;
  int last_encrypt_option;
} fake_client_t;

fake_client_t fake_client;

/* Reset to "the daemon accepts everything". */
static inline void fake_client_reset(void) {
  memset(&fake_client, 0, sizeof(fake_client));
  fake_client.send_result = BB_OK;
  fake_client.response.status = BB_OK;
  fake_client.encrypt_result = BB_OK;
  fake_client.last_encrypt_option = -1;
}

/* The last request that went to the transport, or NULL if none did. */
static inline const bcl_request_t *fake_client_last_send(void) {
  return fake_client.send_count > 0 ? &fake_client.sent[fake_client.send_count - 1] : NULL;
}

/* ---- the seams --------------------------------------------------------- */

bb_result_t bcl_send(bcl_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp) {
  (void)ctx;
  if (req == NULL) {
    return BB_ERR_DB;
  }
  if (fake_client.send_count < FAKE_MAX_SENDS) {
    fake_client.sent[fake_client.send_count++] = *req;
  }
  if (resp != NULL) {
    *resp = fake_client.response;
  }
  return fake_client.send_result;
}

bb_result_t bu_encrypt_ballot(bcl_ctx *ctx, int option_index, const char *nonce,
                              bb_ballot_t *out) {
  (void)ctx;
  fake_client.encrypt_calls++;
  fake_client.last_encrypt_option = option_index;
  if (fake_client.encrypt_result != BB_OK) {
    return fake_client.encrypt_result;
  }
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
    out->payload[0] = (uint8_t)option_index;
    out->payload_len = 1;
    snprintf(out->nonce, BB_NONCE_LEN, "%s", nonce != NULL ? nonce : "");
  }
  return BB_OK;
}

bb_result_t bu_derive_receipt(bcl_ctx *ctx, const char *secret_key, char out[BB_HASH_LEN]) {
  (void)ctx;
  if (out == NULL) {
    return BB_ERR_DB;
  }
  snprintf(out, BB_HASH_LEN, "hash-of-%s", secret_key != NULL ? secret_key : "");
  return BB_OK;
}

#endif /* FAKE_CLIENT_SEAMS_H */
