#ifndef BALLOTCLIENT_CODEC_H
#define BALLOTCLIENT_CODEC_H

/*
 * codec.h - the HTTTP wire mapping shared by every transport that carries a
 * bcl_request_t/bcl_response_t: ballotd's voter listener (TCP+tetrissh),
 * ballotd's admin listener (local, plaintext), and eventually the real
 * bcl_send. One encoding, several transports underneath it - the same
 * relationship tetrisctl's control plane has to libtetrissh (same
 * htttp_parse/serialize, just without the encryption).
 *
 * Method = the bcl_op_t name verbatim (htttp accepts any token as a method
 * by design - "is this allowed" is the caller's job, not the parser's).
 * Path scopes the resource. A "Cert-Name" header carries caller identity
 * where the domain call needs one. Body is "key=value" text lines, repeated
 * keys for arrays - never a packed struct, so there is nothing for
 * padding/endianness/ABI to disagree about, and a request reads like a log
 * line the same way db_exec's SQL-ish lines do.
 *
 * Deliberately NOT a blind field-by-field dump of the domain structs: a JOIN
 * response never includes the eligible-voter list (nobody's identity
 * belongs on that wire), and a RESULTS response only ever carries what
 * bb_get_results actually returns.
 *
 * Method / path / body per op:
 *
 *   CREATE  /election                    title=, open_time=, close_time=,
 *                                         option= (repeated), eligible= (repeated)
 *   OPEN    /election/<id>                (no body)
 *   CLOSE   /election/<id>                (no body)
 *   PUBLISH /election/<id>                (no body)
 *   JOIN    /election/<id>                (no body)
 *   CAST    /election/<id>/ballot         nonce=, payload=<hex>
 *   UPDATE  /election/<id>/ballot         nonce=, payload=<hex>
 *   RESULTS /election/<id>/results        (no body)
 *   CHECK   /election/<id>/check          hash=
 *
 * Response body, every op: first line "status=<bb_result_t name>" - the
 * exact code, since bcl_response_t.status is a bb_result_t and a coarse
 * HTTP status class can't distinguish e.g. BB_ERR_NOT_ELIGIBLE from
 * BB_ERR_CERT_EXPIRED - then op-specific lines (see codec.c for the exact
 * per-op field list). The HTTP status line itself (see bcl_http_status) is
 * only a coarse bucket for generic tooling; the body's status= line is
 * authoritative.
 */

#include "libballotclient/client.h"
#include "libhtttp/htttp.h"

#include <stdint.h>

/* req -> wire bytes (method/path/headers/body). *out_len = buffer capacity
 * on entry, bytes written on success. Returns HTTTP_OK, or an htttp_err_t /
 * -1 if req->op is not a recognised operation or a field could not be
 * encoded (e.g. a header value htttp rejects). */
int bcl_encode_request(const bcl_request_t *req, uint8_t *out, uint32_t *out_len);

/* Parsed HTTTP -> req. Returns 0 on success, -1 on an unrecognised method or
 * a malformed/missing required body field. Self-contained: does not read
 * req->op on entry, only ever writes the whole struct on success. */
int bcl_decode_request(const htttp_request_t *http, bcl_request_t *req);

/* resp -> wire bytes for the given op (op is needed here, unlike decode:
 * bcl_response_t is one struct shared by every op, and only op says which
 * of its fields are meaningful to serialize - see codec.c). */
int bcl_encode_response(bcl_op_t op, const bcl_response_t *resp, uint8_t *out,
                        uint32_t *out_len);

/* Parsed HTTTP -> resp. No op parameter needed: every op's response fields
 * use distinct key names, so whichever keys are present populate the
 * matching fields and the rest stay zeroed. */
int bcl_decode_response(const htttp_response_t *http, bcl_response_t *resp);

/* Coarse HTTP status bucket for a bb_result_t, restricted to the codes
 * htttp_reason() knows (htttp_serialize_response refuses anything else
 * outright) - both daemon channels use this one mapping. */
int bcl_http_status(bb_result_t status);

#endif /* BALLOTCLIENT_CODEC_H */
