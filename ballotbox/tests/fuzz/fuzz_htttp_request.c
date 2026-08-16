/*
 * fuzz_htttp_request.c - htttp_parse_request() on arbitrary bytes.
 *
 * The highest-value target in the repo: every frame libtetrissh decrypts is
 * handed to this function before anything has decided the peer is honest, so
 * it is the first code in the system an attacker reaches. Its whole job is
 * writing wire bytes into fixed-size fields (HTTTP_MAX_METHOD, _PATH, _KEY,
 * _VALUE, _HEADERS), which is the shape overflows live in.
 *
 * Oracles, in order of what they catch:
 *   1. ASan/UBSan  - the parser read or wrote out of bounds.
 *   2. FUZZ_CHECK  - the parser returned HTTTP_OK having left the caller a
 *                    struct its header promises cannot exist (unterminated
 *                    field, header count past the array, body slice outside
 *                    the input buffer).
 *   3. Round-trip  - a parsed request re-serializes to something that parses
 *                    back identically. A failure here is request smuggling:
 *                    two different messages that this parser cannot tell
 *                    apart, or one message it reads differently the second
 *                    time.
 */

#include "libhtttp/htttp.h"
#include "fuzz_support.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp: header names are case-insensitive */

/* What htttp.h promises a caller holding an HTTTP_OK request. */
static void check_request_invariants(const htttp_request_t *req,
                                     const uint8_t *buf, size_t len) {
  FUZZ_CHECK(fuzz_is_cstr(req->method, HTTTP_MAX_METHOD));
  FUZZ_CHECK(fuzz_is_cstr(req->path, HTTTP_MAX_PATH));
  FUZZ_CHECK(req->n_headers <= HTTTP_MAX_HEADERS);

  for (size_t i = 0; i < req->n_headers; i++) {
    FUZZ_CHECK(fuzz_is_cstr(req->headers[i].key, HTTTP_MAX_KEY));
    FUZZ_CHECK(fuzz_is_cstr(req->headers[i].value, HTTTP_MAX_VALUE));
    /* An empty key would make htttp_header_get("") match it, and every
     * lookup for an absent header is a lookup that must not match. */
    FUZZ_CHECK(req->headers[i].key[0] != '\0');
  }

  /* Zero-copy: body points into buf, so body + body_len must too. A body
   * slice running past the input is the bug class that turns "attacker sends
   * a short frame with a large Content-Length" into an over-read in whichever
   * caller reads the body - which is every caller. */
  FUZZ_CHECK(fuzz_slice_inside(req->body, req->body_len, buf, len));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > HTTTP_MAX_FRAME) return 0;

  /* Heap, not a stack array: ASan puts redzones around a heap buffer, so an
   * off-by-one read past the frame is a report rather than a silent read of
   * whatever the next stack slot held. */
  uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
  if (!buf) return 0;
  memcpy(buf, data, size);

  htttp_request_t req;
  memset(&req, 0, sizeof req);
  if (htttp_parse_request(buf, (uint32_t)size, &req) != HTTTP_OK) {
    free(buf);
    return 0;
  }
  check_request_invariants(&req, buf, size);

  /* Round-trip. Serialization can legitimately fail (NOSPACE on a request
   * whose headers fit the parse bounds but not one frame once Content-Length
   * is added), and that is not a finding - only a SUCCESSFUL re-serialize
   * that fails to parse back, or parses back as a different request, is. */
  uint8_t out[HTTTP_MAX_FRAME];
  uint32_t out_len = sizeof out;
  if (htttp_serialize_request(&req, out, &out_len) == HTTTP_OK) {
    htttp_request_t again;
    memset(&again, 0, sizeof again);
    FUZZ_CHECK(htttp_parse_request(out, out_len, &again) == HTTTP_OK);
    check_request_invariants(&again, out, out_len);

    FUZZ_CHECK(strcmp(req.method, again.method) == 0);
    FUZZ_CHECK(strcmp(req.path, again.path) == 0);
    FUZZ_CHECK(req.body_len == again.body_len);
    FUZZ_CHECK(req.body_len == 0 ||
               memcmp(req.body, again.body, req.body_len) == 0);

    /* Every header of the original must still be findable with the same
     * value - the property callers actually rely on.
     *
     * Content-Length is excluded, and that exclusion is a finding from the
     * seed corpus rather than a guess: "Content-Length: 0" with no body
     * parses fine, but the serializer COMPUTES this header from body_len and
     * emits it only when there is a body, so the round trip legitimately
     * drops it. It is a derived header, not a carried one, and holding a
     * derived header to a survival property would have made every future run
     * of this target fail on a message that is entirely correct. */
    for (size_t i = 0; i < req.n_headers; i++) {
      if (strcasecmp(req.headers[i].key, "Content-Length") == 0) continue;

      const char *v = htttp_header_get(again.headers, again.n_headers,
                                       req.headers[i].key);
      FUZZ_CHECK(v != NULL);
      /* A duplicate key in the input makes "first match" the only defined
       * answer, so only compare when the key appears once.
       *
       * strcasecmp, not strcmp, and that distinction was itself a finding:
       * htttp_header_get matches case-insensitively, so "B: o" and "b: " are
       * ONE key as far as the lookup is concerned. Counting them with strcmp
       * saw two unique keys, compared the value of each against the lookup's
       * (which returns the first match for both), and reported a parser bug
       * on a message htttp handled exactly as documented. */
      size_t seen = 0;
      for (size_t j = 0; j < req.n_headers; j++)
        if (strcasecmp(req.headers[j].key, req.headers[i].key) == 0) seen++;
      if (seen == 1) FUZZ_CHECK(strcmp(v, req.headers[i].value) == 0);
    }
  }

  free(buf);
  return 0;
}
