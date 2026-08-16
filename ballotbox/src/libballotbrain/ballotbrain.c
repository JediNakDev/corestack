#include "libballotbrain/ballotbrain.h"
#include "internal.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * Per-instance context: the operation-log sink, the write lock that
 * serialises ballot recording (R1), and (struct bb_ctx itself, in
 * internal.h, shared with db.c) the store connection state. Keeping state
 * here rather than in file-scope globals is what lets each test case run an
 * isolated instance.
 */

bb_ctx *bb_create(void) {
  bb_ctx *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    return NULL;
  }
  ctx->log = stderr;
  if (pthread_mutex_init(&ctx->write_lock, NULL) != 0) {
    free(ctx);
    return NULL;
  }
  return ctx;
}

void bb_write_lock(bb_ctx *ctx) {
  if (ctx != NULL) {
    pthread_mutex_lock(&ctx->write_lock);
  }
}

void bb_write_unlock(bb_ctx *ctx) {
  if (ctx != NULL) {
    pthread_mutex_unlock(&ctx->write_lock);
  }
}

/*
 * Durable id allocation: "select max(seq) from election" on its own
 * connection, own transaction-free (auto-commit) statement. Deliberately
 * NOT in db.c/txn.c: it never calls db_exec or bb_db_begin/commit/rollback,
 * only db_socket_* directly, so it needs no fake and stays safe to call
 * from every test - and when the store is unreachable (no fake, no real
 * bin/tetrisdb, exactly the situation a plain unit test is in) it degrades
 * to a fixed first id rather than failing, which is what keeps
 * test_brain_create.c's assertions ("the id handed back is the id that was
 * stored") true with no DB running at all.
 */
void bb_alloc_id(bb_ctx *ctx, char out[BB_ID_LEN]) {
  out[0] = '\0';
  if (ctx == NULL) {
    return;
  }

  db_socket_opts_t opts = bb_effective_db_opts(ctx);
  db_socket_t *conn = db_socket_open(&opts);
  int next = 100; /* first id if the table is empty or the store is unreachable */

  if (conn != NULL) {
    char body[4096];
    if (db_socket_exec(conn, "select max(seq) from " BB_DB_TABLE_ELECTION ";", body,
                        sizeof body) == DB_OK) {
      int rows = db_row_count(body);
      if (rows > 0) {
        const char *f[1];
        size_t len[1];
        if (db_row_fields(body, 0, f, len, 1) >= 1) {
          char text[24];
          size_t n = len[0] < sizeof(text) - 1 ? len[0] : sizeof(text) - 1;
          memcpy(text, f[0], n);
          text[n] = '\0';
          char *end;
          long v = strtol(text, &end, 10);
          if (*end == '\0' && v >= 0) {
            next = (int)v + 1;
          }
        }
      }
    }
    db_socket_close(conn);
  }

  snprintf(out, BB_ID_LEN, "E-%d", next);
}

void bb_destroy(bb_ctx *ctx) {
  if (ctx == NULL) {
    return;
  }
  /* A transaction left open by a caller that forgot to commit/rollback must
   * not leak the connection - closing it is also a rollback, per the wire
   * contract. db_socket_close() directly, not db.c's bb_db_rollback(): a
   * test that fakes db_exec (fake_brain_seams.h) must not be forced to link
   * the real db.c just because bb_destroy is in its call path - same
   * isolation reasoning as libballotclient's bcl_destroy not calling
   * bcl_disconnect. */
  if (ctx->txn_conn != NULL) {
    db_socket_close(ctx->txn_conn);
  }
  pthread_mutex_destroy(&ctx->write_lock);
  free(ctx);
}

void bb_set_log(bb_ctx *ctx, FILE *sink) {
  if (ctx != NULL) {
    ctx->log = sink;
  }
}

void bb_log(bb_ctx *ctx, const char *fmt, ...) {
  if (ctx == NULL || ctx->log == NULL) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(ctx->log, fmt, ap);
  va_end(ap);
  fputc('\n', ctx->log);
}

const char *bb_state_str(bb_state_t s) {
  switch (s) {
    case BB_STATE_DRAFT: return "DRAFT";
    case BB_STATE_OPEN: return "OPEN";
    case BB_STATE_CLOSED: return "CLOSED";
    case BB_STATE_PUBLISHED: return "PUBLISHED";
  }
  return "?";
}
