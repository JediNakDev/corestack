/*
 * fuzz_codec_request.c - the real request path, end to end:
 *
 *     wire bytes -> htttp_parse_request -> bcl_decode_request -> bcl_request_t
 *
 * This is exactly what ballotd's session does with a frame from a voter
 * (src/ballotd/session.c) before any eligibility or lifecycle check runs, so
 * a bug found here is reachable by anyone who can open a socket. Fuzzing the
 * two layers as a pipeline rather than separately is the point: htttp's
 * output is codec's input, and the interesting bugs are the ones where htttp
 * hands over something codec did not expect it could (an empty path, a
 * Cert-Name of exactly HTTTP_MAX_VALUE-1 bytes going into a BB_CERT_LEN
 * field, a body with no '=' anywhere in it).
 *
 * Oracles:
 *   1. ASan/UBSan.
 *   2. Field bounds: every fixed-size field of bcl_request_t terminated, and
 *      the counts inside their arrays. option_count > BB_MAX_OPTIONS would be
 *      an out-of-bounds loop in every consumer of the struct.
 *   3. Round-trip: decode -> encode -> decode is a fixed point. The first
 *      decode does all the truncating, so anything that changes on the SECOND
 *      pass means encode and decode disagree about the wire format - two
 *      peers reading one message differently, which for a ballot means the
 *      voter and the daemon disagreeing about the vote.
 */

#include "libballotclient/codec.h"
#include "libhtttp/htttp.h"
#include "fuzz_support.h"

#include <stdlib.h>
#include <string.h>

static void check_request_fields(const bcl_request_t *req) {
  FUZZ_CHECK(fuzz_is_cstr(req->cert_name, BB_CERT_LEN));
  FUZZ_CHECK(fuzz_is_cstr(req->election_id, BB_ID_LEN));
  FUZZ_CHECK(fuzz_is_cstr(req->hash, BB_HASH_LEN));
  FUZZ_CHECK(fuzz_is_cstr(req->config.title, BB_TITLE_LEN));
  FUZZ_CHECK(fuzz_is_cstr(req->config.open_time, BB_TIME_LEN));
  FUZZ_CHECK(fuzz_is_cstr(req->config.close_time, BB_TIME_LEN));

  FUZZ_CHECK(req->config.option_count >= 0 &&
             req->config.option_count <= BB_MAX_OPTIONS);
  FUZZ_CHECK(req->config.eligible_count >= 0 &&
             req->config.eligible_count <= BB_MAX_VOTERS);
  for (int i = 0; i < req->config.option_count; i++)
    FUZZ_CHECK(fuzz_is_cstr(req->config.options[i], BB_OPTION_LEN));
  for (int i = 0; i < req->config.eligible_count; i++)
    FUZZ_CHECK(fuzz_is_cstr(req->config.eligible[i], BB_CERT_LEN));

  /* The ciphertext length is what every crypto call downstream trusts. */
  FUZZ_CHECK(req->ballot.payload_len <= BB_CIPHERTEXT_MAX);
  FUZZ_CHECK(fuzz_is_cstr(req->ballot.nonce, BB_NONCE_LEN));
  FUZZ_CHECK(fuzz_is_cstr(req->ballot.cert_name, BB_CERT_LEN));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > HTTTP_MAX_FRAME) return 0;

  uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
  if (!buf) return 0;
  memcpy(buf, data, size);

  htttp_request_t http;
  memset(&http, 0, sizeof http);
  if (htttp_parse_request(buf, (uint32_t)size, &http) != HTTTP_OK) {
    free(buf);
    return 0;
  }

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  if (bcl_decode_request(&http, &req) != 0) {
    free(buf);
    return 0;
  }
  check_request_fields(&req);

  /* Re-encode. A decoded request that will not re-encode is worth knowing
   * about but is not automatically a bug (a CREATE body can decode to
   * strings whose total length exceeds the encoder's 8 KiB body budget), so
   * it ends the case rather than tripping a check. */
  uint8_t out[HTTTP_MAX_FRAME];
  uint32_t out_len = sizeof out;
  if (bcl_encode_request(&req, out, &out_len) != 0) {
    free(buf);
    return 0;
  }

  htttp_request_t http2;
  bcl_request_t req2;
  memset(&http2, 0, sizeof http2);
  memset(&req2, 0, sizeof req2);

  /* Bytes this codec produced must parse. Anything else means the encoder
   * emits frames its own peer will answer with a 400. */
  FUZZ_CHECK(htttp_parse_request(out, out_len, &http2) == HTTTP_OK);
  FUZZ_CHECK(bcl_decode_request(&http2, &req2) == 0);
  check_request_fields(&req2);

  FUZZ_CHECK(req.op == req2.op);
  FUZZ_CHECK(strcmp(req.cert_name, req2.cert_name) == 0);
  FUZZ_CHECK(strcmp(req.election_id, req2.election_id) == 0);
  FUZZ_CHECK(strcmp(req.hash, req2.hash) == 0);
  FUZZ_CHECK(req.ballot.payload_len == req2.ballot.payload_len);
  FUZZ_CHECK(memcmp(req.ballot.payload, req2.ballot.payload,
                    req.ballot.payload_len) == 0);
  FUZZ_CHECK(strcmp(req.ballot.nonce, req2.ballot.nonce) == 0);
  FUZZ_CHECK(req.config.option_count == req2.config.option_count);
  FUZZ_CHECK(req.config.eligible_count == req2.config.eligible_count);
  FUZZ_CHECK(strcmp(req.config.title, req2.config.title) == 0);

  free(buf);
  return 0;
}
