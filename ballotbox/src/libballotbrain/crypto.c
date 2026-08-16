#include "libballotbrain/crypto.h"
#include "libballotbrain/ballotbrain.h"

#include <string.h>

/*
 * Crypto seam - stub implementation.
 *
 * Deterministic, well-formed hex placeholders so the logic layer can run. NOT
 * cryptographically meaningful. Replaced by OpenSSL RSA-OAEP / signed
 * commitments when keys arrive with the transport layer.
 *
 * Placeholder wire convention (must match libballotclient's encrypt stub):
 * payload[0] carries the chosen option index. This lets the cast/update logic
 * round-trip an option end to end without real encryption.
 */

static void hash_from_seed(char *out, unsigned int seed) {
  static const char hex[] = "0123456789abcdef";
  unsigned int state = seed * 2654435761u + 1u;
  for (int i = 0; i < BB_HASH_LEN - 1; i++) {
    state = state * 1103515245u + 12345u;
    out[i] = hex[(state >> 16) & 0xF];
  }
  out[BB_HASH_LEN - 1] = '\0';
}

static unsigned int fold(const char *s) {
  unsigned int h = 5381u;
  for (; *s; s++) {
    h = h * 33u + (unsigned char)*s;
  }
  return h;
}

bb_cert_status_t bb_verify_cert(bb_ctx *ctx, const char *cert_name) {
  /* Placeholder: real X.509 verification (chain, validity window, revocation)
   * arrives with the PKI/transport layer. For now any non-empty name verifies;
   * the eligible-list check in bb_join is the meaningful gate and is pure. */
  if (cert_name == NULL || cert_name[0] == '\0') {
    return BB_CERT_INVALID;
  }
  bb_log(ctx, "[cert] verify '%s' (placeholder -> VALID)", cert_name);
  return BB_CERT_VALID;
}

bb_result_t bb_decrypt_ballot(bb_ctx *ctx, const bb_ballot_t *ballot, int *option_index) {
  if (ballot == NULL || option_index == NULL) {
    return BB_ERR_DECRYPT;
  }
  bb_log(ctx, "[crypto] decrypt ballot (placeholder, nonce=%s)", ballot->nonce);
  /* Placeholder: option index is carried in payload[0]. Real OAEP decryption
   * and BB_ERR_DECRYPT detection land with keys. */
  *option_index = ballot->payload_len > 0 ? (int)ballot->payload[0] : 0;
  return BB_OK;
}

bb_result_t bb_issue_receipt(bb_ctx *ctx, const bb_ballot_t *ballot, int version,
                             bb_receipt_t *out) {
  if (ballot == NULL || out == NULL) {
    return BB_ERR_DB;
  }
  unsigned int seed = fold(ballot->nonce) ^ (unsigned int)(version * 2246822519u);
  hash_from_seed(out->hash, seed);
  /* Timestamp is deferred (needs a real clock source in the daemon). */
  strncpy(out->issued_at, "1970-01-01T00:00:00Z", BB_TIME_LEN - 1);
  out->issued_at[BB_TIME_LEN - 1] = '\0';
  bb_log(ctx, "[crypto] issue receipt v%d -> %s (placeholder)", version, out->hash);
  return BB_OK;
}
