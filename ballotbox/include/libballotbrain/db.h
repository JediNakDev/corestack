#ifndef BALLOTBRAIN_DB_H
#define BALLOTBRAIN_DB_H

/*
 * DB seam.
 *
 * The authoritative store is SimpleDB (50.043), reached through
 * libtetrisdb/socket/db.h's synchronous SocketRunner client (a real backend
 * landed - see db/docs/c-daemon-integration.md for the wire contract this
 * translates onto). Every storage access still goes through one narrow
 * function, db_exec(), which takes a *typed, parameterized* command struct
 * (never a raw SQL string - type-safe and injection-safe by construction).
 *
 * Contract:
 *   - Single-statement ops (INSERT_ELECTION, GET_ELECTION, GET_TALLY,
 *     GET_HASHES, FIND_HASH, NONCE_SEEN, GET_PRIOR_BALLOT) each open one
 *     connection, run, and close it - see socket/db.h's "PER-OPERATION
 *     LIFETIME".
 *   - Multi-statement writes that must be atomic (bb_record_ballot's
 *     read-check-write, bb_transition_state's read-then-write) run inside an
 *     explicit transaction: bb_db_begin() opens a connection and starts one,
 *     every db_exec() call in between reuses that same connection, and
 *     bb_db_commit()/bb_db_rollback() ends it and closes. A db_exec() call
 *     made with no transaction open uses its own one-shot connection, as
 *     above.
 *   - DB_RETRY (a deadlock victim, not an error) surfaces as BB_ERR_RETRY.
 *     The whole transaction is the retry unit - see bb_db_begin()'s doc
 *     comment - so only a caller that owns the begin/commit sequence may
 *     see this code; it must never reach the daemon boundary.
 *
 * Log secrecy (R2 / test U-21): the rendered log line for a ballot append
 * carries the hash and option, never the submitting cert. No log line pairs a
 * voter identity with a ballot or hash. BB_DB_SET_OWNER is the one command
 * that does carry an identity, exactly like GET_PRIOR_BALLOT, and is logged
 * the same way - the cert bound as a parameter, never rendered.
 */

#include "libballotbrain/types.h"
#include "libtetrisdb/socket/db.h"

struct bb_ctx;

/*
 * The catalog lines ballotd must ensure (db_ensure_table) before bin/
 * tetrisdb start ever runs against this data directory - same precedent as
 * TETRISAUTH_DB_TABLE/SCHEMA in include/libtetrisauth/auth.h, and for
 * the same reason: the schema text lives beside db.c, its only parser, so a
 * column reordered in one file cannot silently disagree with the positional
 * db_row_fields() reads here. See db.c's file comment for the full schema
 * design (why six tables, not fewer).
 */
#define BB_DB_TABLE_ELECTION "election"
#define BB_DB_SCHEMA_ELECTION \
  "seq int, id string, title string, state string, open_time string, close_time string"

#define BB_DB_TABLE_OPTION "election_option"
#define BB_DB_SCHEMA_OPTION "election_id string, idx int, name string"

#define BB_DB_TABLE_ELIGIBLE "election_eligible"
#define BB_DB_SCHEMA_ELIGIBLE "election_id string, cert_name string"

#define BB_DB_TABLE_BALLOT "ballot"
#define BB_DB_SCHEMA_BALLOT \
  "election_id string, hash string, option_index int, version int, superseded int"

#define BB_DB_TABLE_OWNER "ballot_owner"
#define BB_DB_SCHEMA_OWNER "election_id string, cert_name string, hash string, version int"

#define BB_DB_TABLE_NONCE "nonce"
#define BB_DB_SCHEMA_NONCE "election_id string, nonce string"

/*
 * Where the store is. Call once, before the first db_exec()/bb_db_begin() -
 * ballotd does this at startup, right after bb_create(). Unset (bb_create()
 * alone) falls back to libtetrisdb/socket/conf.h's defaults, which is enough
 * for a dev checkout with bin/tetrisdb running on its own defaults.
 */
void bb_set_db_opts(struct bb_ctx *ctx, const db_socket_opts_t *opts);

typedef enum {
  BB_DB_INSERT_ELECTION,  /* write: persist a new election in DRAFT */
  BB_DB_UPDATE_STATE,     /* write: set election.state */
  BB_DB_APPEND_BALLOT,    /* write: add a ballot hash row */
  BB_DB_MARK_SUPERSEDED,  /* write: flag the exact prior version superseded */
  BB_DB_NONCE_MARK,       /* write: record a consumed nonce */
  BB_DB_SET_OWNER,        /* write: point a voter's current ballot at hash/version -
                            * the private mapping GET_PRIOR_BALLOT reads back */
  BB_DB_GET_ELECTION,     /* read: fetch an election by id */
  BB_DB_GET_TALLY,        /* read: fetch tally of non-superseded ballots */
  BB_DB_GET_HASHES,       /* read: fetch published hash list */
  BB_DB_FIND_HASH,        /* read: look up one published, non-superseded hash */
  BB_DB_NONCE_SEEN,       /* read: has this nonce been consumed? */
  BB_DB_GET_PRIOR_BALLOT  /* read: this voter's current ballot version, if any */
} bb_db_op_t;

/*
 * One command. Only the fields relevant to `op` are read; the rest are
 * ignored. Kept as a flat struct (not a union) for simple, readable call
 * sites - the SQL translation in db.c reads exactly the fields it needs.
 */
typedef struct {
  bb_db_op_t op;
  char election_id[BB_ID_LEN];
  bb_state_t new_state;               /* UPDATE_STATE */
  const bb_ballot_hash_t *hash_row;   /* APPEND_BALLOT */
  char hash[BB_HASH_LEN];             /* FIND_HASH / MARK_SUPERSEDED / SET_OWNER */
  int version;                        /* SET_OWNER */
  char nonce[BB_NONCE_LEN];           /* NONCE_MARK / NONCE_SEEN */
  char cert_name[BB_CERT_LEN];        /* GET_PRIOR_BALLOT / SET_OWNER */
  const bb_config_t *config;          /* INSERT_ELECTION */
} bb_db_cmd_t;

/*
 * Read-op output.
 */
typedef struct {
  bb_election_t election;          /* GET_ELECTION */
  int tally[BB_MAX_OPTIONS];       /* GET_TALLY */
  bb_ballot_hash_t hashes[BB_MAX_VOTERS]; /* GET_HASHES */
  int hash_count;
  int found;                       /* FIND_HASH / NONCE_SEEN / GET_PRIOR_BALLOT: 1 if present */
  bb_ballot_hash_t row;            /* FIND_HASH / GET_PRIOR_BALLOT: the matching row -
                                     * GET_PRIOR_BALLOT only ever fills .hash/.version,
                                     * the private mapping carries nothing else */
} bb_db_result_t;

/*
 * Execute one command against the real store (or the active transaction, see
 * bb_db_begin()). `out` may be NULL for write ops. Logs a SQL-ish line to the
 * context's log sink.
 */
bb_result_t db_exec(struct bb_ctx *ctx, const bb_db_cmd_t *cmd, bb_db_result_t *out);

/*
 * Open a connection and start a transaction ("set transaction read write;").
 * Every db_exec() call on this ctx until the matching commit/rollback reuses
 * this one connection - required correctness, not an optimisation: SimpleDB
 * has no request ids, so a transaction is exactly "the statements one
 * connection sees between begin and commit" (db/docs/c-daemon-integration.md
 * section 2, invariant 2).
 *
 * ON BB_ERR_RETRY FROM ANY STATEMENT INSIDE, THE WHOLE TRANSACTION IS THE
 * RETRY UNIT: roll back (bb_db_rollback), then bb_db_begin() again and redo
 * every statement, never just the one that reported it - an aborted
 * transaction takes every earlier statement in it down too (mirrors
 * libtetrisauth's account.c::register_txn).
 *
 * Returns BB_ERR_DB if a transaction is already open on ctx or the connect
 * itself fails.
 */
bb_result_t bb_db_begin(struct bb_ctx *ctx);

/* Commits the open transaction and closes its connection. BB_ERR_DB if none
 * is open. */
bb_result_t bb_db_commit(struct bb_ctx *ctx);

/* Rolls back the open transaction and closes its connection. Best-effort:
 * closing alone also rolls back per the wire contract, so this never fails
 * the caller - it exists so a reader following the code sees where the
 * transaction ends, same reasoning as account.c's intentional rollback. */
void bb_db_rollback(struct bb_ctx *ctx);

#endif /* BALLOTBRAIN_DB_H */
