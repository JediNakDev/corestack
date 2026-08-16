#include "libballotclient/voter.h"
#include "internal.h"

#include <string.h>

/*
 * Voter-side logic. The pure decision functions below hold the UC-2/3/4/6
 * client rules; the session flows underneath them reach the daemon only through
 * the transport seam (bcl_send) and the client crypto seam, so each is
 * exercisable on its own.
 */

/* ---- decision logic (pure) -------------------------------------------- */

bu_vote_action_t bu_route_vote(const bu_session_t *session) {
  if (session == NULL || !session->joined) {
    return BU_MUST_JOIN; /* rule 1 */
  }
  if (!session->has_ballot) {
    return BU_CAST; /* rule 3 */
  }
  return BU_UPDATE; /* rule 5 */
}

bu_join_outcome_t bu_classify_join(bb_result_t status, const bb_election_t *election) {
  switch (status) {
    case BB_ERR_NOT_FOUND:
      return BU_JOIN_NOT_FOUND;
    case BB_ERR_NOT_ELIGIBLE:
    case BB_ERR_CERT_INVALID:
    case BB_ERR_CERT_EXPIRED:
      return BU_JOIN_NOT_ELIGIBLE;
    case BB_ERR_NOT_OPEN:
      return BU_JOIN_NOT_OPEN;
    case BB_OK:
      /* Admitted only if the daemon returned an OPEN election; any other state
       * means "saved for later" per UC-2 alternative flow 4a. */
      if (election != NULL && election->state == BB_STATE_OPEN) {
        return BU_JOIN_ADMITTED;
      }
      return BU_JOIN_NOT_OPEN;
    default:
      /* Transport-level failure (unreachable host, not-implemented stub). */
      return BU_JOIN_TIMEOUT;
  }
}

bu_check_outcome_t bu_classify_check(bb_result_t status, int found) {
  if (status == BB_OK) {
    return found ? BU_CHECK_COUNTED : BU_CHECK_DROPPED;
  }
  /* An explicit "no such hash" from the daemon is the dropped-ballot path
   * (UC-6 alt flow 4a); anything else means the answer never arrived, which
   * must not be shown to the voter as a lost ballot. */
  if (status == BB_ERR_NOT_FOUND) {
    return BU_CHECK_DROPPED;
  }
  return BU_CHECK_UNAVAILABLE;
}

/* ---- session flows ----------------------------------------------------- */

void bu_set_before_submit(bcl_ctx *ctx, bu_before_submit_fn hook, void *arg) {
  if (ctx == NULL) return;
  ctx->before_submit = hook;
  ctx->before_submit_arg = arg;
}

bu_join_outcome_t bu_join(bcl_ctx *ctx, bu_session_t *session, const char *election_id,
                          const char *cert_name) {
  if (session == NULL || election_id == NULL || cert_name == NULL) {
    return BU_JOIN_NOT_FOUND;
  }

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_JOIN;
  snprintf(req.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(req.cert_name, BB_CERT_LEN, "%s", cert_name);

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);
  /* A transport failure outranks whatever is left in the response buffer. */
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;

  /* election_id/cert_name may alias fields inside *session (ballotu.c passes
   * g_session.cert_name back in as the argument) - copied out before any
   * memset(session, ...) below, or a successful join would zero its own
   * source string before the snprintf that is meant to restore it. */
  char election_id_copy[BB_ID_LEN];
  char cert_name_copy[BB_CERT_LEN];
  snprintf(election_id_copy, sizeof election_id_copy, "%s", election_id);
  snprintf(cert_name_copy, sizeof cert_name_copy, "%s", cert_name);

  bu_join_outcome_t outcome = bu_classify_join(status, &resp.election);
  switch (outcome) {
    case BU_JOIN_ADMITTED:
      memset(session, 0, sizeof(*session));
      session->joined = 1;
      snprintf(session->cert_name, BB_CERT_LEN, "%s", cert_name_copy);
      snprintf(session->election_id, BB_ID_LEN, "%s", election_id_copy);
      snprintf(session->title, BB_TITLE_LEN, "%s", resp.election.title);
      session->option_count = resp.election.option_count;
      memcpy(session->options, resp.election.options, sizeof(session->options));
      /* Without this, has_ballot/ballot_version stayed at the memset's zero
       * regardless of the server's own record - a rejoin (new process, or
       * Join picked again mid-session) always looked like a first-time
       * voter, silently routing UC-3 (cast) over an existing ballot instead
       * of UC-4 (update). The server now reports this from the same
       * GET_PRIOR_BALLOT record bu_route_vote's decision depends on. */
      session->has_ballot = resp.has_prior_ballot;
      session->ballot_version = resp.prior_ballot_version;
      break;
    case BU_JOIN_NOT_OPEN:
      /* UC-2 alt flow 4a: the election is real, so it is remembered locally for
       * later, but the voter is not joined to it. */
      session->joined = 0;
      snprintf(session->election_id, BB_ID_LEN, "%s", election_id_copy);
      break;
    default:
      /* Timeout / not found / not eligible: no session state is created. */
      break;
  }
  return outcome;
}

bb_result_t bu_submit_vote(bcl_ctx *ctx, bu_session_t *session, int option_index,
                           const char *nonce, bb_receipt_t *out) {
  if (session == NULL || nonce == NULL) {
    return BB_ERR_NOT_JOINED;
  }

  const bu_vote_action_t action = bu_route_vote(session);
  if (action == BU_MUST_JOIN) {
    /* Rule 1: nothing goes on the wire before the voter has joined. */
    return BB_ERR_NOT_JOINED;
  }

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = (action == BU_UPDATE) ? BCL_UPDATE : BCL_CAST;
  snprintf(req.election_id, BB_ID_LEN, "%s", session->election_id);
  snprintf(req.cert_name, BB_CERT_LEN, "%s", session->cert_name);

  bb_result_t er = bu_encrypt_ballot(ctx, option_index, nonce, &req.ballot);
  if (er != BB_OK) {
    return er;
  }
  snprintf(req.ballot.cert_name, BB_CERT_LEN, "%s", session->cert_name);

  if (ctx != NULL && ctx->before_submit != NULL) {
    ctx->before_submit(ctx->before_submit_arg);
  }

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;
  if (status != BB_OK) {
    /* A refused or lost submission must not move local session state. */
    return status;
  }

  session->has_ballot = 1;
  session->ballot_version++;
  snprintf(session->my_hash, BB_HASH_LEN, "%s", resp.receipt.hash);
  if (out != NULL) {
    *out = resp.receipt;
  }
  return BB_OK;
}
