#ifndef BALLOTCLIENT_ADMIN_H
#define BALLOTCLIENT_ADMIN_H

/*
 * Admin-side logic for ballotctl (UC-1 create/open, close, publish). Assembles
 * requests and pre-validates config client-side before it hits the daemon.
 */

#include "libballotclient/client.h"

/* Pure client-side pre-validation of config, so the admin gets immediate
 * feedback without a round trip. Delegates to the brain's authoritative
 * bb_validate_config, which the daemon also enforces. */
bb_result_t bc_prevalidate_config(const bb_config_t *config);

/* Build a CREATE request from a validated config. Returns the pre-validation
 * result; on BB_OK, `out` is a ready-to-send request. */
bb_result_t bc_build_create(const bb_config_t *config, bcl_request_t *out);

/* Build an OPEN/CLOSE/PUBLISH lifecycle request for an election. `op` must be
 * one of BCL_OPEN, BCL_CLOSE, BCL_PUBLISH. */
bb_result_t bc_build_transition(bcl_op_t op, const char *election_id, bcl_request_t *out);

/*
 * Fold *count entries of `out` (a CREATE config's eligible-voter list) the
 * same way libtetrisauth folds every real username (user_name_fold), then
 * deduplicate case-insensitively, keeping first-occurrence order and
 * shrinking *count to match. So "Alice, alice, ALICE" becomes one "alice" -
 * matching what the server's cert_name will actually be for that voter
 * (bb_check_eligibility is a byte-exact strcmp), and not wasting the
 * BB_MAX_VOTERS cap on repeats of the same voter.
 *
 * Returns 0 on success (out and *count updated in place), or -1 on the
 * first entry that is not a legal player name - `bad_entry` (may be NULL)
 * then receives that entry verbatim, `bad_cap` is its capacity, and out
 * and *count are left untouched.
 */
int bc_fold_eligible(char out[][BB_CERT_LEN], int *count, char *bad_entry, size_t bad_cap);

#endif /* BALLOTCLIENT_ADMIN_H */
