/* Unit tests for libhtttp: parse, serialize, round-trip, and the rejection
 * paths. No sockets and no OpenSSL — every case is a byte buffer in memory,
 * so this runs anywhere: make test
 *
 * Wire vectors are built from the CRLF macro rather than typed as "\r\n"
 * (decision D1: bare LF is malformed, so a typo'd terminator would look like
 * a parser bug instead of a test bug). */
#include <stdio.h>
#include "test_output.h"
#include <string.h>
#include "libhtttp/htttp.h"

static int tests_run = 0, tests_failed = 0;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

#define CRLF "\r\n"

/* Length from sizeof, not strlen: a body may contain NULs. Literals only. */
#define WIRE(s) (const uint8_t *)(s), (uint32_t)(sizeof(s) - 1)

/* Occurrences of `needle` in a buffer that is not NUL-terminated. Used to
 * prove generated headers appear exactly once. */
static int count_sub(const uint8_t *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    int n = 0;
    if (nlen == 0 || len < nlen)
        return 0;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0)
            n++;
    return n;
}

/* --- Parsing -------------------------------------------------------------- */

static int test_parse_request(void)
{
    htttp_request_t req;
    CHECK(htttp_parse_request(WIRE("MOVE /room/3/player/2 HTTTP/1.0" CRLF
                                   "Player-Id: 7" CRLF
                                   "Content-Length: 1" CRLF CRLF "1"),
                              &req) == HTTTP_OK,
          "parse");
    CHECK(strcmp(req.method, "MOVE") == 0, "method");
    CHECK(strcmp(req.path, "/room/3/player/2") == 0, "path");
    CHECK(req.n_headers == 2, "header count");
    const char *pid = htttp_header_get(req.headers, req.n_headers, "Player-Id");
    CHECK(pid && strcmp(pid, "7") == 0, "Player-Id value");
    /* Lookup is case-insensitive; the stored key keeps the sender's case. */
    CHECK(htttp_header_get(req.headers, req.n_headers, "player-id") != NULL,
          "case-insensitive lookup");
    CHECK(req.body_len == 1 && req.body && req.body[0] == '1', "body");
    return 0;
}

static int test_parse_response(void)
{
    htttp_response_t res;
    CHECK(htttp_parse_response(WIRE("HTTTP/1.0 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Content-Length: 2" CRLF CRLF "OK"),
                               &res) == HTTTP_OK,
          "parse");
    CHECK(res.status == 200, "status");
    CHECK(res.body_len == 2 && memcmp(res.body, "OK", 2) == 0, "body");
    /* A multi-word reason must not be tokenized. */
    CHECK(htttp_parse_response(WIRE("HTTTP/1.0 404 Not Found" CRLF CRLF),
                               &res) == HTTTP_OK,
          "multi-word reason");
    CHECK(res.status == 404 && res.body_len == 0, "404 no body");
    return 0;
}

static int test_parse_empty_value(void)
{
    /* "Key: " with nothing after the space is a legal empty value. */
    htttp_request_t req;
    CHECK(htttp_parse_request(
              WIRE("LEAVE /room HTTTP/1.0" CRLF "Player-Id: " CRLF CRLF),
              &req) == HTTTP_OK,
          "parse");
    const char *v = htttp_header_get(req.headers, req.n_headers, "Player-Id");
    CHECK(v && v[0] == '\0', "empty value");
    return 0;
}

/* A colon inside a header VALUE is data, not a separator: the split is at the
 * FIRST colon, so everything after ": " survives verbatim. Named Q&A case, and
 * the one that a naive strchr-and-split-every-colon parser gets wrong. */
static int test_colon_in_header_value(void)
{
    htttp_request_t req;
    CHECK(htttp_parse_request(WIRE("JOIN /room/0 HTTTP/1.0" CRLF
                                   "Host: tetrish.local:5555" CRLF
                                   "Player-Id: a:b:c" CRLF CRLF),
                              &req) == HTTTP_OK,
          "parse");
    const char *host = htttp_header_get(req.headers, req.n_headers, "Host");
    CHECK(host && strcmp(host, "tetrish.local:5555") == 0, "host value intact");
    const char *v = htttp_header_get(req.headers, req.n_headers, "Player-Id");
    CHECK(v && strcmp(v, "a:b:c") == 0, "every later colon kept");
    CHECK(req.n_headers == 2, "two headers, not four");

    /* And it survives a round trip, so serialize does not re-split it. */
    uint8_t out[512];
    uint32_t n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_OK, "serialize");
    htttp_request_t back;
    CHECK(htttp_parse_request(out, n, &back) == HTTTP_OK, "reparse");
    const char *v2 = htttp_header_get(back.headers, back.n_headers, "Player-Id");
    CHECK(v2 && strcmp(v2, "a:b:c") == 0, "value survives round trip");
    return 0;
}

/* --- Round-trip ----------------------------------------------------------- */

/* The method/path table from htttp.h. Serialize then parse each one back. */
static int test_roundtrip_methods(void)
{
    static const struct
    {
        const char *method, *path;
    } table[] = {
        {"JOIN", "/room/0"},
        {"LEAVE", "/room/3"},
        {"START", "/room/3"},
        {"MOVE", "/room/3/player/2"},
        {"ROTATE", "/room/3/player/2"},
        {"DROP", "/room/3/player/2"},
        {"HOLD", "/room/3/player/2"},
        {"STATE", "/room/3"},
        {"UPD_SESSION", "/session/state"},
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
    {
        htttp_request_t req;
        memset(&req, 0, sizeof req);
        snprintf(req.method, sizeof req.method, "%s", table[i].method);
        snprintf(req.path, sizeof req.path, "%s", table[i].path);
        CHECK(htttp_header_set(req.headers, &req.n_headers, "Player-Id", "7") ==
                  HTTTP_OK,
              "header_set");
        req.body = (const uint8_t *)"1";
        req.body_len = 1;

        uint8_t out[512];
        uint32_t n = sizeof out;
        CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_OK, "serialize");

        htttp_request_t back;
        CHECK(htttp_parse_request(out, n, &back) == HTTTP_OK, "reparse");
        CHECK(strcmp(back.method, table[i].method) == 0, "method survives");
        CHECK(strcmp(back.path, table[i].path) == 0, "path survives");
        CHECK(back.body_len == 1 && back.body[0] == '1', "body survives");
        const char *pid =
            htttp_header_get(back.headers, back.n_headers, "Player-Id");
        CHECK(pid && strcmp(pid, "7") == 0, "header survives");
        /* Generated exactly once, from the body actually sent. */
        CHECK(count_sub(out, n, "Content-Length: 1" CRLF) == 1,
              "one generated Content-Length");
    }
    return 0;
}

static int test_roundtrip_response(void)
{
    htttp_response_t res;
    memset(&res, 0, sizeof res);
    res.status = 200;
    CHECK(htttp_header_set(res.headers, &res.n_headers, "Room-Id", "42") ==
              HTTTP_OK,
          "header_set");
    res.body = (const uint8_t *)"{\"score\":100}";
    res.body_len = 13;

    uint8_t out[512];
    uint32_t n = sizeof out;
    CHECK(htttp_serialize_response(&res, out, &n) == HTTTP_OK, "serialize");
    CHECK(count_sub(out, n, "HTTTP/1.0 200 OK" CRLF) == 1, "status line");
    CHECK(count_sub(out, n, "Date: ") == 1, "generated Date");
    CHECK(count_sub(out, n, "Content-Length: 13" CRLF) == 1,
          "generated Content-Length");

    htttp_response_t back;
    CHECK(htttp_parse_response(out, n, &back) == HTTTP_OK, "reparse");
    CHECK(back.status == 200, "status survives");
    CHECK(back.body_len == 13 && memcmp(back.body, res.body, 13) == 0,
          "body survives");
    CHECK(htttp_header_get(back.headers, back.n_headers, "Date") != NULL,
          "Date parsed back");
    return 0;
}

/* D5: a parsed message keeps Content-Length (and Date) in its header list.
 * Re-serializing must regenerate them, not echo the stale values. */
static int test_generated_headers_replace_stale(void)
{
    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "JOIN");
    snprintf(req.path, sizeof req.path, "/room/0");
    CHECK(htttp_header_set(req.headers, &req.n_headers, "Player-Id", "7") ==
              HTTTP_OK,
          "set Player-Id");
    CHECK(htttp_header_set(req.headers, &req.n_headers, "Content-Length",
                           "999") == HTTTP_OK,
          "set stale Content-Length");
    req.body = (const uint8_t *)"0";
    req.body_len = 1;

    uint8_t out[512];
    uint32_t n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_OK, "serialize");
    CHECK(count_sub(out, n, "Content-Length: 999") == 0, "stale one dropped");
    CHECK(count_sub(out, n, "Content-Length: 1" CRLF) == 1, "real one emitted");
    /* One Content-Length total, or the message reparses with a wrong length. */
    CHECK(count_sub(out, n, "Content-Length") == 1, "not duplicated");

    /* Same for Date on the response side. */
    htttp_response_t res;
    memset(&res, 0, sizeof res);
    res.status = 200;
    CHECK(htttp_header_set(res.headers, &res.n_headers, "Date",
                           "stale,-01-Jan-1970") == HTTTP_OK,
          "set stale Date");
    n = sizeof out;
    CHECK(htttp_serialize_response(&res, out, &n) == HTTTP_OK, "serialize res");
    CHECK(count_sub(out, n, "stale,-01-Jan-1970") == 0, "stale Date dropped");
    CHECK(count_sub(out, n, "Date: ") == 1, "one Date");
    return 0;
}

/* Duplicate Content-Length on the wire: D4 keeps both, lookup returns the
 * first. Re-serializing must collapse them to one generated field. */
static int test_duplicate_content_length(void)
{
    htttp_request_t req;
    CHECK(htttp_parse_request(WIRE("MOVE /room/3/player/2 HTTTP/1.0" CRLF
                                   "Content-Length: 1" CRLF
                                   "Content-Length: 1" CRLF CRLF "1"),
                              &req) == HTTTP_OK,
          "parse");
    CHECK(req.n_headers == 2, "both kept");
    uint8_t out[512];
    uint32_t n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_OK, "serialize");
    CHECK(count_sub(out, n, "Content-Length") == 1, "collapsed to one");
    return 0;
}

/* --- Bodies --------------------------------------------------------------- */

static int test_bodies(void)
{
    uint8_t out[512];
    uint32_t n;

    /* No body: no Content-Length at all, message ends at the blank line. */
    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "HOLD");
    snprintf(req.path, sizeof req.path, "/room/3/player/2");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_OK, "serialize");
    CHECK(count_sub(out, n, "Content-Length") == 0, "no Content-Length");
    CHECK(n == sizeof("HOLD /room/3/player/2 HTTTP/1.0" CRLF CRLF) - 1,
          "exact length");
    htttp_request_t back;
    CHECK(htttp_parse_request(out, n, &back) == HTTTP_OK, "reparse");
    CHECK(back.body == NULL && back.body_len == 0, "no body parsed");

    /* A length with no buffer behind it must be rejected, not read. */
    req.body = NULL;
    req.body_len = 5;
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "NULL body with length rejected");

    htttp_response_t res;
    memset(&res, 0, sizeof res);
    res.status = 200;
    res.body = NULL;
    res.body_len = 5;
    n = sizeof out;
    CHECK(htttp_serialize_response(&res, out, &n) == HTTTP_ERR_MALFORMED,
          "response NULL body with length rejected");

    /* A body containing NULs must survive: length is carried, never strlen'd.
     */
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "STATE");
    snprintf(req.path, sizeof req.path, "/room/3");
    req.body = (const uint8_t *)"a\0b\0c";
    req.body_len = 5;
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_OK, "serialize NULs");
    CHECK(count_sub(out, n, "Content-Length: 5" CRLF) == 1, "length is 5");
    CHECK(htttp_parse_request(out, n, &back) == HTTTP_OK, "reparse NULs");
    CHECK(back.body_len == 5 && memcmp(back.body, "a\0b\0c", 5) == 0,
          "NUL body survives");
    return 0;
}

/* --- Boundaries (tested in pairs) ----------------------------------------- */

/* A single-sided boundary test passes against both > and >=, which is exactly
 * the off-by-one it is meant to catch. Each pair checks MAX-1 and MAX. */
static int test_boundary_key_len(void)
{
    char key[HTTTP_MAX_KEY + 2];
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t n = 0;

    /* MAX_KEY - 1 bytes plus the terminator exactly fills the field. */
    memset(key, 'k', HTTTP_MAX_KEY - 1);
    key[HTTTP_MAX_KEY - 1] = '\0';
    CHECK(htttp_header_set(headers, &n, key, "v") == HTTTP_OK,
          "MAX_KEY-1 accepted");
    CHECK(n == 1, "count advanced");

    /* MAX_KEY bytes needs MAX_KEY+1 with the terminator: one too many. */
    memset(key, 'k', HTTTP_MAX_KEY);
    key[HTTTP_MAX_KEY] = '\0';
    CHECK(htttp_header_set(headers, &n, key, "v") == HTTTP_ERR_TOOLONG,
          "MAX_KEY rejected");
    CHECK(n == 1, "count not advanced on failure");
    return 0;
}

static int test_boundary_value_len(void)
{
    char value[HTTTP_MAX_VALUE + 2];
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t n = 0;

    memset(value, 'v', HTTTP_MAX_VALUE - 1);
    value[HTTTP_MAX_VALUE - 1] = '\0';
    CHECK(htttp_header_set(headers, &n, "K", value) == HTTTP_OK,
          "MAX_VALUE-1 accepted");

    memset(value, 'v', HTTTP_MAX_VALUE);
    value[HTTTP_MAX_VALUE] = '\0';
    CHECK(htttp_header_set(headers, &n, "K", value) == HTTTP_ERR_TOOLONG,
          "MAX_VALUE rejected");
    return 0;
}

static int test_boundary_header_count(void)
{
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t n = 0;
    char key[16];

    for (size_t i = 0; i < HTTTP_MAX_HEADERS; i++)
    {
        snprintf(key, sizeof key, "K%zu", i);
        CHECK(htttp_header_set(headers, &n, key, "v") == HTTTP_OK,
              "fill to MAX_HEADERS");
    }
    CHECK(n == HTTTP_MAX_HEADERS, "exactly MAX_HEADERS stored");
    CHECK(htttp_header_set(headers, &n, "OneMore", "v") == HTTTP_ERR_TOOLONG,
          "MAX_HEADERS+1 rejected");
    CHECK(n == HTTTP_MAX_HEADERS, "count unchanged on failure");
    return 0;
}

static int test_boundary_parse_header_count(void)
{
    /* The parser must cap at MAX_HEADERS too, not just the setter. */
    char wire[8192];
    size_t off = 0;
    off += (size_t)snprintf(wire + off, sizeof wire - off,
                            "HOLD /room/3/player/2 HTTTP/1.0" CRLF);
    for (size_t i = 0; i < HTTTP_MAX_HEADERS + 1; i++)
        off +=
            (size_t)snprintf(wire + off, sizeof wire - off, "K%zu: v" CRLF, i);
    off += (size_t)snprintf(wire + off, sizeof wire - off, CRLF);

    htttp_request_t req;
    CHECK(htttp_parse_request((const uint8_t *)wire, (uint32_t)off, &req) ==
              HTTTP_ERR_TOOLONG,
          "MAX_HEADERS+1 on the wire rejected");
    return 0;
}

/* Capacity is exact-fit and one-short. On NOSPACE, *out_len must be left
 * alone — the caller must not be able to read it back as a length. */
static int test_boundary_nospace(void)
{
    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "MOVE");
    snprintf(req.path, sizeof req.path, "/room/3/player/2");
    req.body = (const uint8_t *)"1";
    req.body_len = 1;

    uint8_t big[512];
    uint32_t need = sizeof big;
    CHECK(htttp_serialize_request(&req, big, &need) == HTTTP_OK, "measure");

    uint8_t out[512];
    uint32_t n = need;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_OK,
          "exact capacity fits");
    CHECK(n == need, "wrote exactly need bytes");

    n = need - 1;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_NOSPACE,
          "one byte short rejected");
    CHECK(n == need - 1, "*out_len untouched on NOSPACE");

    /* Zero capacity must fail on the very first append, not write anything. */
    n = 0;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_NOSPACE,
          "zero capacity rejected");

    htttp_response_t res;
    memset(&res, 0, sizeof res);
    res.status = 200;
    need = sizeof big;
    CHECK(htttp_serialize_response(&res, big, &need) == HTTTP_OK,
          "measure response");
    n = need;
    CHECK(htttp_serialize_response(&res, out, &n) == HTTTP_OK,
          "response exact capacity fits");
    n = need - 1;
    CHECK(htttp_serialize_response(&res, out, &n) == HTTTP_ERR_NOSPACE,
          "response one byte short rejected");
    return 0;
}

/* --- Malformed input ------------------------------------------------------ */

static int test_malformed_request(void)
{
    struct
    {
        const uint8_t *buf;
        uint32_t len;
        const char *what;
    } cases[] = {
        {WIRE("MOVE /room/3/player/2 HTTTP/1.0"), "no CRLF at all"},
        {WIRE("MOVE /room/3/player/2 HTTTP/1.0\n\n"), "bare LF, not CRLF"},
        {WIRE("MOVE /game/move" CRLF CRLF), "no version field"},
        {WIRE("MOVE /game/move HTTP/1.0" CRLF CRLF), "wrong version (HTTP)"},
        {WIRE("MOVE /game/move HTTTP/2.0" CRLF CRLF), "wrong version (2.0)"},
        {WIRE("MOVE" CRLF CRLF), "one field, no space"},
        {WIRE("MOVE /game/move X HTTTP/1.0" CRLF CRLF), "three spaces"},
        {WIRE(" /game/move HTTTP/1.0" CRLF CRLF), "empty method"},
        {WIRE("MOVE  HTTTP/1.0" CRLF CRLF), "empty path"},
        {WIRE("MOVE /x HTTTP/1.0" CRLF "PlayerId 7" CRLF CRLF), "no colon"},
        {WIRE("MOVE /x HTTTP/1.0" CRLF "Player-Id:7" CRLF CRLF),
         "colon with no space"},
        {WIRE("MOVE /x HTTTP/1.0" CRLF "Player-Id:" CRLF CRLF),
         "colon at end of line"},
        {WIRE("MOVE /x HTTTP/1.0" CRLF ": 7" CRLF CRLF), "empty key"},
        {WIRE("MOVE /x HTTTP/1.0" CRLF "Player-Id: 7" CRLF),
         "headers never terminated"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        htttp_request_t req;
        int rc = htttp_parse_request(cases[i].buf, cases[i].len, &req);
        if (rc == HTTTP_OK)
        {
            test_output_failure_detailf(__FILE__, __LINE__,
                                        "accepted malformed: %s",
                                        cases[i].what);
            return -1;
        }
    }
    return 0;
}

static int test_malformed_response(void)
{
    struct
    {
        const uint8_t *buf;
        uint32_t len;
        const char *what;
    } cases[] = {
        {WIRE("HTTTP/1.0 200 OK"), "no CRLF"},
        {WIRE("HTTTP/1.0 200 OK\n\n"), "bare LF"},
        {WIRE("HTTP/1.0 200 OK" CRLF CRLF), "wrong version"},
        {WIRE("HTTTP/1.0" CRLF CRLF), "version only"},
        {WIRE("HTTTP/1.0 200" CRLF CRLF), "no reason field"},
        {WIRE("HTTTP/1.0  OK" CRLF CRLF), "empty status"},
        {WIRE("HTTTP/1.0 2xx OK" CRLF CRLF), "non-numeric status"},
        {WIRE("HTTTP/1.0 99 Nope" CRLF CRLF), "status below 100"},
        {WIRE("HTTTP/1.0 600 Nope" CRLF CRLF), "status above 599"},
        {WIRE("HTTTP/1.0_200 OK" CRLF CRLF), "no space after version"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        htttp_response_t res;
        int rc = htttp_parse_response(cases[i].buf, cases[i].len, &res);
        if (rc == HTTTP_OK)
        {
            test_output_failure_detailf(__FILE__, __LINE__,
                                        "accepted malformed: %s",
                                        cases[i].what);
            return -1;
        }
    }
    return 0;
}

/* --- Content-Length handling (D3) ----------------------------------------- */

static int test_length(void)
{
    htttp_request_t req;

    /* Declared longer than what arrived: the over-read this check exists for.
     */
    CHECK(htttp_parse_request(
              WIRE("MOVE /x HTTTP/1.0" CRLF "Content-Length: 5" CRLF CRLF "ab"),
              &req) == HTTTP_ERR_LENGTH,
          "declared too long");

    /* Declared shorter: trailing bytes would be silently dropped. */
    CHECK(htttp_parse_request(WIRE("MOVE /x HTTTP/1.0" CRLF
                                   "Content-Length: 1" CRLF CRLF "abc"),
                              &req) == HTTTP_ERR_LENGTH,
          "declared too short");

    /* Absent Content-Length means no body, so trailing bytes are unaccounted.
     */
    CHECK(htttp_parse_request(WIRE("MOVE /x HTTTP/1.0" CRLF CRLF "extra"),
                              &req) == HTTTP_ERR_LENGTH,
          "trailing bytes with no Content-Length");

    CHECK(htttp_parse_request(
              WIRE("MOVE /x HTTTP/1.0" CRLF "Content-Length: abc" CRLF CRLF),
              &req) != HTTTP_OK,
          "non-numeric length");

    /* Empty value must not read as zero. */
    CHECK(htttp_parse_request(
              WIRE("MOVE /x HTTTP/1.0" CRLF "Content-Length: " CRLF CRLF),
              &req) != HTTTP_OK,
          "empty length");

    /* Overflows uint32_t. Rejected, never wrapped. */
    CHECK(htttp_parse_request(WIRE("MOVE /x HTTTP/1.0" CRLF
                                   "Content-Length: 99999999999999" CRLF CRLF),
                              &req) != HTTTP_OK,
          "overflowing length");

    /* No sign, no whitespace, no hex — parse_uint takes digits only. */
    CHECK(htttp_parse_request(
              WIRE("MOVE /x HTTTP/1.0" CRLF "Content-Length: -1" CRLF CRLF),
              &req) != HTTTP_OK,
          "signed length");
    CHECK(htttp_parse_request(
              WIRE("MOVE /x HTTTP/1.0" CRLF "Content-Length: 0x10" CRLF CRLF),
              &req) != HTTTP_OK,
          "hex length");

    /* Content-Length: 0 with no body is consistent and must be accepted. */
    CHECK(htttp_parse_request(
              WIRE("MOVE /x HTTTP/1.0" CRLF "Content-Length: 0" CRLF CRLF),
              &req) == HTTTP_OK,
          "zero length accepted");
    CHECK(req.body == NULL && req.body_len == 0, "zero length gives no body");
    return 0;
}

/* --- Degenerate input ----------------------------------------------------- */

static int test_degenerate(void)
{
    htttp_request_t req;
    htttp_response_t res;
    uint8_t out[64];
    uint32_t n;
    const uint8_t *buf = (const uint8_t *)"MOVE /x HTTTP/1.0" CRLF CRLF;

    CHECK(htttp_parse_request(buf, 0, &req) != HTTTP_OK, "len 0");
    CHECK(htttp_parse_request(buf, 1, &req) != HTTTP_OK, "len 1");
    CHECK(htttp_parse_request(buf, 2, &req) != HTTTP_OK, "len 2");
    CHECK(htttp_parse_request(NULL, 10, &req) == HTTTP_ERR_MALFORMED,
          "NULL buffer");
    CHECK(htttp_parse_request(buf, 10, NULL) == HTTTP_ERR_MALFORMED,
          "NULL out struct");
    CHECK(htttp_parse_response(NULL, 10, &res) == HTTTP_ERR_MALFORMED,
          "NULL buffer, response");
    CHECK(htttp_parse_response(buf, 10, NULL) == HTTTP_ERR_MALFORMED,
          "NULL out struct, response");

    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "HOLD");
    snprintf(req.path, sizeof req.path, "/room/3/player/2");
    n = sizeof out;
    CHECK(htttp_serialize_request(NULL, out, &n) == HTTTP_ERR_MALFORMED,
          "NULL req");
    CHECK(htttp_serialize_request(&req, NULL, &n) == HTTTP_ERR_MALFORMED,
          "NULL out");
    CHECK(htttp_serialize_request(&req, out, NULL) == HTTTP_ERR_MALFORMED,
          "NULL out_len");

    /* Empty method or path would emit two adjacent spaces. */
    memset(&req, 0, sizeof req);
    snprintf(req.path, sizeof req.path, "/room");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "empty method");
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "START");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "empty path");

    memset(&res, 0, sizeof res);
    res.status = 200;
    n = sizeof out;
    CHECK(htttp_serialize_response(NULL, out, &n) == HTTTP_ERR_MALFORMED,
          "NULL res");
    CHECK(htttp_serialize_response(&res, NULL, &n) == HTTTP_ERR_MALFORMED,
          "NULL out, response");
    CHECK(htttp_serialize_response(&res, out, NULL) == HTTTP_ERR_MALFORMED,
          "NULL out_len, response");
    return 0;
}

/* --- Injection ------------------------------------------------------------ */

/* A space or CRLF in a method, path, or key forges an extra request line or
 * header. header_set is the first gate; the serializer is the second, and it
 * must hold even for a struct built by hand around the setter. */
static int test_injection_header_set(void)
{
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t n = 0;

    CHECK(htttp_header_set(headers, &n, "Bad Key", "v") == HTTTP_ERR_MALFORMED,
          "space in key");
    CHECK(htttp_header_set(headers, &n, "Bad\r\nKey", "v") ==
              HTTTP_ERR_MALFORMED,
          "CRLF in key");
    CHECK(htttp_header_set(headers, &n, "X", "a\r\nInjected: 1") ==
              HTTTP_ERR_MALFORMED,
          "CRLF in value");
    CHECK(htttp_header_set(headers, &n, "", "v") == HTTTP_ERR_MALFORMED,
          "empty key");
    CHECK(htttp_header_set(headers, &n, "X\x7f", "v") == HTTTP_ERR_MALFORMED,
          "DEL in key");
    CHECK(htttp_header_set(headers, &n, "X\x80", "v") == HTTTP_ERR_MALFORMED,
          "high byte in key");
    CHECK(n == 0, "nothing stored");

    /* A space in a value is legal — only keys are space-free. */
    CHECK(htttp_header_set(headers, &n, "Reason-Note", "not found") == HTTTP_OK,
          "space in value allowed");
    return 0;
}

static int test_injection_serialize(void)
{
    uint8_t out[512];
    uint32_t n;
    htttp_request_t req;

    /* Fields written directly, bypassing header_set, to prove the serializer
     * is an independent gate rather than trusting its input. */
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "MO VE");
    snprintf(req.path, sizeof req.path, "/room/3/player/2");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "space in method");

    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "MOVE");
    snprintf(req.path, sizeof req.path, "/a b");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "space in path");

    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "MOVE");
    snprintf(req.path, sizeof req.path, "/a\r\nX: 1");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "CRLF in path");

    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "MOVE");
    snprintf(req.path, sizeof req.path, "/room/3/player/2");
    req.n_headers = 1;
    snprintf(req.headers[0].key, sizeof req.headers[0].key, "Bad Key");
    snprintf(req.headers[0].value, sizeof req.headers[0].value, "v");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "space in header key");

    snprintf(req.headers[0].key, sizeof req.headers[0].key, "X");
    snprintf(req.headers[0].value, sizeof req.headers[0].value,
             "a\r\nInjected: 1");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "CRLF in header value");

    /* Empty key would emit ": v\r\n" — malformed wire with no error. */
    req.headers[0].key[0] = '\0';
    snprintf(req.headers[0].value, sizeof req.headers[0].value, "v");
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_MALFORMED,
          "empty header key rejected at serialize");

    /* n_headers larger than the array behind it must not be walked. */
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "MOVE");
    snprintf(req.path, sizeof req.path, "/room/3/player/2");
    req.n_headers = HTTTP_MAX_HEADERS + 1;
    n = sizeof out;
    CHECK(htttp_serialize_request(&req, out, &n) == HTTTP_ERR_TOOLONG,
          "n_headers past array rejected");
    return 0;
}

/* --- Status codes --------------------------------------------------------- */

static int test_reason_phrases(void)
{
    static const int known[] = {200, 201, 400, 401, 403,
                                404, 409, 413, 429, 500};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        CHECK(htttp_reason(known[i]) != NULL, "known status has a phrase");
    /* NULL is load-bearing: serialize_response uses it as the guard, so a
     * placeholder string here would defeat that check. */
    CHECK(htttp_reason(418) == NULL, "unknown status has no phrase");
    CHECK(htttp_reason(0) == NULL, "zero has no phrase");
    CHECK(htttp_reason(-1) == NULL, "negative has no phrase");
    return 0;
}

/* Documents a deliberate asymmetry: the parser accepts any status in 100..599,
 * the serializer only emits the ones htttp_reason knows. A peer's 418 parses,
 * but this library will not re-emit it. */
static int test_status_asymmetry(void)
{
    htttp_response_t res;
    CHECK(htttp_parse_response(WIRE("HTTTP/1.0 418 Teapot" CRLF CRLF), &res) ==
              HTTTP_OK,
          "off-list status parses");
    CHECK(res.status == 418, "status stored");

    uint8_t out[256];
    uint32_t n = sizeof out;
    CHECK(htttp_serialize_response(&res, out, &n) == HTTTP_ERR_MALFORMED,
          "off-list status will not serialize");
    CHECK(n == sizeof out, "*out_len untouched on rejection");
    return 0;
}

/* --- Header lookup -------------------------------------------------------- */

static int test_header_get(void)
{
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t n = 0;
    CHECK(htttp_header_set(headers, &n, "Player-Id", "7") == HTTTP_OK, "set");
    CHECK(htttp_header_set(headers, &n, "Player-Id", "9") == HTTTP_OK,
          "set duplicate");

    /* D4: duplicates are kept, and lookup returns the first. */
    const char *v = htttp_header_get(headers, n, "Player-Id");
    CHECK(v && strcmp(v, "7") == 0, "first duplicate wins");
    CHECK(htttp_header_get(headers, n, "PLAYER-ID") != NULL,
          "case-insensitive");
    CHECK(htttp_header_get(headers, n, "Absent") == NULL, "absent key");
    /* Prefixes must not match either direction. */
    CHECK(htttp_header_get(headers, n, "Player") == NULL, "prefix no match");
    CHECK(htttp_header_get(headers, n, "Player-Id-Extra") == NULL,
          "longer no match");
    CHECK(htttp_header_get(NULL, n, "Player-Id") == NULL, "NULL headers");
    CHECK(htttp_header_get(headers, n, NULL) == NULL, "NULL key");
    CHECK(htttp_header_get(headers, 0, "Player-Id") == NULL, "zero count");
    return 0;
}

#define RUN(fn)                                                                \
    do                                                                         \
    {                                                                          \
        tests_run++;                                                           \
        if (fn() == 0)                                                         \
            test_output_pass(#fn);                                             \
        else                                                                   \
        {                                                                      \
            test_output_fail(#fn);                                             \
            tests_failed++;                                                    \
        }                                                                      \
    } while (0)

int main(void)
{
    test_output_begin("test_htttp");
    RUN(test_parse_request);
    RUN(test_parse_response);
    RUN(test_parse_empty_value);
    RUN(test_colon_in_header_value);
    RUN(test_roundtrip_methods);
    RUN(test_roundtrip_response);
    RUN(test_generated_headers_replace_stale);
    RUN(test_duplicate_content_length);
    RUN(test_bodies);
    RUN(test_boundary_key_len);
    RUN(test_boundary_value_len);
    RUN(test_boundary_header_count);
    RUN(test_boundary_parse_header_count);
    RUN(test_boundary_nospace);
    RUN(test_malformed_request);
    RUN(test_malformed_response);
    RUN(test_length);
    RUN(test_degenerate);
    RUN(test_injection_header_set);
    RUN(test_injection_serialize);
    RUN(test_reason_phrases);
    RUN(test_status_asymmetry);
    RUN(test_header_get);

    test_output_summary(tests_run, tests_failed, 0);
    return tests_failed ? 1 : 0;
}
