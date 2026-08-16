#ifndef LIBBALLOTBRAIN_INTERNAL_H
#define LIBBALLOTBRAIN_INTERNAL_H

/*
 * internal.h - bb_ctx's real layout, shared by ballotbrain.c (log,
 * lifecycle, the write lock, bb_alloc_id), db.c (db_exec) and txn.c
 * (bb_db_begin/commit/rollback). Same seam-isolation reasoning as
 * libballotclient/internal.h: callers of the public API never need to know
 * a db_socket_t exists.
 *
 * db_exec and the transaction control functions are DELIBERATELY two
 * separate translation units (db.c, txn.c), even though both need
 * libtetrisdb - a test that fakes db_exec but exercises bb_record_ballot
 * also needs to fake bb_db_begin/commit/rollback (fake_brain_seams.h does),
 * and faking one must not force the other's real member into the archive
 * pull. bb_alloc_id lives in ballotbrain.c instead, on its own: it never
 * calls db_exec or a transaction function, only db_socket_* directly, and
 * degrades to a fixed id when unreachable - so it needs no fake at all, and
 * ballotbrain.o is already linked into every test regardless (bb_create/
 * bb_destroy).
 */

#include "libtetrisdb/socket/db.h"

#include <pthread.h>
#include <stdio.h>

struct bb_ctx {
  FILE *log;
  pthread_mutex_t write_lock;

  db_socket_opts_t db_opts;
  int db_opts_set; /* 0 -> falls back to db_socket_opts_load() */

  /* Non-NULL between bb_db_begin() and bb_db_commit()/bb_db_rollback():
   * every db_exec() call in that window reuses this connection instead of
   * opening its own. See db.h's contract comment. */
  db_socket_t *txn_conn;
};

/* Shared by ballotbrain.c/db.c/txn.c so the three do not each carry their
 * own copy. static inline, not a symbol: including it costs nothing at link
 * time, so it cannot reintroduce the isolation problem the file comment
 * above describes. */
static inline db_socket_opts_t bb_effective_db_opts(struct bb_ctx *ctx) {
  db_socket_opts_t opts;
  if (ctx->db_opts_set) {
    opts = ctx->db_opts;
  } else {
    db_socket_opts_load(&opts);
  }
  return opts;
}

#endif /* LIBBALLOTBRAIN_INTERNAL_H */
