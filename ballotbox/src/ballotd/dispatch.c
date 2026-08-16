/*
 * dispatch.c - see dispatch.h. Pure routing: turns a decoded bcl_request_t
 * into arguments for exactly one libballotbrain call, and that call's
 * out-params into a bcl_response_t. No domain logic lives here - every rule
 * this switch could possibly need already lives in the libballotbrain
 * sources, untouched.
 */

#include "ballotd/dispatch.h"

#include <string.h>

bb_result_t ballotd_dispatch(bb_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp) {
  memset(resp, 0, sizeof *resp);

  switch (req->op) {
    case BCL_CREATE: {
      char id[BB_ID_LEN];
      memset(id, 0, sizeof id);
      resp->status = bb_create_election(ctx, &req->config, req->election_id, id);
      if (resp->status == BB_OK) snprintf(resp->election.id, BB_ID_LEN, "%s", id);
      return resp->status;
    }

    /* Admin channel only (see control_plane.c's class check): a read-only
     * peek at what CREATE would auto-allocate if the client submits with
     * election_id left blank, so ballotctl can pre-fill and let the
     * operator edit it before actually creating anything. bb_alloc_id has
     * no side effect (a plain select max(seq)), so peeking costs nothing
     * and reserves nothing - a later CREATE can still race another one
     * server-side no differently than it always could. */
    case BCL_ADMIN_NEXT_ID: {
      char id[BB_ID_LEN];
      memset(id, 0, sizeof id);
      bb_alloc_id(ctx, id);
      snprintf(resp->election.id, BB_ID_LEN, "%s", id);
      resp->status = BB_OK;
      return resp->status;
    }

    case BCL_OPEN:
      resp->status = bb_transition_state(ctx, req->election_id, BB_STATE_OPEN);
      return resp->status;

    case BCL_CLOSE:
      resp->status = bb_transition_state(ctx, req->election_id, BB_STATE_CLOSED);
      return resp->status;

    case BCL_PUBLISH:
      resp->status = bb_publish_results(ctx, req->election_id);
      return resp->status;

    case BCL_JOIN:
      resp->status = bb_join(ctx, req->election_id, req->cert_name, &resp->election,
                             &resp->has_prior_ballot, &resp->prior_ballot_version);
      return resp->status;

    case BCL_CAST:
    case BCL_UPDATE:
      resp->status = bb_record_ballot(ctx, req->election_id, &req->ballot, &resp->receipt);
      return resp->status;

    case BCL_RESULTS: {
      bb_results_t results;
      memset(&results, 0, sizeof results);
      resp->status = bb_get_results(ctx, req->election_id, req->cert_name, &results);
      if (resp->status == BB_OK) {
        snprintf(resp->election.id, BB_ID_LEN, "%s", req->election_id);
        snprintf(resp->election.title, BB_TITLE_LEN, "%s", results.title);
        resp->option_count = results.option_count;
        memcpy(resp->tally, results.tally, sizeof resp->tally);
        memcpy(resp->options, results.options, sizeof resp->options);
        resp->hash_count = results.hash_count;
        memcpy(resp->hashes, results.hashes, sizeof resp->hashes);
      }
      return resp->status;
    }

    /* Admin channel only (see control_plane.c's class check): same read,
     * no eligible-list check - the operator need not be their own
     * election's eligible voter. */
    case BCL_ADMIN_RESULTS: {
      bb_results_t results;
      memset(&results, 0, sizeof results);
      resp->status = bb_get_results_admin(ctx, req->election_id, &results);
      if (resp->status == BB_OK) {
        snprintf(resp->election.id, BB_ID_LEN, "%s", req->election_id);
        snprintf(resp->election.title, BB_TITLE_LEN, "%s", results.title);
        resp->option_count = results.option_count;
        memcpy(resp->tally, results.tally, sizeof resp->tally);
        memcpy(resp->options, results.options, sizeof resp->options);
        resp->hash_count = results.hash_count;
        memcpy(resp->hashes, results.hashes, sizeof resp->hashes);
      }
      return resp->status;
    }

    /* CHECK (voter channel) and ADMIN_CHECK (admin channel, since ballotctl
     * has no voter session to route CHECK through at all) both resolve to
     * the identical read - bb_lookup_hash is ungated on election state, so
     * there is no PUBLISHED-vs-not distinction left between them, only
     * which transport a caller reaches ballotd on. */
    case BCL_CHECK:
    case BCL_ADMIN_CHECK: {
      bb_ballot_hash_t row;
      memset(&row, 0, sizeof row);
      resp->status = bb_lookup_hash(ctx, req->election_id, req->hash, &row, resp->found_option_name);
      resp->found = (resp->status == BB_OK) ? 1 : 0;
      if (resp->found) resp->found_option = row.option_index;
      return resp->status;
    }

    default:
      /* Unreachable in practice: both intake channels reject any op outside
       * their own set before a request ever reaches here (see session.c /
       * control_plane.c). Kept as a defensive fallback, not a real path. */
      resp->status = BB_ERR_NOT_IMPLEMENTED;
      return resp->status;
  }
}
