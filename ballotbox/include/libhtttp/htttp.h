#ifndef HTTTP_H
#define HTTTP_H

/* libhtttp — HTTTP/1.0 grammar layer (Hypertext Tetris Transfer Protocol).
 *
 * Turns decrypted bytes into structured requests/responses and back.
 * Knows nothing about crypto or sockets: input/output is byte buffers only;
 * transport is session_send/session_recv (libtetrissh).
 *
 * App-agnostic by design (shared by tetriSH and BallotBox):
 *   - Any token parses as a method (MOVE, JOIN, CAST, UPD_GAME, ...).
 *     "Is this method allowed" is the app dispatcher's job, not the parser's.
 *   - Headers are a generic key-value list (Player-Id vs Voter-Id).
 *     "Which headers matter" is the app's job.
 *   - Requests flow BOTH directions: clients send commands (MOVE, JOIN),
 *     the server pushes state as requests too (UPD_GAME, UPD_SESSION).
 *     Both ends therefore use both parse functions.
 *
 * Grammar:
 *   request  = METHOD SP PATH SP "HTTTP/1.0" CRLF *(header CRLF) CRLF [body]
 *   response = "HTTTP/1.0" SP STATUS SP REASON CRLF *(header CRLF) CRLF [body]
 *   header   = KEY ":" SP VALUE
 *
 * tetriSH method mapping (see htttp.md; args ride in path or body — the lib
 * only sees METHOD + PATH + headers + body):
 *   JOIN | LEAVE | START   /room/<id>              id=0 on JOIN -> new room
 *   MOVE | ROTATE | DROP   /room/<id>/player/<pid>
 *   HOLD                   /room/<id>/player/<pid> (ours, not in the handout)
 *   HISTORY                /player/history         (ours, not in the handout)
 *   STATE                  /room/<id>              (server -> client)
 *   UPD_SESSION /session/state  UPD_RESULT /game/result   (server -> client)
 *   UPD_HISTORY /player/history                          (server -> client)
 *
 * Command bodies are words, not digits: MOVE LEFT|RIGHT, ROTATE CW|CCW,
 * DROP SOFT|HARD. Content-Type is application/tetris-command on a client
 * request with a body, application/tetris-state on a server push.
 */

#include <stdint.h>
#include <stddef.h>

#define HTTTP_VERSION "HTTTP/1.0"
#define VERSION_LEN (sizeof(HTTTP_VERSION) - 1)
#define HTTTP_MAX_METHOD 16
#define HTTTP_MAX_PATH 256
#define HTTTP_MAX_HEADERS 32
#define HTTTP_MAX_KEY 64
#define HTTTP_MAX_VALUE 256

/* Whole serialized message must fit one libtetrissh frame (64 KiB wire
 * limit). Bound checked by the app when sizing its serialize buffer. */
#define HTTTP_MAX_FRAME 65536

/* Parse/serialize results */
typedef enum
{
    HTTTP_OK = 0,
    HTTTP_ERR_MALFORMED =
        -1, /* bad request line / header syntax -> app sends 400 */
    HTTTP_ERR_TOOLONG =
        -2, /* method/path/header exceeds bound  -> app sends 400 */
    HTTTP_ERR_LENGTH =
        -3, /* body size != Content-Length       -> app sends 400 */
    HTTTP_ERR_NOSPACE = -4, /* output buffer too small (serialize) */
} htttp_err_t;

typedef struct
{
    char key[HTTTP_MAX_KEY];
    char value[HTTTP_MAX_VALUE];
} htttp_header_t;

typedef struct
{
    char method[HTTTP_MAX_METHOD]; /* any token — validation is app's job */
    char path[HTTTP_MAX_PATH];
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t n_headers;
    const uint8_t *body; /* points INTO the input buffer — no copy, no free */
    uint32_t body_len;   /* = Content-Length */
} htttp_request_t;

typedef struct
{
    int status; /* 200, 201, 400, 401, 403, 404, 409, 413, 429, 500 */
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t n_headers;
    const uint8_t *body;
    uint32_t body_len;
} htttp_response_t;

/* --- Parsing -------------------------------------------------------------- */

/* Parse one complete request from buf (exactly one frame from session_recv —
 * framing guarantees message boundaries, so no incremental parsing needed).
 * Zero-copy: req->body points into buf; buf must outlive req. */
int htttp_parse_request(const uint8_t *buf, uint32_t len, htttp_request_t *req);

/* Mirror: parse one complete response. */
int htttp_parse_response(const uint8_t *buf, uint32_t len,
                         htttp_response_t *res);

/* --- Serializing ---------------------------------------------------------- */

/* Write request as wire bytes into out. On entry *out_len = capacity;
 * on success *out_len = bytes written. Adds Content-Length automatically
 * when body_len > 0. */
int htttp_serialize_request(const htttp_request_t *req, uint8_t *out,
                            uint32_t *out_len);

/* Response mirror. Adds Content-Length and Date (RFC 1123) automatically. */
int htttp_serialize_response(const htttp_response_t *res, uint8_t *out,
                             uint32_t *out_len);

/* --- Header helpers ------------------------------------------------------- */

/* Case-insensitive lookup ("Player-Id" == "player-id"). NULL if absent. */
const char *htttp_header_get(const htttp_header_t *headers, size_t n,
                             const char *key);

/* Append key/value; HTTTP_ERR_TOOLONG if full or oversized. */
int htttp_header_set(htttp_header_t *headers, size_t *n, const char *key,
                     const char *value);

/* --- Misc ----------------------------------------------------------------- */

/* Canonical reason phrase: 200->"OK", 404->"Not Found",
 * 413->"Payload Too Large", ... NULL for unknown status. */
const char *htttp_reason(int status);

#endif /* HTTTP_H */
