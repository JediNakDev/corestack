#include "libballotbrain/db.h"
#include "libballotbrain/ballotbrain.h"
#include "internal.h"

/*
 * Transaction control - see db.h's contract comment and internal.h's file
 * comment for why this is its own translation unit, separate from db.c's
 * db_exec: a test that fakes db_exec to unit-test bb_record_ballot also
 * needs to fake these (fake_brain_seams.h does, as trivial always-succeed
 * no-ops), and the two fakes must be independently substitutable.
 */

static bb_result_t map_status(db_status_t st) {
  switch (st) {
    case DB_OK: return BB_OK;
    case DB_RETRY: return BB_ERR_RETRY;
    default: return BB_ERR_DB;
  }
}

bb_result_t bb_db_begin(bb_ctx *ctx) {
  if (ctx == NULL || ctx->txn_conn != NULL) {
    return BB_ERR_DB;
  }
  db_socket_opts_t opts = bb_effective_db_opts(ctx);
  db_socket_t *conn = db_socket_open(&opts);
  if (conn == NULL) {
    return BB_ERR_DB;
  }
  bb_result_t r = map_status(db_socket_exec(conn, "set transaction read write;", NULL, 0));
  if (r != BB_OK) {
    db_socket_close(conn);
    return r;
  }
  ctx->txn_conn = conn;
  return BB_OK;
}

bb_result_t bb_db_commit(bb_ctx *ctx) {
  if (ctx == NULL || ctx->txn_conn == NULL) {
    return BB_ERR_DB;
  }
  bb_result_t r = map_status(db_socket_exec(ctx->txn_conn, "commit;", NULL, 0));
  db_socket_close(ctx->txn_conn);
  ctx->txn_conn = NULL;
  return r;
}

void bb_db_rollback(bb_ctx *ctx) {
  if (ctx == NULL || ctx->txn_conn == NULL) {
    return;
  }
  /* Intent, not a requirement: closing rolls back anything left open
   * regardless (db/docs/c-daemon-integration.md section 4, "Shutdown"). */
  (void)db_socket_exec(ctx->txn_conn, "rollback;", NULL, 0);
  db_socket_close(ctx->txn_conn);
  ctx->txn_conn = NULL;
}
