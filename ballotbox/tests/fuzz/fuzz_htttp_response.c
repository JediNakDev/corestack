/*
 * fuzz_htttp_response.c - htttp_parse_response() on arbitrary bytes.
 *
 * The mirror of fuzz_htttp_request, and not redundant with it: requests flow
 * BOTH directions in this protocol (htttp.h's header comment - the server
 * pushes UPD_GAME/UPD_SESSION as requests), so both parsers run on both ends,
 * and the response parser's status-line half is code the request target never
 * reaches. Line 1 is the only difference between the two parsers, so this
 * target exists almost entirely to fuzz that one line.
 *
 * Round-trip is asymmetric here on purpose: htttp_serialize_response refuses
 * any status htttp_reason() has no phrase for, so a parsed status of 999
 * re-serializes as an error rather than bytes. That refusal is correct, so
 * only a SUCCESSFUL serialize is held to the parse-back property.
 */

#include "libhtttp/htttp.h"
#include "fuzz_support.h"

#include <stdlib.h>
#include <string.h>

static void check_response_invariants(const htttp_response_t *res,
                                      const uint8_t *buf, size_t len) {
  FUZZ_CHECK(res->n_headers <= HTTTP_MAX_HEADERS);
  for (size_t i = 0; i < res->n_headers; i++) {
    FUZZ_CHECK(fuzz_is_cstr(res->headers[i].key, HTTTP_MAX_KEY));
    FUZZ_CHECK(fuzz_is_cstr(res->headers[i].value, HTTTP_MAX_VALUE));
    FUZZ_CHECK(res->headers[i].key[0] != '\0');
  }
  FUZZ_CHECK(fuzz_slice_inside(res->body, res->body_len, buf, len));

  /* htttp.h documents the status as an HTTP status code. A negative one
   * would sail through every `status >= 400` check the apps write. */
  FUZZ_CHECK(res->status >= 100 && res->status <= 599);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > HTTTP_MAX_FRAME) return 0;

  uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
  if (!buf) return 0;
  memcpy(buf, data, size);

  htttp_response_t res;
  memset(&res, 0, sizeof res);
  if (htttp_parse_response(buf, (uint32_t)size, &res) != HTTTP_OK) {
    free(buf);
    return 0;
  }
  check_response_invariants(&res, buf, size);

  uint8_t out[HTTTP_MAX_FRAME];
  uint32_t out_len = sizeof out;
  if (htttp_serialize_response(&res, out, &out_len) == HTTTP_OK) {
    htttp_response_t again;
    memset(&again, 0, sizeof again);
    FUZZ_CHECK(htttp_parse_response(out, out_len, &again) == HTTTP_OK);
    check_response_invariants(&again, out, out_len);
    FUZZ_CHECK(res.status == again.status);
    FUZZ_CHECK(res.body_len == again.body_len);
    FUZZ_CHECK(res.body_len == 0 ||
               memcmp(res.body, again.body, res.body_len) == 0);
  }

  free(buf);
  return 0;
}
