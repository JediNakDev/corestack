#ifndef BALLOTBRAIN_H
#define BALLOTBRAIN_H

/*
 * libballotbrain - the daemon-side authoritative logic for BallotBox (the
 * `BallotdService` control class). This is the umbrella header: consumers
 * include it to get the whole API.
 *
 * Scope note: this library holds the *logic*. Storage (SimpleDB) and
 * cryptography sit behind narrow seams (db.h, crypto.h) whose implementations
 * are stubbed today. The logic above them is complete and enforces every rule
 * against the seam contract, so each function is unit-testable now with the
 * seams substituted; only the seam bodies wait on SimpleDB and PKI.
 */

#include "libballotbrain/types.h"
#include "libballotbrain/db.h"
#include "libballotbrain/crypto.h"

#include <stdio.h>

/* Opaque per-instance context (holds config + the log sink; later the store
 * handle and the write mutex). No hidden file-scope state, so isolated
 * instances - and isolated tests - are possible. */
typedef struct bb_ctx bb_ctx;

bb_ctx *bb_create(void);
void bb_destroy(bb_ctx *ctx);

/* Redirect the seam/operation log (defaults to stderr). Pass NULL to silence. */
void bb_set_log(bb_ctx *ctx, FILE *sink);

/* Internal: emit one operation-log line. Exposed for the seam sources. */
void bb_log(bb_ctx *ctx, const char *fmt, ...);

/* Internal: allocate the next placeholder election id. */
void bb_alloc_id(bb_ctx *ctx, char out[BB_ID_LEN]);

/* Internal: the instance write lock, held across a ballot's read-check-write
 * so concurrent submissions cannot interleave (R1). */
void bb_write_lock(bb_ctx *ctx);
void bb_write_unlock(bb_ctx *ctx);

/* Human-readable state name (for logs and error messages). */
const char *bb_state_str(bb_state_t s);

/* ---- UC-1: instantiate ------------------------------------------------- */

/* Pure predicate: validate an election config. Returns BB_OK or a specific
 * BB_ERR_CONFIG_* code. No side effects. */
bb_result_t bb_validate_config(const bb_config_t *config);

/*
 * Validate, then persist a new election in DRAFT. Fills out_id. Persistence
 * goes through the DB seam.
 *
 * desired_id: NULL or "" auto-allocates (bb_alloc_id), same as before this
 * parameter existed. Given non-empty, that id is used instead - refused
 * with BB_ERR_CONFIG_ID_TAKEN if an election with that id already exists.
 */
bb_result_t bb_create_election(bb_ctx *ctx, const bb_config_t *config, const char *desired_id,
                               char out_id[BB_ID_LEN]);

/* ---- lifecycle --------------------------------------------------------- */

/* Pure: is `to` a legal successor of `from`? (DRAFT->OPEN->CLOSED->PUBLISHED) */
int bb_is_legal_transition(bb_state_t from, bb_state_t to);

/*
 * Transition an election to `to`. The current state is read back from the store
 * (never supplied by the caller, which would leave a check-then-act gap) and
 * the edge must be legal, else BB_ERR_ILLEGAL_TRANSITION and no write.
 */
bb_result_t bb_transition_state(bb_ctx *ctx, const char *election_id, bb_state_t to);

/* ---- eligibility / certs ---------------------------------------------- */

/* Pure: is cert_name on the election's eligible list? */
int bb_check_eligibility(const bb_election_t *election, const char *cert_name);

/* ---- UC-2: join -------------------------------------------------------- */

/*
 * Admit a voter to an election: election must exist and be OPEN, the cert must
 * verify, and it must be on the eligible list. On BB_OK the admitted voter's
 * election config is copied to `out` (may be NULL). Refusals are specific:
 * BB_ERR_NOT_FOUND, BB_ERR_CERT_INVALID / BB_ERR_CERT_EXPIRED,
 * BB_ERR_NOT_ELIGIBLE, BB_ERR_NOT_OPEN.
 *
 * On BB_OK, if out_has_ballot and/or out_ballot_version are non-NULL (either
 * may be NULL independently), they are filled from the store's
 * GET_PRIOR_BALLOT record for this cert - the same lookup bb_record_ballot
 * uses to pick cast vs. update - so a caller whose own session state does
 * not remember a prior vote (a new process, or a fresh JOIN mid-session)
 * can still route correctly instead of treating a returning voter as
 * casting for the first time.
 */
bb_result_t bb_join(bb_ctx *ctx, const char *election_id, const char *cert_name,
                    bb_election_t *out, int *out_has_ballot, int *out_ballot_version);

/* ---- UC-3 / UC-4: cast & update --------------------------------------- */

/*
 * Record a ballot (cast or update). Gates in order: election OPEN, cert still
 * eligible, payload decrypts, option in range, nonce unused. On success it
 * issues a receipt, appends the identity-free hash row, supersedes the voter's
 * prior version (UC-4) and consumes the nonce. Serialised by the instance write
 * lock, so concurrent voters cannot interleave (R1).
 */
bb_result_t bb_record_ballot(bb_ctx *ctx, const char *election_id, const bb_ballot_t *ballot,
                             bb_receipt_t *out);

/* ---- UC-5: results ----------------------------------------------------- */

/* Publish a CLOSED election's results (gated: requires CLOSED). */
bb_result_t bb_publish_results(bb_ctx *ctx, const char *election_id);

/*
 * Fetch the published view for an observer. Gated: the election must be
 * PUBLISHED (else BB_ERR_NOT_PUBLISHED) and the observer must be on its
 * eligible list (else BB_ERR_NOT_ELIGIBLE). Nothing is written to `out` unless
 * both gates pass.
 */
bb_result_t bb_get_results(bb_ctx *ctx, const char *election_id, const char *cert_name,
                           bb_results_t *out);

/*
 * Admin-channel variant: same PUBLISHED gate, no eligible-voter check. The
 * operator who ran an election is not necessarily on its own eligible list;
 * reachability over the local admin socket is the authority instead, same
 * as CREATE/OPEN/CLOSE/PUBLISH. Never reachable from the voter channel -
 * see ballotd/control_plane.c's class check.
 */
bb_result_t bb_get_results_admin(bb_ctx *ctx, const char *election_id, bb_results_t *out);

/* ---- UC-6: check your vote -------------------------------------------- */

/*
 * Look up a receipt hash in the live (non-superseded) set. BB_OK fills
 * `out` with the row; BB_ERR_NOT_FOUND is the dropped-ballot path;
 * BB_ERR_NOT_FOUND also covers an unknown election id.
 *
 * Deliberately not gated on election state (works before close/publish
 * too): the hash is a secret only its own holder has, and FIND_HASH's row
 * carries no identity, so this reveals nothing about any other ballot
 * regardless of when it runs - unlike bb_get_results, which stays
 * PUBLISHED-only because it hands back an aggregate. Reachable from both
 * BCL_CHECK (voter channel) and BCL_ADMIN_CHECK (admin channel, since
 * ballotctl has no voter session to route CHECK through at all) - same
 * function either way; only the transport differs.
 *
 * option_name (optional, may be NULL) is filled with out->option_index's
 * display text - the option list is public (shown at JOIN), so returning it
 * here reveals nothing beyond what `out` already does.
 */
bb_result_t bb_lookup_hash(bb_ctx *ctx, const char *election_id, const char *hash,
                           bb_ballot_hash_t *out, char option_name[BB_OPTION_LEN]);

#endif /* BALLOTBRAIN_H */
