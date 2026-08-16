/*
 * fuzz_codec_response.c - the response path a CLIENT runs on daemon bytes:
 *
 *     wire bytes -> htttp_parse_response -> bcl_decode_response
 *
 * Worth a target of its own because the trust direction is reversed and the
 * parsing is looser. bcl_decode_response takes no op (codec.h: "every op's
 * response fields use distinct key names"), so it populates whatever keys
 * appear - including combinations no real daemon would ever send together,
 * which is precisely what a hostile or wedged daemon WOULD send. The counted
 * arrays are the risk: tally_count, hash_count and option_count each index a
 * fixed array in bcl_response_t, and each arrives as text from the wire.
 */

#include "libballotclient/codec.h"
#include "libhtttp/htttp.h"
#include "fuzz_support.h"

#include <stdlib.h>
#include <string.h>

static void check_response_fields(const bcl_response_t *resp) {
  /* The counts are the whole game: every one of these indexes an array. */
  FUZZ_CHECK(resp->option_count >= 0 && resp->option_count <= BB_MAX_OPTIONS);
  FUZZ_CHECK(resp->hash_count >= 0 && resp->hash_count <= BB_MAX_VOTERS);

  for (int i = 0; i < resp->option_count; i++)
    FUZZ_CHECK(fuzz_is_cstr(resp->options[i], BB_OPTION_LEN));
  for (int i = 0; i < resp->hash_count; i++)
    FUZZ_CHECK(fuzz_is_cstr(resp->hashes[i].hash, BB_HASH_LEN));

  FUZZ_CHECK(fuzz_is_cstr(resp->election.id, BB_ID_LEN));
  FUZZ_CHECK(fuzz_is_cstr(resp->election.title, BB_TITLE_LEN));
  FUZZ_CHECK(resp->election.option_count >= 0 &&
             resp->election.option_count <= BB_MAX_OPTIONS);
  FUZZ_CHECK(fuzz_is_cstr(resp->receipt.hash, BB_HASH_LEN));
  FUZZ_CHECK(fuzz_is_cstr(resp->found_option_name, BB_OPTION_LEN));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > HTTTP_MAX_FRAME) return 0;

  /* First byte picks the op to re-encode under: bcl_encode_response needs one
   * (the struct is shared by every op and only the op says which fields are
   * meaningful), and the decoder does not recover it. Taking it from the
   * input lets the fuzzer explore every op's encoder rather than pinning one.
   * The rest of the input is the frame. */
  if (size < 1) return 0;
  bcl_op_t op = (bcl_op_t)(data[0] % 12); /* BCL_JOIN .. BCL_ADMIN_NEXT_ID */
  const uint8_t *frame = data + 1;
  size_t frame_len = size - 1;

  uint8_t *buf = (uint8_t *)malloc(frame_len ? frame_len : 1);
  if (!buf) return 0;
  memcpy(buf, frame, frame_len);

  htttp_response_t http;
  memset(&http, 0, sizeof http);
  if (htttp_parse_response(buf, (uint32_t)frame_len, &http) != HTTTP_OK) {
    free(buf);
    return 0;
  }

  bcl_response_t resp;
  memset(&resp, 0, sizeof resp);
  if (bcl_decode_response(&http, &resp) != 0) {
    free(buf);
    return 0;
  }
  check_response_fields(&resp);

  uint8_t out[HTTTP_MAX_FRAME];
  uint32_t out_len = sizeof out;
  if (bcl_encode_response(op, &resp, out, &out_len) != 0) {
    free(buf);
    return 0;
  }

  htttp_response_t http2;
  bcl_response_t resp2;
  memset(&http2, 0, sizeof http2);
  memset(&resp2, 0, sizeof resp2);
  FUZZ_CHECK(htttp_parse_response(out, out_len, &http2) == HTTTP_OK);
  FUZZ_CHECK(bcl_decode_response(&http2, &resp2) == 0);
  check_response_fields(&resp2);

  /* status is the one field every op carries, and the one the clients branch
   * on. codec.h: the body's status= line is authoritative, so it must be the
   * value that survives a round trip - not the coarse HTTP status. */
  FUZZ_CHECK(resp.status == resp2.status);

  free(buf);
  return 0;
}
