#ifndef BALLOTCLIENT_H
#define BALLOTCLIENT_H

/*
 * libballotclient - client-side logic shared by ballotu (voter) and ballotctl
 * (admin). It reuses the canonical domain model from libballotbrain rather than
 * redefining it.
 *
 * Like the daemon library, this holds the *logic*. The wire transport
 * (libtetrissh / libhtttp) and the ballot cryptography sit behind stubbed seams
 * and are wired in once the teammate's transport layer and PKI land. The pure
 * decision logic (vote routing, join-outcome classification, config assembly)
 * is complete now.
 *
 * Naming: bcl_* shared core, bu_* voter entry points, bc_* admin entry points.
 */

#include "libballotbrain/types.h"

#include <stdio.h>

/* Opaque per-client context (log sink today; the session handle later). */
typedef struct bcl_ctx bcl_ctx;

bcl_ctx *bcl_create(void);
void bcl_destroy(bcl_ctx *ctx);
void bcl_set_log(bcl_ctx *ctx, FILE *sink);
void bcl_log(bcl_ctx *ctx, const char *fmt, ...);

/* Every request the clients can make of the daemon. */
typedef enum {
  BCL_JOIN,     /* voter: UC-2 */
  BCL_CAST,     /* voter: UC-3 */
  BCL_UPDATE,   /* voter: UC-4 */
  BCL_RESULTS,  /* observer: UC-5 */
  BCL_CHECK,    /* voter: UC-6 */
  BCL_CREATE,   /* admin: UC-1 create */
  BCL_OPEN,     /* admin: UC-1 open */
  BCL_CLOSE,    /* admin: close */
  BCL_PUBLISH,  /* admin: publish */
  BCL_ADMIN_RESULTS, /* admin: UC-5 results, no eligible-list check - see bb_get_results_admin */
  BCL_ADMIN_CHECK,   /* admin: UC-6 check via the admin socket - see bb_lookup_hash */
  BCL_ADMIN_NEXT_ID  /* admin: UC-1 peek - what CREATE would auto-allocate, no side effect */
} bcl_op_t;

/* One request. Only the fields relevant to `op` are populated. */
typedef struct {
  bcl_op_t op;
  char cert_name[BB_CERT_LEN];
  char election_id[BB_ID_LEN];
  bb_ballot_t ballot;      /* CAST / UPDATE */
  char hash[BB_HASH_LEN];  /* CHECK: derived receipt hash */
  bb_config_t config;      /* CREATE */
} bcl_request_t;

/* One response. Fields are filled per the request op once transport is real. */
typedef struct {
  bb_result_t status;
  bb_election_t election;               /* JOIN / RESULTS */
  int has_prior_ballot;                 /* JOIN: 1 if this voter already has a ballot here */
  int prior_ballot_version;             /* JOIN: that ballot's version, if has_prior_ballot */
  bb_receipt_t receipt;                 /* CAST / UPDATE */
  int tally[BB_MAX_OPTIONS];            /* RESULTS */
  int option_count;                     /* RESULTS: how many of tally[]/options[]
                                          * are valid - bb_results_t carries this
                                          * but the field was missing here, so the
                                          * wire codec had no way to know how many
                                          * entries to send */
  char options[BB_MAX_OPTIONS][BB_OPTION_LEN]; /* RESULTS: names, parallel to tally[] -
                                                 * bb_results_t already carries these */
  bb_ballot_hash_t hashes[BB_MAX_VOTERS]; /* RESULTS */
  int hash_count;
  int found;                            /* CHECK: 1 if hash counted */
  int found_option;                     /* CHECK: revealed only to the key holder */
  char found_option_name[BB_OPTION_LEN]; /* CHECK: found_option's display text */
} bcl_response_t;

/*
 * Why a bcl_connect attempt failed.
 *
 * bb_result_t cannot carry this: it is the wire status enum, shared with
 * libballotbrain and serialised by the codec, and a "the cert was rejected"
 * value has no meaning as a daemon verdict - the daemon never sends it. So
 * the cause rides beside the result rather than inside it, and the enum stays
 * client-local.
 *
 * The distinction is not cosmetic. "Nothing is listening" and "something is
 * listening but it is not the ballotd this CA vouches for" are the same
 * BB_ERR_DB to a caller, and reporting the second as the first tells a voter
 * to go check a server that is running perfectly well - while hiding the one
 * failure that actually matters for a vote's integrity.
 */
typedef enum {
  BCL_CONN_OK,           /* connected and handshaken */
  BCL_CONN_ERR_ADDRESS,  /* host is not a dotted-quad IPv4 address */
  BCL_CONN_ERR_SOCKET,   /* socket() failed, or the ctx/args were unusable */
  BCL_CONN_ERR_REFUSED,  /* TCP never came up: refused, no route, timed out */
  BCL_CONN_ERR_CERT,     /* handshake ran, server cert failed CA verification */
  BCL_CONN_ERR_IO,       /* TCP came up, peer hung up during the handshake */
  BCL_CONN_ERR_PROTO     /* handshake reached, crypto or protocol error */
} bcl_conn_t;

/*
 * Open the transport: TCP-connect to host:port, then the tetrissh handshake,
 * verified against ca_path. Must succeed before any bcl_send call.
 *
 * Returns BB_OK, or BB_ERR_DB on any connect/handshake failure - the public
 * surface stays uniformly bb_result_t-typed. When `why` is non-NULL it also
 * receives the cause, which is what a UI needs to say something true: only
 * BCL_CONN_ERR_ADDRESS/SOCKET/REFUSED mean the TCP step itself failed, so a
 * progress panel must not mark that step bad for the others - the server WAS
 * reached in those cases, and saying otherwise sends the voter looking in the
 * wrong place (this is the mistake tetrisu's screen_connect documents at its
 * CLIENT_ERR_CONNECT branch).
 *
 * bcl_connect is bcl_connect_why with `why` = NULL, kept for callers that do
 * not report causes (the tests, and ballotctl's admin-only path).
 */
bb_result_t bcl_connect_why(bcl_ctx *ctx, const char *host, int port, const char *ca_path,
                            bcl_conn_t *why);
bb_result_t bcl_connect(bcl_ctx *ctx, const char *host, int port, const char *ca_path);

/*
 * Is the voter-channel transport still usable?
 *
 * 1 only while a bcl_connect has succeeded and no send/recv has since marked
 * the session dead (see send_via_session: a failed session_send, or a recv
 * that returned SESSION_ERR_IO/TOOBIG, clears the flag and every later
 * bcl_send fails immediately).
 *
 * Exists because BB_ERR_DB from bcl_send is ambiguous - it is both "the wire
 * broke" and "the daemon answered, and its answer was a database failure" -
 * and a UI that wants to offer a reconnect must not offer one for the second.
 * Ask here instead of guessing from the status.
 */
int bcl_connected(const bcl_ctx *ctx);

/* Close the transport, if one is open. Safe to call on an unconnected or
 * already-disconnected ctx. NOT called automatically by bcl_destroy (that
 * would force every caller, including unit tests that fake bcl_send, to
 * link the real transport) - call this yourself before bcl_destroy if you
 * connected. */
void bcl_disconnect(bcl_ctx *ctx);

/*
 * The pre-auth exchange, client side: send one LOGIN or REGISTER over the
 * session bcl_connect() already opened, wait for ballotd's auth_login()
 * (libtetrisauth, unmodified) to answer, and hand back the raw HTTTP status
 * - 200 success (a JWT rides in the body; nothing here reads it, same as
 * tetriSH's own client - see docs/libtetrisauth.md), 400 malformed, 401
 * wrong password, 404 no such user, 409 username taken (REGISTER only), 500
 * account service unreachable. Separate from bcl_send: LOGIN/REGISTER are
 * not bcl_op_t values (that enum is BallotBox's ballot protocol; this is
 * tetriSH's account protocol, riding the same wire), and auth_login() owns
 * the exchange on the daemon side before any bcl_op_t request is even
 * legal - see ballotd/session.c's call site.
 *
 * `method` must be "LOGIN" or "REGISTER" (case-sensitive, matches
 * tauth.c's auth_method_of()). `password` is never copied by this
 * function beyond the one stack buffer it builds the wire body in and
 * scrubs before returning - callers should scrub their own copy too.
 *
 * Returns 0 with *out_status set to the response's HTTTP status, or -1 on
 * a transport-level failure (not connected, send/recv/parse error) with
 * *out_status untouched.
 */
int bcl_auth(bcl_ctx *ctx, const char *method, const char *username, const char *password,
             int *out_status);

/*
 * Name the local admin socket (ballotd's ctl_frame-framed AF_UNIX channel)
 * for CREATE/OPEN/CLOSE/PUBLISH. Unlike bcl_connect, this opens nothing -
 * the admin channel is one connection per request (ballotd's ctl_thread
 * closes after every reply), so bcl_send dials fresh each time an admin op
 * goes out. Call once before any admin-op bcl_send; ballotctl needs this
 * and never bcl_connect (it has no voter-channel traffic at all).
 */
void bcl_set_ctl_path(bcl_ctx *ctx, const char *path);

/*
 * Transport seam. Routes on req->op: CREATE/OPEN/CLOSE/PUBLISH dial the
 * admin socket named by bcl_set_ctl_path (one connection, this call only);
 * everything else uses the persistent session opened by bcl_connect. Fills
 * `resp`. Returns BB_ERR_DB if the relevant transport isn't configured or
 * the round trip itself failed (nothing usable in `resp`); otherwise
 * returns resp->status, whatever the daemon answered.
 */
bb_result_t bcl_send(bcl_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp);

#endif /* BALLOTCLIENT_H */
