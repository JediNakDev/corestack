/*
 * codec.c - the HTTTP wire mapping for bcl_request_t/bcl_response_t. See
 * codec.h for the wire shape. Pure struct<->bytes translation: nothing here
 * decides whether an operation is allowed or what it means, only whether
 * the bytes are well-formed.
 */

#include "libballotclient/codec.h"
#include "libballotbrain/ballotbrain.h" /* bb_state_str */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- op <-> method name -------------------------------------------------- */

static const struct {
  bcl_op_t op;
  const char *name;
} OP_NAMES[] = {
    {BCL_JOIN, "JOIN"},       {BCL_CAST, "CAST"},     {BCL_UPDATE, "UPDATE"},
    {BCL_RESULTS, "RESULTS"}, {BCL_CHECK, "CHECK"},   {BCL_CREATE, "CREATE"},
    {BCL_OPEN, "OPEN"},       {BCL_CLOSE, "CLOSE"},   {BCL_PUBLISH, "PUBLISH"},
    {BCL_ADMIN_RESULTS, "ADMIN_RESULTS"},
    {BCL_ADMIN_CHECK, "ADMIN_CHECK"},
    {BCL_ADMIN_NEXT_ID, "ADMIN_NEXT_ID"},
};
#define N_OPS (sizeof(OP_NAMES) / sizeof(OP_NAMES[0]))

static const char *method_from_op(bcl_op_t op) {
  for (size_t i = 0; i < N_OPS; i++)
    if (OP_NAMES[i].op == op) return OP_NAMES[i].name;
  return NULL;
}

static int op_from_method(const char *m, bcl_op_t *out) {
  for (size_t i = 0; i < N_OPS; i++)
    if (strcmp(OP_NAMES[i].name, m) == 0) {
      *out = OP_NAMES[i].op;
      return 0;
    }
  return -1;
}

/* ---- bb_result_t <-> name ------------------------------------------------- */

static const struct {
  bb_result_t code;
  const char *name;
} RESULT_NAMES[] = {
    {BB_OK, "BB_OK"},
    {BB_ERR_CONFIG_TITLE, "BB_ERR_CONFIG_TITLE"},
    {BB_ERR_CONFIG_OPTIONS, "BB_ERR_CONFIG_OPTIONS"},
    {BB_ERR_CONFIG_TIME, "BB_ERR_CONFIG_TIME"},
    {BB_ERR_CONFIG_ID_TAKEN, "BB_ERR_CONFIG_ID_TAKEN"},
    {BB_ERR_ILLEGAL_TRANSITION, "BB_ERR_ILLEGAL_TRANSITION"},
    {BB_ERR_NOT_OPEN, "BB_ERR_NOT_OPEN"},
    {BB_ERR_CLOSED, "BB_ERR_CLOSED"},
    {BB_ERR_NOT_PUBLISHED, "BB_ERR_NOT_PUBLISHED"},
    {BB_ERR_NOT_ELIGIBLE, "BB_ERR_NOT_ELIGIBLE"},
    {BB_ERR_CERT_INVALID, "BB_ERR_CERT_INVALID"},
    {BB_ERR_CERT_EXPIRED, "BB_ERR_CERT_EXPIRED"},
    {BB_ERR_BAD_OPTION, "BB_ERR_BAD_OPTION"},
    {BB_ERR_REPLAY, "BB_ERR_REPLAY"},
    {BB_ERR_DECRYPT, "BB_ERR_DECRYPT"},
    {BB_ERR_NOT_FOUND, "BB_ERR_NOT_FOUND"},
    {BB_ERR_NOT_JOINED, "BB_ERR_NOT_JOINED"},
    {BB_ERR_DB, "BB_ERR_DB"},
    {BB_ERR_NOT_IMPLEMENTED, "BB_ERR_NOT_IMPLEMENTED"},
};
#define N_RESULTS (sizeof(RESULT_NAMES) / sizeof(RESULT_NAMES[0]))

static const char *name_from_result(bb_result_t r) {
  for (size_t i = 0; i < N_RESULTS; i++)
    if (RESULT_NAMES[i].code == r) return RESULT_NAMES[i].name;
  return NULL;
}

static int result_from_name(const char *s, bb_result_t *out) {
  for (size_t i = 0; i < N_RESULTS; i++)
    if (strcmp(RESULT_NAMES[i].name, s) == 0) {
      *out = RESULT_NAMES[i].code;
      return 0;
    }
  return -1;
}

/* bb_state_str (ballotbrain.c) already gives "DRAFT"/"OPEN"/"CLOSED"/
 * "PUBLISHED" for encoding; only the reverse direction is missing. */
static int state_from_name(const char *s, bb_state_t *out) {
  static const struct {
    bb_state_t st;
    const char *name;
  } states[] = {
      {BB_STATE_DRAFT, "DRAFT"},
      {BB_STATE_OPEN, "OPEN"},
      {BB_STATE_CLOSED, "CLOSED"},
      {BB_STATE_PUBLISHED, "PUBLISHED"},
  };
  for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++)
    if (strcmp(states[i].name, s) == 0) {
      *out = states[i].st;
      return 0;
    }
  return -1;
}

/* ---- hex <-> bytes, for bb_ballot_t.payload -------------------------------- */

static int hex_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
  static const char digits[] = "0123456789abcdef";
  if (out_cap < in_len * 2 + 1) return -1;
  for (size_t i = 0; i < in_len; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 0x0f];
  }
  out[in_len * 2] = '\0';
  return 0;
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len) {
  size_t n = strlen(hex);
  if (n % 2 != 0) return -1;
  size_t bytes = n / 2;
  if (bytes > out_cap) return -1;
  for (size_t i = 0; i < bytes; i++) {
    int hi = hex_nibble(hex[2 * i]);
    int lo = hex_nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  *out_len = bytes;
  return 0;
}

/* ---- body writer: bounded "key=value\n" accumulation ----------------------- */

#define CODEC_BODY_MAX 8192

static int body_append(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap - *off) return -1;

  /* The body is line-structured, so a field carrying its own line break is
   * not a field with an odd character in it - it is extra lines.
   *
   * An election titled "Budget\neligible=mallory" encodes to a CREATE body
   * with an extra eligible= line in it, and the daemon reads what was sent,
   * not what was meant. The same input also fails to survive a round trip
   * (decode splits it back into two fields), which is how it was found -
   * tests/fuzz/fuzz_codec_request, on a title carrying a CR.
   *
   * Checked here rather than at the twenty-odd call sites: this is the only
   * function that puts a caller's string into the body, so it is the only
   * place the rule can be stated once. A newline as the LAST character is the
   * line terminator the format strings write deliberately; anywhere else, and
   * any CR at all, came out of a %s. Rejecting is right rather than escaping:
   * there is no unescaper on the other side, and inventing one would make the
   * wire format two formats. */
  for (int i = 0; i < n; i++) {
    char ch = buf[*off + (size_t)i];
    if (ch == '\r' || (ch == '\n' && i != n - 1)) return -1;
  }

  *off += (size_t)n;
  return 0;
}

/* ---- body reader: iterate "key=value" lines -------------------------------- */

typedef void (*kv_fn)(const char *key, const char *val, void *ctx);

static void body_for_each(const uint8_t *body, uint32_t body_len, kv_fn fn, void *ctx) {
  /* A bodyless message arrives here as (NULL, 0) - htttp leaves req->body
   * NULL when there is no body, and both decoders call this unconditionally
   * rather than checking at each of their call sites. NULL + 0 is undefined
   * behaviour even though every compiler in practice yields NULL, so the
   * check is here rather than in the five callers. Found by
   * tests/fuzz/fuzz_codec_request and fuzz_codec_response. */
  if (body == NULL || body_len == 0) return;

  const char *p = (const char *)body;
  const char *end = p + body_len;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *line_end = nl ? nl : end;
    const char *le = line_end;
    if (le > p && le[-1] == '\r') le--;

    const char *eq = memchr(p, '=', (size_t)(le - p));
    if (eq && eq > p) {
      char key[64];
      char val[320];
      size_t klen = (size_t)(eq - p);
      if (klen >= sizeof key) klen = sizeof key - 1;
      memcpy(key, p, klen);
      key[klen] = '\0';

      size_t vlen = (size_t)(le - (eq + 1));
      if (vlen >= sizeof val) vlen = sizeof val - 1;
      memcpy(val, eq + 1, vlen);
      val[vlen] = '\0';

      fn(key, val, ctx);
    }
    p = nl ? nl + 1 : end;
  }
}

/* ---- path <-> election id --------------------------------------------------
 *
 * Every id-bearing path is "/election/<id>[/suffix]"; %[^/] stops at the
 * first '/' regardless of what suffix follows (or none), so one pattern
 * extracts the id for every op that carries one - the method already tells
 * the caller which op this is, so the suffix itself is never inspected. */

static int path_election_id(const char *path, char out[BB_ID_LEN]) {
  out[0] = '\0';
  /* width = BB_ID_LEN - 1 */
  if (sscanf(path, "/election/%15[^/]", out) != 1 || out[0] == '\0') return -1;
  return 0;
}

/* ---- request body: per-op decode ------------------------------------------- */

typedef struct {
  bb_config_t *cfg;
  char *id; /* req->election_id: the desired-id override, "" if omitted */
  int opt_i;
  int elig_i;
} create_ctx_t;

static void create_kv(const char *k, const char *v, void *ctxp) {
  create_ctx_t *c = ctxp;
  if (strcmp(k, "election_id") == 0) {
    snprintf(c->id, BB_ID_LEN, "%s", v);
  } else if (strcmp(k, "title") == 0) {
    snprintf(c->cfg->title, BB_TITLE_LEN, "%s", v);
  } else if (strcmp(k, "open_time") == 0) {
    snprintf(c->cfg->open_time, BB_TIME_LEN, "%s", v);
  } else if (strcmp(k, "close_time") == 0) {
    snprintf(c->cfg->close_time, BB_TIME_LEN, "%s", v);
  } else if (strcmp(k, "option") == 0) {
    if (c->opt_i < BB_MAX_OPTIONS) snprintf(c->cfg->options[c->opt_i++], BB_OPTION_LEN, "%s", v);
  } else if (strcmp(k, "eligible") == 0) {
    if (c->elig_i < BB_MAX_VOTERS) snprintf(c->cfg->eligible[c->elig_i++], BB_CERT_LEN, "%s", v);
  }
}

typedef struct {
  bb_ballot_t *ballot;
  int got_payload;
} ballot_ctx_t;

static void ballot_kv(const char *k, const char *v, void *ctxp) {
  ballot_ctx_t *c = ctxp;
  if (strcmp(k, "nonce") == 0) {
    snprintf(c->ballot->nonce, BB_NONCE_LEN, "%s", v);
  } else if (strcmp(k, "payload") == 0) {
    size_t n = 0;
    if (hex_decode(v, c->ballot->payload, sizeof c->ballot->payload, &n) == 0) {
      c->ballot->payload_len = n;
      c->got_payload = 1;
    }
  }
}

static void hash_kv(const char *k, const char *v, void *ctxp) {
  char *hash = ctxp;
  if (strcmp(k, "hash") == 0) snprintf(hash, BB_HASH_LEN, "%s", v);
}

/* ---- public: request ------------------------------------------------------- */

int bcl_encode_request(const bcl_request_t *req, uint8_t *out, uint32_t *out_len) {
  if (!req || !out || !out_len) return -1;

  const char *method = method_from_op(req->op);
  if (!method) return -1;

  char path[HTTTP_MAX_PATH];
  char body[CODEC_BODY_MAX];
  size_t off = 0;
  body[0] = '\0';

  switch (req->op) {
    case BCL_CREATE:
      snprintf(path, sizeof path, "/election");
      if (req->election_id[0])
        if (body_append(body, sizeof body, &off, "election_id=%s\n", req->election_id)) return -1;
      if (body_append(body, sizeof body, &off, "title=%s\n", req->config.title)) return -1;
      if (body_append(body, sizeof body, &off, "open_time=%s\n", req->config.open_time)) return -1;
      if (body_append(body, sizeof body, &off, "close_time=%s\n", req->config.close_time)) return -1;
      for (int i = 0; i < req->config.option_count; i++)
        if (body_append(body, sizeof body, &off, "option=%s\n", req->config.options[i])) return -1;
      for (int i = 0; i < req->config.eligible_count; i++)
        if (body_append(body, sizeof body, &off, "eligible=%s\n", req->config.eligible[i])) return -1;
      break;

    case BCL_OPEN:
    case BCL_CLOSE:
    case BCL_PUBLISH:
    case BCL_JOIN:
    case BCL_RESULTS:
    case BCL_ADMIN_RESULTS:
      snprintf(path, sizeof path, "/election/%s", req->election_id);
      break;

    case BCL_CAST:
    case BCL_UPDATE:
      snprintf(path, sizeof path, "/election/%s/ballot", req->election_id);
      if (body_append(body, sizeof body, &off, "nonce=%s\n", req->ballot.nonce)) return -1;
      {
        char hex[BB_CIPHERTEXT_MAX * 2 + 1];
        if (hex_encode(req->ballot.payload, req->ballot.payload_len, hex, sizeof hex)) return -1;
        if (body_append(body, sizeof body, &off, "payload=%s\n", hex)) return -1;
      }
      break;

    case BCL_CHECK:
    case BCL_ADMIN_CHECK:
      snprintf(path, sizeof path, "/election/%s/check", req->election_id);
      if (body_append(body, sizeof body, &off, "hash=%s\n", req->hash)) return -1;
      break;

    case BCL_ADMIN_NEXT_ID:
      snprintf(path, sizeof path, "/election/next-id");
      break;

    default:
      return -1;
  }

  htttp_request_t h;
  memset(&h, 0, sizeof h);
  snprintf(h.method, sizeof h.method, "%s", method);
  snprintf(h.path, sizeof h.path, "%s", path);
  if (req->cert_name[0] != '\0') {
    if (htttp_header_set(h.headers, &h.n_headers, "Cert-Name", req->cert_name) != HTTTP_OK)
      return -1;
  }
  if (off > 0) {
    h.body = (const uint8_t *)body;
    h.body_len = (uint32_t)off;
  }

  return htttp_serialize_request(&h, out, out_len) == HTTTP_OK ? 0 : -1;
}

int bcl_decode_request(const htttp_request_t *http, bcl_request_t *req) {
  if (!http || !req) return -1;
  memset(req, 0, sizeof *req);

  bcl_op_t op;
  if (op_from_method(http->method, &op) != 0) return -1;
  req->op = op;

  const char *cert = htttp_header_get(http->headers, http->n_headers, "Cert-Name");
  if (cert) snprintf(req->cert_name, BB_CERT_LEN, "%s", cert);

  switch (op) {
    case BCL_CREATE: {
      create_ctx_t c = {&req->config, req->election_id, 0, 0};
      body_for_each(http->body, http->body_len, create_kv, &c);
      req->config.option_count = c.opt_i;
      req->config.eligible_count = c.elig_i;
      return 0;
    }

    case BCL_ADMIN_NEXT_ID:
      return 0;

    case BCL_OPEN:
    case BCL_CLOSE:
    case BCL_PUBLISH:
    case BCL_JOIN:
    case BCL_RESULTS:
    case BCL_ADMIN_RESULTS:
      return path_election_id(http->path, req->election_id);

    case BCL_CAST:
    case BCL_UPDATE: {
      if (path_election_id(http->path, req->election_id) != 0) return -1;
      snprintf(req->ballot.cert_name, BB_CERT_LEN, "%s", req->cert_name);
      ballot_ctx_t c = {&req->ballot, 0};
      body_for_each(http->body, http->body_len, ballot_kv, &c);
      return c.got_payload ? 0 : -1;
    }

    case BCL_CHECK:
    case BCL_ADMIN_CHECK:
      if (path_election_id(http->path, req->election_id) != 0) return -1;
      body_for_each(http->body, http->body_len, hash_kv, req->hash);
      return req->hash[0] != '\0' ? 0 : -1;

    default:
      return -1;
  }
}

/* ---- public: response ------------------------------------------------------- */

int bcl_http_status(bb_result_t status) {
  switch (status) {
    case BB_OK:
      return 200;
    case BB_ERR_CONFIG_TITLE:
    case BB_ERR_CONFIG_OPTIONS:
    case BB_ERR_CONFIG_TIME:
    case BB_ERR_BAD_OPTION:
    case BB_ERR_DECRYPT:
      return 400;
    case BB_ERR_NOT_ELIGIBLE:
    case BB_ERR_CERT_INVALID:
    case BB_ERR_CERT_EXPIRED:
      return 403;
    case BB_ERR_NOT_FOUND:
      return 404;
    case BB_ERR_ILLEGAL_TRANSITION:
    case BB_ERR_NOT_OPEN:
    case BB_ERR_CLOSED:
    case BB_ERR_NOT_PUBLISHED:
    case BB_ERR_REPLAY:
    case BB_ERR_NOT_JOINED:
    case BB_ERR_CONFIG_ID_TAKEN:
      return 409;
    case BB_ERR_DB:
    case BB_ERR_NOT_IMPLEMENTED:
    default:
      /* htttp_reason() has no phrase for 501, so "not implemented yet" is
       * bucketed with "infrastructure not ready" rather than invented a
       * status htttp_serialize_response would refuse to emit. */
      return 500;
  }
}

/* Response body per op, after the shared "status=" line:
 *   CREATE/ADMIN_NEXT_ID election_id=
 *   OPEN/CLOSE/PUBLISH (nothing else)
 *   JOIN               election_id=, title=, state=, open_time=, close_time=,
 *                       option= (repeated) - never the eligible list
 *   CAST/UPDATE        hash=, issued_at=
 *   RESULTS/ADMIN_RESULTS tally_count=, tally=<csv>, hash_count=,
 *                       row=<hash>,<option_index>,<version>,<superseded> (repeated)
 *   CHECK/ADMIN_CHECK  found=, found_option=, found_option_name= (only when found)
 * Every field is emitted only when it has content, so a failed request's
 * response is just the status= line. CREATE's request body also carries an
 * optional election_id= (the operator's desired id; blank auto-allocates). */
int bcl_encode_response(bcl_op_t op, const bcl_response_t *resp, uint8_t *out,
                        uint32_t *out_len) {
  if (!resp || !out || !out_len) return -1;

  char body[CODEC_BODY_MAX];
  size_t off = 0;
  body[0] = '\0';

  const char *sname = name_from_result(resp->status);
  if (!sname) return -1;
  if (body_append(body, sizeof body, &off, "status=%s\n", sname)) return -1;

  switch (op) {
    case BCL_CREATE:
    case BCL_ADMIN_NEXT_ID:
      if (resp->election.id[0])
        if (body_append(body, sizeof body, &off, "election_id=%s\n", resp->election.id))
          return -1;
      break;

    case BCL_OPEN:
    case BCL_CLOSE:
    case BCL_PUBLISH:
      break;

    case BCL_JOIN:
      if (resp->election.id[0]) {
        if (body_append(body, sizeof body, &off, "election_id=%s\n", resp->election.id) ||
            body_append(body, sizeof body, &off, "title=%s\n", resp->election.title) ||
            body_append(body, sizeof body, &off, "state=%s\n", bb_state_str(resp->election.state)) ||
            body_append(body, sizeof body, &off, "open_time=%s\n", resp->election.open_time) ||
            body_append(body, sizeof body, &off, "close_time=%s\n", resp->election.close_time) ||
            body_append(body, sizeof body, &off, "has_prior_ballot=%d\n", resp->has_prior_ballot) ||
            body_append(body, sizeof body, &off, "prior_ballot_version=%d\n",
                        resp->prior_ballot_version))
          return -1;
        for (int i = 0; i < resp->election.option_count; i++)
          if (body_append(body, sizeof body, &off, "option=%s\n", resp->election.options[i]))
            return -1;
      }
      break;

    case BCL_CAST:
    case BCL_UPDATE:
      if (resp->receipt.hash[0]) {
        if (body_append(body, sizeof body, &off, "hash=%s\n", resp->receipt.hash) ||
            body_append(body, sizeof body, &off, "issued_at=%s\n", resp->receipt.issued_at))
          return -1;
      }
      break;

    case BCL_RESULTS:
    case BCL_ADMIN_RESULTS:
      /* Reuses JOIN's "election_id"/"title" keys - response_kv() below
       * writes both into r->election regardless of which op produced them,
       * so a results screen can show the title next to the id, not just
       * the id the caller already typed in. */
      if (body_append(body, sizeof body, &off, "election_id=%s\n", resp->election.id) ||
          body_append(body, sizeof body, &off, "title=%s\n", resp->election.title))
        return -1;
      if (body_append(body, sizeof body, &off, "tally_count=%d\n", resp->option_count))
        return -1;
      if (resp->option_count > 0) {
        if (body_append(body, sizeof body, &off, "tally=")) return -1;
        for (int i = 0; i < resp->option_count; i++)
          if (body_append(body, sizeof body, &off, "%s%d", i ? "," : "", resp->tally[i]))
            return -1;
        if (body_append(body, sizeof body, &off, "\n")) return -1;
        /* Own key, not "option=": that name already means JOIN's option
         * list in the shared decoder, and the two must never collide (see
         * "found_option" for the same reasoning on CHECK). */
        for (int i = 0; i < resp->option_count; i++)
          if (body_append(body, sizeof body, &off, "result_option=%s\n", resp->options[i]))
            return -1;
      }
      if (body_append(body, sizeof body, &off, "hash_count=%d\n", resp->hash_count)) return -1;
      for (int i = 0; i < resp->hash_count; i++) {
        const bb_ballot_hash_t *h = &resp->hashes[i];
        if (body_append(body, sizeof body, &off, "row=%s,%d,%d,%d\n", h->hash, h->option_index,
                        h->version, h->superseded))
          return -1;
      }
      break;

    case BCL_CHECK:
    case BCL_ADMIN_CHECK:
      if (body_append(body, sizeof body, &off, "found=%d\n", resp->found)) return -1;
      if (resp->found) {
        if (body_append(body, sizeof body, &off, "found_option=%d\n", resp->found_option))
          return -1;
        if (body_append(body, sizeof body, &off, "found_option_name=%s\n",
                        resp->found_option_name))
          return -1;
      }
      break;

    default:
      return -1;
  }

  htttp_response_t h;
  memset(&h, 0, sizeof h);
  h.status = bcl_http_status(resp->status);
  if (off > 0) {
    h.body = (const uint8_t *)body;
    h.body_len = (uint32_t)off;
  }

  return htttp_serialize_response(&h, out, out_len) == HTTTP_OK ? 0 : -1;
}

typedef struct {
  bcl_response_t *resp;
  int row_i;
  int result_opt_i;
  int status_valid; /* status= was present and named a real bb_result_t */
} resp_ctx_t;

static void response_kv(const char *k, const char *v, void *ctxp) {
  resp_ctx_t *c = ctxp;
  bcl_response_t *r = c->resp;

  if (strcmp(k, "status") == 0) {
    bb_result_t st;
    if (result_from_name(v, &st) == 0) {
      r->status = st;
      c->status_valid = 1;
    }
  } else if (strcmp(k, "election_id") == 0) {
    snprintf(r->election.id, BB_ID_LEN, "%s", v);
  } else if (strcmp(k, "title") == 0) {
    snprintf(r->election.title, BB_TITLE_LEN, "%s", v);
  } else if (strcmp(k, "state") == 0) {
    bb_state_t st;
    if (state_from_name(v, &st) == 0) r->election.state = st;
  } else if (strcmp(k, "open_time") == 0) {
    snprintf(r->election.open_time, BB_TIME_LEN, "%s", v);
  } else if (strcmp(k, "close_time") == 0) {
    snprintf(r->election.close_time, BB_TIME_LEN, "%s", v);
  } else if (strcmp(k, "has_prior_ballot") == 0) {
    r->has_prior_ballot = atoi(v);
  } else if (strcmp(k, "prior_ballot_version") == 0) {
    r->prior_ballot_version = atoi(v);
  } else if (strcmp(k, "option") == 0) {
    if (r->election.option_count < BB_MAX_OPTIONS)
      snprintf(r->election.options[r->election.option_count++], BB_OPTION_LEN, "%s", v);
  } else if (strcmp(k, "hash") == 0) {
    snprintf(r->receipt.hash, BB_HASH_LEN, "%s", v);
  } else if (strcmp(k, "issued_at") == 0) {
    snprintf(r->receipt.issued_at, BB_TIME_LEN, "%s", v);
  } else if (strcmp(k, "tally_count") == 0) {
    r->option_count = atoi(v);
  } else if (strcmp(k, "result_option") == 0) {
    if (c->result_opt_i < BB_MAX_OPTIONS)
      snprintf(r->options[c->result_opt_i++], BB_OPTION_LEN, "%s", v);
  } else if (strcmp(k, "tally") == 0) {
    int i = 0;
    const char *p = v;
    while (*p && i < BB_MAX_OPTIONS) {
      r->tally[i++] = atoi(p);
      const char *comma = strchr(p, ',');
      if (!comma) break;
      p = comma + 1;
    }
  } else if (strcmp(k, "hash_count") == 0) {
    r->hash_count = atoi(v);
  } else if (strcmp(k, "row") == 0) {
    if (c->row_i < BB_MAX_VOTERS) {
      bb_ballot_hash_t *h = &r->hashes[c->row_i];
      char hash[BB_HASH_LEN];
      int opt = 0, ver = 0, sup = 0;
      if (sscanf(v, "%64[^,],%d,%d,%d", hash, &opt, &ver, &sup) == 4) {
        snprintf(h->hash, BB_HASH_LEN, "%s", hash);
        h->option_index = opt;
        h->version = ver;
        h->superseded = sup;
        c->row_i++;
      }
    }
  } else if (strcmp(k, "found") == 0) {
    r->found = atoi(v);
  } else if (strcmp(k, "found_option") == 0) {
    r->found_option = atoi(v);
  } else if (strcmp(k, "found_option_name") == 0) {
    snprintf(r->found_option_name, BB_OPTION_LEN, "%s", v);
  }
}

int bcl_decode_response(const htttp_response_t *http, bcl_response_t *resp) {
  if (!http || !resp) return -1;
  memset(resp, 0, sizeof *resp);

  resp_ctx_t c = {resp, 0, 0, 0};
  body_for_each(http->body, http->body_len, response_kv, &c);

  /* The counts are what the SENDER claimed; these are what actually arrived.
   *
   * tally_count= and hash_count= are read straight off the wire by atoi,
   * while the row= and option= writers stop at BB_MAX_OPTIONS/BB_MAX_VOTERS -
   * so a body saying "hash_count=78" with two rows in it produced a struct
   * announcing 78 entries in a 64-entry array with 2 of them filled. Every
   * caller loops to the count, so the daemon (or anything able to answer as
   * it) chose how far past the array its client would read. A negative count
   * did the same in the other direction.
   *
   * Clamping rather than rejecting: a short or over-claimed table is the
   * daemon disagreeing with itself, not a reason to throw away a response
   * whose status= line is perfectly readable - and the tally is only ever
   * displayed alongside the rows it came with. Found by
   * tests/fuzz/fuzz_codec_response. */
  if (resp->hash_count < 0 || resp->hash_count > c.row_i)
    resp->hash_count = c.row_i;
  if (resp->option_count < 0 || resp->option_count > BB_MAX_OPTIONS)
    resp->option_count = c.result_opt_i;

  return c.status_valid ? 0 : -1;
}
