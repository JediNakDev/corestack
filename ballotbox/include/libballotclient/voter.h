#ifndef BALLOTCLIENT_VOTER_H
#define BALLOTCLIENT_VOTER_H

/*
 * Voter-side logic for ballotu (UC-2/3/4/6). Pure decision logic plus the
 * client crypto seam (ballot encryption, receipt-key derivation), which is a
 * deterministic placeholder until keys arrive.
 */

#include "libballotclient/client.h"

/* The voter's local session (mirrors VoterSession in the class diagram). This
 * is client-local state, distinct from the daemon's authoritative store. */
typedef struct {
  char cert_name[BB_CERT_LEN];
  int joined;                 /* 1 once admitted to an election */
  char election_id[BB_ID_LEN];
  int has_ballot;             /* 1 once a ballot has been cast */
  int ballot_version;
  char my_hash[BB_HASH_LEN];  /* latest receipt hash */

  /* Remembered from the JOIN response so a caller can render a ballot menu
   * without a second round trip - bu_join is the only place that ever sees
   * bb_election_t.options, and it would otherwise discard them. */
  char title[BB_TITLE_LEN];
  char options[BB_MAX_OPTIONS][BB_OPTION_LEN];
  int option_count;
} bu_session_t;

/* Optional instance-scoped synchronization point invoked after ballot
 * encryption and immediately before the request is sent. Production clients
 * leave it unset. System tests use it to put an election close precisely in
 * the client-encrypted/server-not-yet-recorded window. */
typedef void (*bu_before_submit_fn)(void *arg);
void bu_set_before_submit(bcl_ctx *ctx, bu_before_submit_fn hook, void *arg);

/* Which flow the `vote` command should take (decision table, client-side rules
 * 1/3/5). The election-open check is the daemon's job and is not decided here. */
typedef enum {
  BU_MUST_JOIN,  /* rule 1: not joined */
  BU_CAST,       /* rule 3: joined, no prior ballot */
  BU_UPDATE      /* rule 5: joined, prior ballot exists */
} bu_vote_action_t;

/* Pure: route the vote command from local session state. */
bu_vote_action_t bu_route_vote(const bu_session_t *session);

/* How a join attempt resolved (UC-2 partitions). */
typedef enum {
  BU_JOIN_TIMEOUT,      /* host unreachable */
  BU_JOIN_NOT_FOUND,    /* election id unknown */
  BU_JOIN_NOT_ELIGIBLE, /* cert not on the eligible list */
  BU_JOIN_NOT_OPEN,     /* election exists but not OPEN (saved for later) */
  BU_JOIN_ADMITTED      /* eligible and OPEN */
} bu_join_outcome_t;

/* Pure: classify a join response into a UC-2 outcome. `election` may be NULL
 * for transport-level failures (timeout / not found). */
bu_join_outcome_t bu_classify_join(bb_result_t status, const bb_election_t *election);

/* How a check-your-vote lookup resolved (UC-6). */
typedef enum {
  BU_CHECK_COUNTED,    /* hash found in the published tally */
  BU_CHECK_DROPPED,    /* hash absent: dropped ballot, escalate to the admin */
  BU_CHECK_UNAVAILABLE /* the lookup itself failed (transport, not published) */
} bu_check_outcome_t;

/* Pure: classify a check response. `found` is only meaningful when the daemon
 * answered BB_OK; a failed lookup is never reported as a dropped ballot. */
bu_check_outcome_t bu_classify_check(bb_result_t status, int found);

/* ---- session flows (drive the transport seam) -------------------------- */

/*
 * UC-2: join an election. Sends the JOIN request, classifies the reply, and
 * updates `session` accordingly: admitted starts a fresh session on that
 * election; a not-open election is still recorded locally (UC-2 alt flow 4a);
 * every other outcome leaves the session untouched.
 */
bu_join_outcome_t bu_join(bcl_ctx *ctx, bu_session_t *session, const char *election_id,
                          const char *cert_name);

/*
 * UC-3/UC-4: submit a vote. Routes on local session state (BB_ERR_NOT_JOINED
 * and nothing sent when the voter has not joined), encrypts the selection,
 * sends CAST or UPDATE, and on success records the receipt in the session.
 * `out` may be NULL.
 */
bb_result_t bu_submit_vote(bcl_ctx *ctx, bu_session_t *session, int option_index,
                           const char *nonce, bb_receipt_t *out);

/* ---- crypto seam (placeholder) ---------------------------------------- */

/* Encrypt a ballot selection with a fresh nonce. Placeholder encodes the option
 * in payload[0] to round-trip with the daemon's decrypt stub; real RSA-OAEP
 * with the election public key lands with keys. */
bb_result_t bu_encrypt_ballot(bcl_ctx *ctx, int option_index, const char *nonce, bb_ballot_t *out);

/* Derive the receipt hash from the voter's secret ballot key (UC-6 KDF).
 * Deterministic placeholder; real SHA-256/HKDF lands with the crypto layer. */
bb_result_t bu_derive_receipt(bcl_ctx *ctx, const char *secret_key, char out[BB_HASH_LEN]);

#endif /* BALLOTCLIENT_VOTER_H */
