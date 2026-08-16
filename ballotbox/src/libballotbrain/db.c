#include "libballotbrain/db.h"
#include "libballotbrain/ballotbrain.h"
#include "internal.h"

#include "libtetrisdb/schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * DB seam - real implementation, against libtetrisdb/socket/db.h's
 * synchronous SocketRunner client. See db.h for the transaction contract
 * (self-owned vs. caller-managed via bb_db_begin/commit/rollback).
 *
 * Schema. SimpleDB has no arrays and no UPDATE (delete then insert), which
 * shapes every table here:
 *
 *   election(seq int, id string, title string, state string,
 *            open_time string, close_time string)
 *     One row per election. `seq` is allocation-only (bb_alloc_id's
 *     "select max(seq)") - `id` ("E-<seq>") is the key everything else
 *     uses. A state change is delete-then-reinsert with every other column
 *     carried over unchanged.
 *
 *   election_option(election_id string, idx int, name string)
 *   election_eligible(election_id string, cert_name string)
 *     The two arrays bb_config_t/bb_election_t carry, one row each - no
 *     delimiter-in-a-string encoding, so no escaping question to get wrong.
 *
 *   ballot(election_id string, hash string, option_index int, version int,
 *          superseded int)
 *     The public, identity-free row bb_ballot_hash_t mirrors (R2). Every
 *     version of every ballot stays here; `superseded` is the only field
 *     that ever changes (delete + reinsert, from BB_DB_MARK_SUPERSEDED).
 *
 *   ballot_owner(election_id string, cert_name string, hash string,
 *                version int)
 *     THE PRIVATE MAPPING GET_PRIOR_BALLOT NEEDS - this table did not exist
 *     before this seam was real, which is exactly the gap the handoff
 *     flagged ("the private voter-to-version mapping is missing from the
 *     write command"). One row per (election, voter), replaced whole on
 *     every cast/update (BB_DB_SET_OWNER). Never joined with `ballot` in a
 *     way that would leave a voter-to-hash link in a log line - see the
 *     secrecy note on BB_DB_SET_OWNER/BB_DB_GET_PRIOR_BALLOT below.
 *
 *   nonce(election_id string, nonce string)
 *     Consumed nonces (anti-replay).
 *
 * Column-safety: SimpleDB's select reply is tab-separated and unquoted, so a
 * stored value containing TAB/CR/LF would shift every field after it for
 * every row printed alongside it (the same hazard TETRISAUTH_DB_SCHEMA's own
 * comment names for its username column). unsafe() rejects any such field
 * before it reaches a statement, as a seam-level backstop independent of
 * whatever the validation layer above does today.
 */

#define BODY_MAX 16384
/* Comfortably larger than six QUOTE_MAX fields plus literal text - true
 * field lengths (BB_TITLE_LEN etc.) are all far under QUOTE_MAX, but sizing
 * against QUOTE_MAX itself is what lets the compiler prove no snprintf here
 * can truncate, rather than merely trusting it won't in practice. */
#define SQL_MAX 4096
#define QUOTE_MAX 512

#define SELF_RETRY_ATTEMPTS 3
#define SELF_RETRY_BACKOFF_US 5000

/* ---- small helpers ------------------------------------------------------ */

static bb_result_t map_status(db_status_t st) {
  switch (st) {
    case DB_OK: return BB_OK;
    case DB_RETRY: return BB_ERR_RETRY;
    default: return BB_ERR_DB;
  }
}

static bb_result_t exec_stmt(db_socket_t *conn, const char *sql, char *body, size_t cap) {
  return map_status(db_socket_exec(conn, sql, body, cap));
}

/* TAB/CR/LF would desynchronise every reply printed after this field - see
 * the file comment. Applied to every user-supplied string before it reaches
 * a statement. */
static int unsafe(const char *s) {
  for (; *s; s++) {
    if (*s == '\t' || *s == '\n' || *s == '\r') return 1;
  }
  return 0;
}

/* Bounded copy from a db_row_fields() slice (not NUL-terminated) into a
 * fixed buffer. */
static void field_copy(const char *f, size_t len, char *dst, size_t cap) {
  size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(dst, f, n);
  dst[n] = '\0';
}

static int field_int(const char *f, size_t len) {
  char text[24];
  field_copy(f, len, text, sizeof text);
  return atoi(text);
}

static int parse_state(const char *text, bb_state_t *out) {
  if (strcmp(text, "DRAFT") == 0) { *out = BB_STATE_DRAFT; return 0; }
  if (strcmp(text, "OPEN") == 0) { *out = BB_STATE_OPEN; return 0; }
  if (strcmp(text, "CLOSED") == 0) { *out = BB_STATE_CLOSED; return 0; }
  if (strcmp(text, "PUBLISHED") == 0) { *out = BB_STATE_PUBLISHED; return 0; }
  return -1;
}

/* "E-100" -> 100. Only ever applied to ids this file generated itself
 * (bb_alloc_id), so a parse failure (0) can only mean a caller passed a
 * hand-built id - the insert still proceeds, just re-using seq 0. */
static int id_seq(const char *id) {
  const char *dash = strrchr(id, '-');
  if (dash == NULL) return 0;
  char *end;
  long v = strtol(dash + 1, &end, 10);
  if (*end != '\0' || v < 0) return 0;
  return (int)v;
}

void bb_set_db_opts(bb_ctx *ctx, const db_socket_opts_t *opts) {
  if (ctx == NULL || opts == NULL) {
    return;
  }
  ctx->db_opts = *opts;
  ctx->db_opts_set = 1;
}

/* bb_alloc_id lives in ballotbrain.c and bb_db_begin/commit/rollback in
 * txn.c - not here - so that a test faking db_exec never has to also fake
 * them, and vice versa. See internal.h's file comment. */

/* ---- op dispatch, given an already-open connection ------------------------ */

static int op_is_write(bb_db_op_t op) {
  switch (op) {
    case BB_DB_INSERT_ELECTION:
    case BB_DB_UPDATE_STATE:
    case BB_DB_APPEND_BALLOT:
    case BB_DB_MARK_SUPERSEDED:
    case BB_DB_NONCE_MARK:
    case BB_DB_SET_OWNER:
      return 1;
    default:
      return 0;
  }
}

/*
 * Describes the command, unconditionally, before any connection is
 * attempted - so the log states intent even when the store is unreachable,
 * matching the old stub's observable behaviour (and what test_brain_secrecy
 * relies on: a hash must appear in the log for the absence of a cert to mean
 * anything). Called once, from db_exec, not from each run_* handler - those
 * only run after a connection succeeds, which the log must not depend on.
 *
 * Secrecy (R2 / U-21): the cert never appears rendered for APPEND_BALLOT or
 * SET_OWNER, and GET_PRIOR_BALLOT's binds it as "cert=?" instead - the same
 * discipline the old stub had, preserved verbatim here.
 */
static void log_cmd(bb_ctx *ctx, const bb_db_cmd_t *cmd) {
  switch (cmd->op) {
    case BB_DB_INSERT_ELECTION:
      bb_log(ctx, "[db] insert into election(id, title, state) values ('%s', ?, 'DRAFT')",
            cmd->election_id);
      return;
    case BB_DB_GET_ELECTION:
      bb_log(ctx, "[db] select * from election where id = '%s'", cmd->election_id);
      return;
    case BB_DB_UPDATE_STATE:
      bb_log(ctx, "[db] election '%s': state -> '%s' (delete + reinsert, no UPDATE in SimpleDB)",
            cmd->election_id, bb_state_str(cmd->new_state));
      return;
    case BB_DB_APPEND_BALLOT:
      /* hash + option only - never the submitting cert (R2, U-21). */
      bb_log(ctx,
            "[db] insert into ballot(election_id, hash, option_index, version, superseded) "
            "values ('%s', '%s', %d, %d, %d)",
            cmd->election_id, cmd->hash_row ? cmd->hash_row->hash : "?",
            cmd->hash_row ? cmd->hash_row->option_index : -1,
            cmd->hash_row ? cmd->hash_row->version : -1,
            cmd->hash_row ? cmd->hash_row->superseded : 0);
      return;
    case BB_DB_MARK_SUPERSEDED:
      bb_log(ctx, "[db] ballot '%s' in '%s': superseded -> 1 (delete + reinsert)", cmd->hash,
            cmd->election_id);
      return;
    case BB_DB_NONCE_MARK:
      bb_log(ctx, "[db] insert into nonce(election_id, nonce) values ('%s', '%s')",
            cmd->election_id, cmd->nonce);
      return;
    case BB_DB_SET_OWNER:
      /* The one write that carries an identity. Logged like GET_PRIOR_BALLOT:
       * the cert bound as a parameter, never rendered (R2). */
      bb_log(ctx, "[db] ballot_owner('%s', cert=?) -> hash='%s' version=%d", cmd->election_id,
            cmd->hash, cmd->version);
      return;
    case BB_DB_GET_TALLY:
      bb_log(ctx, "[db] select option_index from ballot where election_id = '%s' and superseded = 0",
            cmd->election_id);
      return;
    case BB_DB_GET_HASHES:
      bb_log(ctx,
            "[db] select hash, option_index from ballot where election_id = '%s' and superseded = 0",
            cmd->election_id);
      return;
    case BB_DB_FIND_HASH:
      bb_log(ctx,
            "[db] select 1 from ballot where election_id = '%s' and hash = '%s' and superseded = 0",
            cmd->election_id, cmd->hash);
      return;
    case BB_DB_NONCE_SEEN:
      bb_log(ctx, "[db] select 1 from nonce where election_id = '%s' and nonce = '%s'",
            cmd->election_id, cmd->nonce);
      return;
    case BB_DB_GET_PRIOR_BALLOT:
      /* The cert is bound as a parameter and deliberately not rendered: this
       * is the one query keyed by voter identity, and logging it would
       * create the voter-to-ballot link R2 forbids. */
      bb_log(ctx,
            "[db] select hash, version from ballot_owner where election_id = '%s' and cert=?",
            cmd->election_id);
      return;
  }
}

static bb_result_t run_insert_election(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd) {
  (void)ctx;
  if (cmd->config == NULL || unsafe(cmd->election_id) || unsafe(cmd->config->title) ||
      unsafe(cmd->config->open_time) || unsafe(cmd->config->close_time)) {
    return BB_ERR_DB;
  }
  const bb_config_t *cfg = cmd->config;

  char qid[QUOTE_MAX], qtitle[QUOTE_MAX], qopen[QUOTE_MAX], qclose[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qtitle, sizeof qtitle, cfg->title);
  db_quote(qopen, sizeof qopen, cfg->open_time);
  db_quote(qclose, sizeof qclose, cfg->close_time);

  char sql[SQL_MAX];
  snprintf(sql, sizeof sql,
          "insert into " BB_DB_TABLE_ELECTION " values (%d, %s, %s, 'DRAFT', %s, %s);",
          id_seq(cmd->election_id), qid, qtitle, qopen, qclose);
  bb_result_t r = exec_stmt(conn, sql, NULL, 0);
  if (r != BB_OK) {
    return r;
  }

  for (int i = 0; i < cfg->option_count && i < BB_MAX_OPTIONS; i++) {
    if (unsafe(cfg->options[i])) {
      return BB_ERR_DB;
    }
    char qopt[QUOTE_MAX];
    db_quote(qopt, sizeof qopt, cfg->options[i]);
    snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_OPTION " values (%s, %d, %s);", qid, i,
            qopt);
    r = exec_stmt(conn, sql, NULL, 0);
    if (r != BB_OK) {
      return r;
    }
  }

  for (int i = 0; i < cfg->eligible_count && i < BB_MAX_VOTERS; i++) {
    if (unsafe(cfg->eligible[i])) {
      return BB_ERR_DB;
    }
    char qelig[QUOTE_MAX];
    db_quote(qelig, sizeof qelig, cfg->eligible[i]);
    snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_ELIGIBLE " values (%s, %s);", qid, qelig);
    r = exec_stmt(conn, sql, NULL, 0);
    if (r != BB_OK) {
      return r;
    }
  }
  return BB_OK;
}

static bb_result_t run_get_election(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd,
                                    bb_db_result_t *out) {
  (void)ctx;
  if (out == NULL) {
    return BB_ERR_DB;
  }

  char qid[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql,
          "select seq, id, title, state, open_time, close_time from " BB_DB_TABLE_ELECTION
          " where id = %s;",
          qid);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  int rows = db_row_count(body);
  if (rows < 0) {
    return BB_ERR_DB;
  }
  if (rows == 0) {
    out->found = 0;
    return BB_OK;
  }

  const char *f[6];
  size_t len[6];
  if (db_row_fields(body, 0, f, len, 6) < 6) {
    return BB_ERR_DB;
  }

  bb_election_t e;
  memset(&e, 0, sizeof e);
  field_copy(f[1], len[1], e.id, BB_ID_LEN);
  field_copy(f[2], len[2], e.title, BB_TITLE_LEN);
  char state_text[16];
  field_copy(f[3], len[3], state_text, sizeof state_text);
  if (parse_state(state_text, &e.state) != 0) {
    return BB_ERR_DB;
  }
  field_copy(f[4], len[4], e.open_time, BB_TIME_LEN);
  field_copy(f[5], len[5], e.close_time, BB_TIME_LEN);

  snprintf(sql, sizeof sql, "select idx, name from " BB_DB_TABLE_OPTION " where election_id = %s;",
          qid);
  r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  int orows = db_row_count(body);
  if (orows < 0) {
    orows = 0;
  }
  for (int i = 0; i < orows; i++) {
    const char *of[2];
    size_t olen[2];
    if (db_row_fields(body, i, of, olen, 2) < 2) {
      continue;
    }
    int idx = field_int(of[0], olen[0]);
    if (idx >= 0 && idx < BB_MAX_OPTIONS) {
      field_copy(of[1], olen[1], e.options[idx], BB_OPTION_LEN);
    }
  }
  e.option_count = orows < BB_MAX_OPTIONS ? orows : BB_MAX_OPTIONS;

  snprintf(sql, sizeof sql, "select cert_name from " BB_DB_TABLE_ELIGIBLE " where election_id = %s;",
          qid);
  r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  int erows = db_row_count(body);
  if (erows < 0) {
    erows = 0;
  }
  for (int i = 0; i < erows && i < BB_MAX_VOTERS; i++) {
    const char *ef[1];
    size_t elen[1];
    if (db_row_fields(body, i, ef, elen, 1) < 1) {
      continue;
    }
    field_copy(ef[0], elen[0], e.eligible[i], BB_CERT_LEN);
  }
  e.eligible_count = erows < BB_MAX_VOTERS ? erows : BB_MAX_VOTERS;

  out->election = e;
  out->found = 1;
  return BB_OK;
}

static bb_result_t run_update_state(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd) {
  (void)ctx;
  char qid[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql,
          "select seq, title, open_time, close_time from " BB_DB_TABLE_ELECTION
          " where id = %s;",
          qid);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  if (db_row_count(body) <= 0) {
    /* The caller (bb_transition_state) always reads the election back first
     * and only calls this when it exists; a miss here means it vanished
     * between those two reads, which nothing in this single-writer system
     * can cause - report it as an infrastructure fault, not NOT_FOUND. */
    return BB_ERR_DB;
  }

  const char *f[4];
  size_t len[4];
  if (db_row_fields(body, 0, f, len, 4) < 4) {
    return BB_ERR_DB;
  }
  char seq_text[24], title_raw[BB_TITLE_LEN], open_raw[BB_TIME_LEN], close_raw[BB_TIME_LEN];
  field_copy(f[0], len[0], seq_text, sizeof seq_text);
  field_copy(f[1], len[1], title_raw, sizeof title_raw);
  field_copy(f[2], len[2], open_raw, sizeof open_raw);
  field_copy(f[3], len[3], close_raw, sizeof close_raw);

  snprintf(sql, sizeof sql, "delete from " BB_DB_TABLE_ELECTION " where id = %s;", qid);
  r = exec_stmt(conn, sql, NULL, 0);
  if (r != BB_OK) {
    return r;
  }

  char qtitle[QUOTE_MAX], qopen[QUOTE_MAX], qclose[QUOTE_MAX];
  db_quote(qtitle, sizeof qtitle, title_raw);
  db_quote(qopen, sizeof qopen, open_raw);
  db_quote(qclose, sizeof qclose, close_raw);

  snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_ELECTION " values (%s, %s, %s, '%s', %s, %s);",
          seq_text, qid, qtitle, bb_state_str(cmd->new_state), qopen, qclose);
  return exec_stmt(conn, sql, NULL, 0);
}

static bb_result_t run_append_ballot(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd) {
  (void)ctx;
  if (cmd->hash_row == NULL) {
    return BB_ERR_DB;
  }

  char qid[QUOTE_MAX], qhash[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qhash, sizeof qhash, cmd->hash_row->hash);

  char sql[SQL_MAX];
  snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_BALLOT " values (%s, %s, %d, %d, %d);", qid,
          qhash, cmd->hash_row->option_index, cmd->hash_row->version, cmd->hash_row->superseded);
  return exec_stmt(conn, sql, NULL, 0);
}

static bb_result_t run_mark_superseded(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd) {
  (void)ctx;
  char qid[QUOTE_MAX], qhash[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qhash, sizeof qhash, cmd->hash);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql,
          "select option_index, version from " BB_DB_TABLE_BALLOT
          " where election_id = %s and hash = %s;",
          qid, qhash);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  if (db_row_count(body) <= 0) {
    /* record_ballot_locked passes the exact hash it just read via
     * GET_PRIOR_BALLOT, in the same transaction - a miss here is
     * infrastructure, never a caller error. */
    return BB_ERR_DB;
  }

  const char *f[2];
  size_t len[2];
  if (db_row_fields(body, 0, f, len, 2) < 2) {
    return BB_ERR_DB;
  }
  char opt_text[24], ver_text[24];
  field_copy(f[0], len[0], opt_text, sizeof opt_text);
  field_copy(f[1], len[1], ver_text, sizeof ver_text);

  snprintf(sql, sizeof sql, "delete from " BB_DB_TABLE_BALLOT " where election_id = %s and hash = %s;",
          qid, qhash);
  r = exec_stmt(conn, sql, NULL, 0);
  if (r != BB_OK) {
    return r;
  }

  snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_BALLOT " values (%s, %s, %s, %s, 1);", qid,
          qhash, opt_text, ver_text);
  return exec_stmt(conn, sql, NULL, 0);
}

static bb_result_t run_nonce_mark(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd) {
  (void)ctx;
  char qid[QUOTE_MAX], qnonce[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qnonce, sizeof qnonce, cmd->nonce);

  char sql[SQL_MAX];
  snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_NONCE " values (%s, %s);", qid, qnonce);
  return exec_stmt(conn, sql, NULL, 0);
}

static bb_result_t run_set_owner(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd) {
  (void)ctx;
  char qid[QUOTE_MAX], qcert[QUOTE_MAX], qhash[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qcert, sizeof qcert, cmd->cert_name);
  db_quote(qhash, sizeof qhash, cmd->hash);

  char sql[SQL_MAX];
  snprintf(sql, sizeof sql,
          "delete from " BB_DB_TABLE_OWNER " where election_id = %s and cert_name = %s;", qid,
          qcert);
  bb_result_t r = exec_stmt(conn, sql, NULL, 0);
  if (r != BB_OK) {
    return r;
  }

  snprintf(sql, sizeof sql, "insert into " BB_DB_TABLE_OWNER " values (%s, %s, %s, %d);", qid, qcert,
          qhash, cmd->version);
  return exec_stmt(conn, sql, NULL, 0);
}

static bb_result_t run_get_tally(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd,
                                 bb_db_result_t *out) {
  (void)ctx;
  if (out == NULL) {
    return BB_ERR_DB;
  }

  char qid[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql,
          "select option_index from " BB_DB_TABLE_BALLOT
          " where election_id = %s and superseded = 0;",
          qid);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  int rows = db_row_count(body);
  if (rows < 0) {
    rows = 0;
  }
  for (int i = 0; i < rows; i++) {
    const char *f[1];
    size_t len[1];
    if (db_row_fields(body, i, f, len, 1) < 1) {
      continue;
    }
    int idx = field_int(f[0], len[0]);
    if (idx >= 0 && idx < BB_MAX_OPTIONS) {
      out->tally[idx]++;
    }
  }
  return BB_OK;
}

static bb_result_t run_get_hashes(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd,
                                  bb_db_result_t *out) {
  (void)ctx;
  if (out == NULL) {
    return BB_ERR_DB;
  }

  char qid[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql,
          "select hash, option_index, version, superseded from " BB_DB_TABLE_BALLOT
          " where election_id = %s and superseded = 0;",
          qid);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  int rows = db_row_count(body);
  if (rows < 0) {
    rows = 0;
  }
  int n = 0;
  for (int i = 0; i < rows && n < BB_MAX_VOTERS; i++) {
    const char *f[4];
    size_t len[4];
    if (db_row_fields(body, i, f, len, 4) < 4) {
      continue;
    }
    bb_ballot_hash_t *row = &out->hashes[n];
    field_copy(f[0], len[0], row->hash, BB_HASH_LEN);
    row->option_index = field_int(f[1], len[1]);
    row->version = field_int(f[2], len[2]);
    row->superseded = field_int(f[3], len[3]);
    n++;
  }
  out->hash_count = n;
  return BB_OK;
}

static bb_result_t run_find_hash(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd,
                                 bb_db_result_t *out) {
  (void)ctx;
  if (out == NULL) {
    return BB_ERR_DB;
  }

  char qid[QUOTE_MAX], qhash[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qhash, sizeof qhash, cmd->hash);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql,
          "select hash, option_index, version, superseded from " BB_DB_TABLE_BALLOT
          " where election_id = %s and hash = %s and superseded = 0;",
          qid, qhash);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  if (db_row_count(body) <= 0) {
    out->found = 0;
    return BB_OK;
  }
  const char *f[4];
  size_t len[4];
  if (db_row_fields(body, 0, f, len, 4) < 4) {
    return BB_ERR_DB;
  }
  field_copy(f[0], len[0], out->row.hash, BB_HASH_LEN);
  out->row.option_index = field_int(f[1], len[1]);
  out->row.version = field_int(f[2], len[2]);
  out->row.superseded = field_int(f[3], len[3]);
  out->found = 1;
  return BB_OK;
}

static bb_result_t run_nonce_seen(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd,
                                  bb_db_result_t *out) {
  (void)ctx;
  if (out == NULL) {
    return BB_ERR_DB;
  }

  char qid[QUOTE_MAX], qnonce[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qnonce, sizeof qnonce, cmd->nonce);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql, "select nonce from " BB_DB_TABLE_NONCE " where election_id = %s and nonce = %s;",
          qid, qnonce);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  out->found = db_row_count(body) > 0 ? 1 : 0;
  return BB_OK;
}

static bb_result_t run_get_prior_ballot(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd,
                                        bb_db_result_t *out) {
  (void)ctx;
  if (out == NULL) {
    return BB_ERR_DB;
  }

  char qid[QUOTE_MAX], qcert[QUOTE_MAX];
  db_quote(qid, sizeof qid, cmd->election_id);
  db_quote(qcert, sizeof qcert, cmd->cert_name);

  char sql[SQL_MAX], body[BODY_MAX];
  snprintf(sql, sizeof sql,
          "select hash, version from " BB_DB_TABLE_OWNER " where election_id = %s and cert_name = %s;",
          qid, qcert);
  bb_result_t r = exec_stmt(conn, sql, body, sizeof body);
  if (r != BB_OK) {
    return r;
  }
  if (db_row_count(body) <= 0) {
    out->found = 0;
    return BB_OK;
  }
  const char *f[2];
  size_t len[2];
  if (db_row_fields(body, 0, f, len, 2) < 2) {
    return BB_ERR_DB;
  }
  field_copy(f[0], len[0], out->row.hash, BB_HASH_LEN);
  out->row.version = field_int(f[1], len[1]);
  out->found = 1;
  return BB_OK;
}

static bb_result_t run_op(bb_ctx *ctx, db_socket_t *conn, const bb_db_cmd_t *cmd,
                          bb_db_result_t *out) {
  switch (cmd->op) {
    case BB_DB_INSERT_ELECTION: return run_insert_election(ctx, conn, cmd);
    case BB_DB_UPDATE_STATE: return run_update_state(ctx, conn, cmd);
    case BB_DB_APPEND_BALLOT: return run_append_ballot(ctx, conn, cmd);
    case BB_DB_MARK_SUPERSEDED: return run_mark_superseded(ctx, conn, cmd);
    case BB_DB_NONCE_MARK: return run_nonce_mark(ctx, conn, cmd);
    case BB_DB_SET_OWNER: return run_set_owner(ctx, conn, cmd);
    case BB_DB_GET_ELECTION: return run_get_election(ctx, conn, cmd, out);
    case BB_DB_GET_TALLY: return run_get_tally(ctx, conn, cmd, out);
    case BB_DB_GET_HASHES: return run_get_hashes(ctx, conn, cmd, out);
    case BB_DB_FIND_HASH: return run_find_hash(ctx, conn, cmd, out);
    case BB_DB_NONCE_SEEN: return run_nonce_seen(ctx, conn, cmd, out);
    case BB_DB_GET_PRIOR_BALLOT: return run_get_prior_ballot(ctx, conn, cmd, out);
  }
  return BB_ERR_DB;
}

/* One self-owned attempt: open, wrap in a transaction if the op writes more
 * than one statement, run it, commit/rollback, close. Returns BB_ERR_RETRY
 * on a deadlock abort so the caller (db_exec) can redo the whole attempt on
 * a fresh connection. */
static bb_result_t one_shot_attempt(bb_ctx *ctx, const bb_db_cmd_t *cmd, bb_db_result_t *out,
                                    int is_write) {
  db_socket_opts_t opts = bb_effective_db_opts(ctx);
  db_socket_t *conn = db_socket_open(&opts);
  if (conn == NULL) {
    return BB_ERR_DB;
  }

  bb_result_t r = BB_OK;
  if (is_write) {
    r = exec_stmt(conn, "set transaction read write;", NULL, 0);
  }
  if (r == BB_OK) {
    r = run_op(ctx, conn, cmd, out);
  }
  if (is_write) {
    if (r == BB_OK) {
      r = exec_stmt(conn, "commit;", NULL, 0);
    } else if (r != BB_ERR_RETRY) {
      /* Intent, not a requirement: closing rolls back anything left open
       * regardless (see bb_db_rollback's own comment). */
      (void)db_socket_exec(conn, "rollback;", NULL, 0);
    }
    /* r == BB_ERR_RETRY: the transaction is already gone server-side - no
     * rollback of it is possible or needed. */
  }
  db_socket_close(conn);
  return r;
}

bb_result_t db_exec(bb_ctx *ctx, const bb_db_cmd_t *cmd, bb_db_result_t *out) {
  if (ctx == NULL || cmd == NULL) {
    return BB_ERR_DB;
  }
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
  }
  log_cmd(ctx, cmd);

  if (ctx->txn_conn != NULL) {
    /* Caller-managed transaction (bb_db_begin): add our statements to it.
     * BB_ERR_RETRY is returned verbatim - only the caller, which owns the
     * whole multi-call sequence, can redo it from the top. */
    return run_op(ctx, ctx->txn_conn, cmd, out);
  }

  int is_write = op_is_write(cmd->op);
  bb_result_t r = BB_ERR_DB;
  for (int attempt = 0; attempt < SELF_RETRY_ATTEMPTS; attempt++) {
    if (out != NULL) {
      memset(out, 0, sizeof(*out));
    }
    r = one_shot_attempt(ctx, cmd, out, is_write);
    if (r != BB_ERR_RETRY) {
      return r;
    }
    usleep(SELF_RETRY_BACKOFF_US << attempt);
  }
  return BB_ERR_DB; /* retries exhausted */
}
