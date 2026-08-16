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
 * Open the transport: TCP-connect to host:port, then the tetrissh handshake,
 * verified against ca_path. Must succeed before any bcl_send call. Returns
 * BB_OK, or BB_ERR_DB on any connect/handshake failure - libballotclient's
 * public surface is uniformly bb_result_t-typed, so a failure here is not
 * distinguished further (unreachable host vs. rejected cert); ballotu shows
 * one "could not reach ballotd" message either way.
 */
bb_result_t bcl_connect(bcl_ctx *ctx, const char *host, int port, const char *ca_path);

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
