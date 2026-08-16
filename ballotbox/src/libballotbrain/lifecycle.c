#include "libballotbrain/ballotbrain.h"

#include <string.h>

/*
 * Election lifecycle. The legal chain is strictly:
 *   DRAFT -> OPEN -> CLOSED -> PUBLISHED
 * No skips, no reversals. bb_is_legal_transition is a pure table, used to guard
 * transitions here and exercised directly by the transition-table tests.
 */
int bb_is_legal_transition(bb_state_t from, bb_state_t to) {
  switch (from) {
    case BB_STATE_DRAFT: return to == BB_STATE_OPEN;
    case BB_STATE_OPEN: return to == BB_STATE_CLOSED;
    case BB_STATE_CLOSED: return to == BB_STATE_PUBLISHED;
    case BB_STATE_PUBLISHED: return 0;
  }
  return 0;
}

bb_result_t bb_transition_state(bb_ctx *ctx, const char *election_id, bb_state_t to) {
  if (election_id == NULL) {
    return BB_ERR_NOT_FOUND;
  }

  /* The current state is read from the store, never taken from the caller:
   * deciding legality against a stale `from` would leave a check-then-act gap
   * where two admins could both drive the same election forward. */
  bb_db_cmd_t get = {0};
  get.op = BB_DB_GET_ELECTION;
  snprintf(get.election_id, BB_ID_LEN, "%s", election_id);

  bb_db_result_t res;
  memset(&res, 0, sizeof(res));
  bb_result_t gr = db_exec(ctx, &get, &res);
  if (gr != BB_OK) {
    return gr;
  }
  if (!res.found) {
    return BB_ERR_NOT_FOUND;
  }

  const bb_state_t from = res.election.state;
  if (!bb_is_legal_transition(from, to)) {
    bb_log(ctx, "[lifecycle] reject %s -> %s on '%s'", bb_state_str(from), bb_state_str(to),
           election_id);
    return BB_ERR_ILLEGAL_TRANSITION;
  }

  bb_db_cmd_t upd = {0};
  upd.op = BB_DB_UPDATE_STATE;
  snprintf(upd.election_id, BB_ID_LEN, "%s", election_id);
  upd.new_state = to;
  return db_exec(ctx, &upd, NULL);
}
