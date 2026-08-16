/*
 * Unit tests for the shared HTTTP codec (libballotclient/codec.c).
 *
 * Round-trips every op through encode -> htttp_serialize -> htttp_parse ->
 * decode and checks the struct survives, plus malformed-input handling
 * (unknown method, missing required field, bad hex, unrecognised status).
 * No seams needed - this layer has no collaborators, just bytes in and out.
 */

#include "libballotclient/codec.h"
#include "unity.h"

#include <string.h>

#define WIRE_MAX 65536

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ------------------------------------------------------------- */

static void roundtrip_request(const bcl_request_t *in, bcl_request_t *out) {
  uint8_t wire[WIRE_MAX];
  uint32_t wlen = sizeof wire;
  TEST_ASSERT_EQUAL_INT(0, bcl_encode_request(in, wire, &wlen));

  htttp_request_t http;
  TEST_ASSERT_EQUAL_INT(HTTTP_OK, htttp_parse_request(wire, wlen, &http));
  TEST_ASSERT_EQUAL_INT(0, bcl_decode_request(&http, out));
}

static void roundtrip_response(bcl_op_t op, const bcl_response_t *in, bcl_response_t *out) {
  uint8_t wire[WIRE_MAX];
  uint32_t wlen = sizeof wire;
  TEST_ASSERT_EQUAL_INT(0, bcl_encode_response(op, in, wire, &wlen));

  htttp_response_t http;
  TEST_ASSERT_EQUAL_INT(HTTTP_OK, htttp_parse_response(wire, wlen, &http));
  TEST_ASSERT_EQUAL_INT(0, bcl_decode_response(&http, out));
}

/* ---- request round trips --------------------------------------------------- */

void test_create_request_roundtrip(void) {
  bcl_request_t in;
  memset(&in, 0, sizeof in);
  in.op = BCL_CREATE;
  snprintf(in.cert_name, BB_CERT_LEN, "admin");
  snprintf(in.config.title, BB_TITLE_LEN, "Officers 2026");
  snprintf(in.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(in.config.close_time, BB_TIME_LEN, "2026-01-01T01:00:00Z");
  snprintf(in.config.options[0], BB_OPTION_LEN, "Alice");
  snprintf(in.config.options[1], BB_OPTION_LEN, "Bob");
  in.config.option_count = 2;
  snprintf(in.config.eligible[0], BB_CERT_LEN, "alice");
  snprintf(in.config.eligible[1], BB_CERT_LEN, "bob");
  in.config.eligible_count = 2;

  bcl_request_t out;
  roundtrip_request(&in, &out);

  TEST_ASSERT_EQUAL_INT(BCL_CREATE, out.op);
  TEST_ASSERT_EQUAL_STRING("admin", out.cert_name);
  TEST_ASSERT_EQUAL_STRING("Officers 2026", out.config.title);
  TEST_ASSERT_EQUAL_STRING("2026-01-01T00:00:00Z", out.config.open_time);
  TEST_ASSERT_EQUAL_STRING("2026-01-01T01:00:00Z", out.config.close_time);
  TEST_ASSERT_EQUAL_INT(2, out.config.option_count);
  TEST_ASSERT_EQUAL_STRING("Alice", out.config.options[0]);
  TEST_ASSERT_EQUAL_STRING("Bob", out.config.options[1]);
  TEST_ASSERT_EQUAL_INT(2, out.config.eligible_count);
  TEST_ASSERT_EQUAL_STRING("alice", out.config.eligible[0]);
  TEST_ASSERT_EQUAL_STRING("bob", out.config.eligible[1]);
}

void test_lifecycle_requests_roundtrip(void) {
  const bcl_op_t ops[] = {BCL_OPEN, BCL_CLOSE, BCL_PUBLISH, BCL_JOIN, BCL_RESULTS};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    bcl_request_t in;
    memset(&in, 0, sizeof in);
    in.op = ops[i];
    snprintf(in.cert_name, BB_CERT_LEN, "admin");
    snprintf(in.election_id, BB_ID_LEN, "E-100");

    bcl_request_t out;
    roundtrip_request(&in, &out);

    TEST_ASSERT_EQUAL_INT(ops[i], out.op);
    TEST_ASSERT_EQUAL_STRING("E-100", out.election_id);
    TEST_ASSERT_EQUAL_STRING("admin", out.cert_name);
  }
}

void test_cast_request_roundtrip(void) {
  bcl_request_t in;
  memset(&in, 0, sizeof in);
  in.op = BCL_CAST;
  snprintf(in.cert_name, BB_CERT_LEN, "alice");
  snprintf(in.election_id, BB_ID_LEN, "E-100");
  snprintf(in.ballot.nonce, BB_NONCE_LEN, "nonce-123");
  uint8_t payload[] = {0x00, 0xFF, 0x10, 0xAB};
  memcpy(in.ballot.payload, payload, sizeof payload);
  in.ballot.payload_len = sizeof payload;

  bcl_request_t out;
  roundtrip_request(&in, &out);

  TEST_ASSERT_EQUAL_INT(BCL_CAST, out.op);
  TEST_ASSERT_EQUAL_STRING("E-100", out.election_id);
  TEST_ASSERT_EQUAL_STRING("nonce-123", out.ballot.nonce);
  TEST_ASSERT_EQUAL_STRING("alice", out.ballot.cert_name);
  TEST_ASSERT_EQUAL_UINT(sizeof payload, out.ballot.payload_len);
  TEST_ASSERT_EQUAL_MEMORY(payload, out.ballot.payload, sizeof payload);
}

void test_check_request_roundtrip(void) {
  bcl_request_t in;
  memset(&in, 0, sizeof in);
  in.op = BCL_CHECK;
  snprintf(in.election_id, BB_ID_LEN, "E-042");
  snprintf(in.hash, BB_HASH_LEN, "fa15b8bb00000000000000000000000000000000000000000000000028a63299");

  bcl_request_t out;
  roundtrip_request(&in, &out);

  TEST_ASSERT_EQUAL_INT(BCL_CHECK, out.op);
  TEST_ASSERT_EQUAL_STRING("E-042", out.election_id);
  TEST_ASSERT_EQUAL_STRING(in.hash, out.hash);
}

/* ---- response round trips --------------------------------------------------- */

void test_create_response_roundtrip_ok(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_OK;
  snprintf(in.election.id, BB_ID_LEN, "E-101");

  bcl_response_t out;
  roundtrip_response(BCL_CREATE, &in, &out);

  TEST_ASSERT_EQUAL_INT(BB_OK, out.status);
  TEST_ASSERT_EQUAL_STRING("E-101", out.election.id);
}

void test_create_response_roundtrip_error_has_no_election(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_ERR_CONFIG_TITLE;

  bcl_response_t out;
  roundtrip_response(BCL_CREATE, &in, &out);

  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TITLE, out.status);
  TEST_ASSERT_EQUAL_STRING("", out.election.id);
}

void test_join_response_roundtrip(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_OK;
  snprintf(in.election.id, BB_ID_LEN, "E-100");
  snprintf(in.election.title, BB_TITLE_LEN, "Board Motion 2026");
  in.election.state = BB_STATE_OPEN;
  snprintf(in.election.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(in.election.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");
  snprintf(in.election.options[0], BB_OPTION_LEN, "Approve");
  snprintf(in.election.options[1], BB_OPTION_LEN, "Reject");
  snprintf(in.election.options[2], BB_OPTION_LEN, "Abstain");
  in.election.option_count = 3;
  /* Eligible list must never cross the wire, even though it's set here. */
  snprintf(in.election.eligible[0], BB_CERT_LEN, "alice");
  in.election.eligible_count = 1;

  bcl_response_t out;
  roundtrip_response(BCL_JOIN, &in, &out);

  TEST_ASSERT_EQUAL_INT(BB_OK, out.status);
  TEST_ASSERT_EQUAL_STRING("E-100", out.election.id);
  TEST_ASSERT_EQUAL_STRING("Board Motion 2026", out.election.title);
  TEST_ASSERT_EQUAL_INT(BB_STATE_OPEN, out.election.state);
  TEST_ASSERT_EQUAL_INT(3, out.election.option_count);
  TEST_ASSERT_EQUAL_STRING("Approve", out.election.options[0]);
  TEST_ASSERT_EQUAL_STRING("Abstain", out.election.options[2]);
  TEST_ASSERT_EQUAL_INT(0, out.election.eligible_count);
}

void test_cast_response_roundtrip(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_OK;
  snprintf(in.receipt.hash, BB_HASH_LEN, "abc123");
  snprintf(in.receipt.issued_at, BB_TIME_LEN, "2026-01-01T00:00:01Z");

  bcl_response_t out;
  roundtrip_response(BCL_CAST, &in, &out);

  TEST_ASSERT_EQUAL_INT(BB_OK, out.status);
  TEST_ASSERT_EQUAL_STRING("abc123", out.receipt.hash);
  TEST_ASSERT_EQUAL_STRING("2026-01-01T00:00:01Z", out.receipt.issued_at);
}

void test_results_response_roundtrip(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_OK;
  snprintf(in.election.id, BB_ID_LEN, "E-100");
  snprintf(in.election.title, BB_TITLE_LEN, "Favourite colour");
  in.option_count = 2;
  in.tally[0] = 14;
  in.tally[1] = 6;
  snprintf(in.options[0], BB_OPTION_LEN, "Yes");
  snprintf(in.options[1], BB_OPTION_LEN, "No");
  in.hash_count = 2;
  snprintf(in.hashes[0].hash, BB_HASH_LEN, "hash-one");
  in.hashes[0].option_index = 0;
  in.hashes[0].version = 1;
  in.hashes[0].superseded = 0;
  snprintf(in.hashes[1].hash, BB_HASH_LEN, "hash-two");
  in.hashes[1].option_index = 1;
  in.hashes[1].version = 2;
  in.hashes[1].superseded = 1;

  bcl_response_t out;
  roundtrip_response(BCL_RESULTS, &in, &out);

  TEST_ASSERT_EQUAL_INT(BB_OK, out.status);
  TEST_ASSERT_EQUAL_STRING("E-100", out.election.id);
  TEST_ASSERT_EQUAL_STRING("Favourite colour", out.election.title);
  TEST_ASSERT_EQUAL_INT(2, out.option_count);
  TEST_ASSERT_EQUAL_INT(14, out.tally[0]);
  TEST_ASSERT_EQUAL_INT(6, out.tally[1]);
  TEST_ASSERT_EQUAL_STRING("Yes", out.options[0]);
  TEST_ASSERT_EQUAL_STRING("No", out.options[1]);
  TEST_ASSERT_EQUAL_INT(2, out.hash_count);
  TEST_ASSERT_EQUAL_STRING("hash-one", out.hashes[0].hash);
  TEST_ASSERT_EQUAL_INT(0, out.hashes[0].option_index);
  TEST_ASSERT_EQUAL_INT(1, out.hashes[0].version);
  TEST_ASSERT_EQUAL_INT(0, out.hashes[0].superseded);
  TEST_ASSERT_EQUAL_STRING("hash-two", out.hashes[1].hash);
  TEST_ASSERT_EQUAL_INT(1, out.hashes[1].superseded);
  /* "result_option" must never land in election.options - that's JOIN's
   * "option" key, a different field entirely. */
  TEST_ASSERT_EQUAL_INT(0, out.election.option_count);
}

void test_check_response_roundtrip_found(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_OK;
  in.found = 1;
  in.found_option = 1;

  bcl_response_t out;
  roundtrip_response(BCL_CHECK, &in, &out);

  TEST_ASSERT_EQUAL_INT(1, out.found);
  TEST_ASSERT_EQUAL_INT(1, out.found_option);
}

void test_check_response_roundtrip_not_found(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_ERR_NOT_FOUND;
  in.found = 0;

  bcl_response_t out;
  roundtrip_response(BCL_CHECK, &in, &out);

  TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, out.status);
  TEST_ASSERT_EQUAL_INT(0, out.found);
}

/* CHECK's found_option and JOIN's option list must not collide in the
 * shared decoder now that they are distinct keys (found_option vs option). */
void test_check_found_option_does_not_leak_into_election_options(void) {
  bcl_response_t in;
  memset(&in, 0, sizeof in);
  in.status = BB_OK;
  in.found = 1;
  in.found_option = 5;

  bcl_response_t out;
  roundtrip_response(BCL_CHECK, &in, &out);

  TEST_ASSERT_EQUAL_INT(0, out.election.option_count);
}

/* ---- malformed input -------------------------------------------------------- */

void test_decode_request_rejects_unknown_method(void) {
  htttp_request_t http;
  memset(&http, 0, sizeof http);
  snprintf(http.method, sizeof http.method, "DELETE");
  snprintf(http.path, sizeof http.path, "/election/E-100");

  bcl_request_t out;
  TEST_ASSERT_EQUAL_INT(-1, bcl_decode_request(&http, &out));
}

void test_decode_request_rejects_missing_election_id(void) {
  htttp_request_t http;
  memset(&http, 0, sizeof http);
  snprintf(http.method, sizeof http.method, "JOIN");
  snprintf(http.path, sizeof http.path, "/election/");

  bcl_request_t out;
  TEST_ASSERT_EQUAL_INT(-1, bcl_decode_request(&http, &out));
}

void test_decode_request_rejects_cast_without_payload(void) {
  htttp_request_t http;
  memset(&http, 0, sizeof http);
  snprintf(http.method, sizeof http.method, "CAST");
  snprintf(http.path, sizeof http.path, "/election/E-100/ballot");
  const char *body = "nonce=abc\n";
  http.body = (const uint8_t *)body;
  http.body_len = (uint32_t)strlen(body);

  bcl_request_t out;
  TEST_ASSERT_EQUAL_INT(-1, bcl_decode_request(&http, &out));
}

void test_decode_request_rejects_bad_hex_payload(void) {
  htttp_request_t http;
  memset(&http, 0, sizeof http);
  snprintf(http.method, sizeof http.method, "CAST");
  snprintf(http.path, sizeof http.path, "/election/E-100/ballot");
  const char *body = "nonce=abc\npayload=zzzz\n";
  http.body = (const uint8_t *)body;
  http.body_len = (uint32_t)strlen(body);

  bcl_request_t out;
  TEST_ASSERT_EQUAL_INT(-1, bcl_decode_request(&http, &out));
}

void test_decode_response_rejects_missing_status(void) {
  htttp_response_t http;
  memset(&http, 0, sizeof http);
  http.status = 200;
  const char *body = "election_id=E-100\n";
  http.body = (const uint8_t *)body;
  http.body_len = (uint32_t)strlen(body);

  bcl_response_t out;
  TEST_ASSERT_EQUAL_INT(-1, bcl_decode_response(&http, &out));
}

void test_decode_response_rejects_unrecognised_status_name(void) {
  htttp_response_t http;
  memset(&http, 0, sizeof http);
  http.status = 200;
  const char *body = "status=BB_ERR_MADE_UP\n";
  http.body = (const uint8_t *)body;
  http.body_len = (uint32_t)strlen(body);

  bcl_response_t out;
  TEST_ASSERT_EQUAL_INT(-1, bcl_decode_response(&http, &out));
}

/* ---- HTTP status bucket ------------------------------------------------------ */

void test_http_status_bucket_is_always_a_known_reason(void) {
  const bb_result_t codes[] = {
      BB_OK, BB_ERR_CONFIG_TITLE, BB_ERR_CONFIG_OPTIONS, BB_ERR_CONFIG_TIME,
      BB_ERR_ILLEGAL_TRANSITION, BB_ERR_NOT_OPEN, BB_ERR_CLOSED, BB_ERR_NOT_PUBLISHED,
      BB_ERR_NOT_ELIGIBLE, BB_ERR_CERT_INVALID, BB_ERR_CERT_EXPIRED, BB_ERR_BAD_OPTION,
      BB_ERR_REPLAY, BB_ERR_DECRYPT, BB_ERR_NOT_FOUND, BB_ERR_NOT_JOINED,
      BB_ERR_DB, BB_ERR_NOT_IMPLEMENTED,
  };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
    int status = bcl_http_status(codes[i]);
    TEST_ASSERT_NOT_NULL(htttp_reason(status));
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_create_request_roundtrip);
  RUN_TEST(test_lifecycle_requests_roundtrip);
  RUN_TEST(test_cast_request_roundtrip);
  RUN_TEST(test_check_request_roundtrip);
  RUN_TEST(test_create_response_roundtrip_ok);
  RUN_TEST(test_create_response_roundtrip_error_has_no_election);
  RUN_TEST(test_join_response_roundtrip);
  RUN_TEST(test_cast_response_roundtrip);
  RUN_TEST(test_results_response_roundtrip);
  RUN_TEST(test_check_response_roundtrip_found);
  RUN_TEST(test_check_response_roundtrip_not_found);
  RUN_TEST(test_check_found_option_does_not_leak_into_election_options);
  RUN_TEST(test_decode_request_rejects_unknown_method);
  RUN_TEST(test_decode_request_rejects_missing_election_id);
  RUN_TEST(test_decode_request_rejects_cast_without_payload);
  RUN_TEST(test_decode_request_rejects_bad_hex_payload);
  RUN_TEST(test_decode_response_rejects_missing_status);
  RUN_TEST(test_decode_response_rejects_unrecognised_status_name);
  RUN_TEST(test_http_status_bucket_is_always_a_known_reason);
  return UNITY_END();
}
