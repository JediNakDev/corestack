#ifndef FAKE_BRAIN_SEAMS_H
#define FAKE_BRAIN_SEAMS_H

/*
 * Substitute seams for libballotbrain unit tests.
 *
 * A unit test exercises exactly one function, so everything that function calls
 * out to - the store (db_exec) and the crypto/PKI seam (bb_verify_cert,
 * bb_decrypt_ballot, bb_issue_receipt) - is replaced here by a programmable
 * fake. Each test states the answers the seams give and then asserts both the
 * return value and the commands the function issued. No SimpleDB, no keys, no
 * sockets are involved, so none of these tests wait on those milestones.
 *
 * Link mechanics: this header *defines* the seam symbols, so the linker
 * satisfies libballotbrain.a's references from here and never pulls the real
 * db.o / crypto.o members. Include it in exactly one translation unit per test
 * binary (each test file is its own binary).
 */

#include "libballotbrain/ballotbrain.h"

#include <pthread.h>
#include <string.h>

#define FAKE_MAX_CALLS 512

/* One recorded db_exec invocation, flattened to the fields we assert on. */
typedef struct {
  bb_db_op_t op;
  char election_id[BB_ID_LEN];
  bb_state_t new_state;
  char hash[BB_HASH_LEN];
  int version; /* SET_OWNER */
  char nonce[BB_NONCE_LEN];
  char cert_name[BB_CERT_LEN];
  bb_ballot_hash_t row; /* APPEND_BALLOT payload */
} fake_call_t;

typedef struct {
  /* ---- answers the seams give -------------------------------------- */
  int election_present;       /* 0 => GET_ELECTION reports no such row */
  bb_election_t election;
  int nonce_seen;             /* NONCE_SEEN */
  int prior_found;            /* GET_PRIOR_BALLOT */
  bb_ballot_hash_t prior_row;
  int find_found;             /* FIND_HASH */
  bb_ballot_hash_t find_row;
  int tally[BB_MAX_OPTIONS];  /* GET_TALLY */
  int hash_count;             /* GET_HASHES */
  bb_ballot_hash_t hashes[BB_MAX_VOTERS];

  int fail_armed;             /* make one op fail, to test error propagation */
  bb_db_op_t fail_op;
  bb_result_t fail_code;

  bb_cert_status_t cert_status; /* bb_verify_cert */
  bb_result_t decrypt_result;   /* bb_decrypt_ballot */
  int decrypt_option_set;       /* 1 => hand back decrypt_option verbatim, */
  int decrypt_option;           /*      including an out-of-range one */
  bb_result_t receipt_result;   /* bb_issue_receipt */

  /* ---- what the unit under test did --------------------------------- */
  fake_call_t calls[FAKE_MAX_CALLS];
  int call_count;
  int verify_cert_calls;
  int decrypt_calls;
  int receipt_calls;
  pthread_mutex_t lock; /* the fake is driven from several threads in U-22 */
} fake_seams_t;

fake_seams_t fake;

/* Reset to the permissive defaults: no election stored, valid cert, decryption
 * succeeds and reads the option out of payload[0]. */
static inline void fake_reset(void) {
  memset(&fake, 0, sizeof(fake));
  fake.cert_status = BB_CERT_VALID;
  fake.decrypt_result = BB_OK;
  fake.receipt_result = BB_OK;
  pthread_mutex_init(&fake.lock, NULL);
}

/* Store one election for GET_ELECTION to return. */
static inline void fake_seed_election(const char *id, bb_state_t state, int option_count,
                                      const char *const *eligible, int eligible_count) {
  memset(&fake.election, 0, sizeof(fake.election));
  snprintf(fake.election.id, BB_ID_LEN, "%s", id);
  fake.election.state = state;
  fake.election.option_count = option_count;
  for (int i = 0; i < option_count && i < BB_MAX_OPTIONS; i++) {
    snprintf(fake.election.options[i], BB_OPTION_LEN, "option-%d", i);
  }
  for (int i = 0; i < eligible_count && i < BB_MAX_VOTERS; i++) {
    snprintf(fake.election.eligible[i], BB_CERT_LEN, "%s", eligible[i]);
  }
  fake.election.eligible_count = eligible_count;
  fake.election_present = 1;
}

/* How many times `op` was issued. */
static inline int fake_count(bb_db_op_t op) {
  int n = 0;
  for (int i = 0; i < fake.call_count; i++) {
    if (fake.calls[i].op == op) {
      n++;
    }
  }
  return n;
}

/* The last command issued with `op`, or NULL if there was none. */
static inline const fake_call_t *fake_last(bb_db_op_t op) {
  for (int i = fake.call_count - 1; i >= 0; i--) {
    if (fake.calls[i].op == op) {
      return &fake.calls[i];
    }
  }
  return NULL;
}

/* Total write commands issued - the "store unchanged" postcondition. */
static inline int fake_write_count(void) {
  return fake_count(BB_DB_INSERT_ELECTION) + fake_count(BB_DB_UPDATE_STATE) +
         fake_count(BB_DB_APPEND_BALLOT) + fake_count(BB_DB_MARK_SUPERSEDED) +
         fake_count(BB_DB_NONCE_MARK) + fake_count(BB_DB_SET_OWNER);
}

/* ---- the seams --------------------------------------------------------- */

bb_result_t db_exec(bb_ctx *ctx, const bb_db_cmd_t *cmd, bb_db_result_t *out) {
  (void)ctx;
  if (cmd == NULL) {
    return BB_ERR_DB;
  }

  pthread_mutex_lock(&fake.lock);
  if (fake.call_count < FAKE_MAX_CALLS) {
    fake_call_t *rec = &fake.calls[fake.call_count++];
    memset(rec, 0, sizeof(*rec));
    rec->op = cmd->op;
    memcpy(rec->election_id, cmd->election_id, BB_ID_LEN);
    rec->new_state = cmd->new_state;
    memcpy(rec->hash, cmd->hash, BB_HASH_LEN);
    rec->version = cmd->version;
    memcpy(rec->nonce, cmd->nonce, BB_NONCE_LEN);
    memcpy(rec->cert_name, cmd->cert_name, BB_CERT_LEN);
    if (cmd->hash_row != NULL) {
      rec->row = *cmd->hash_row;
    }
  }
  pthread_mutex_unlock(&fake.lock);

  if (fake.fail_armed && cmd->op == fake.fail_op) {
    return fake.fail_code;
  }

  switch (cmd->op) {
    case BB_DB_GET_ELECTION:
      if (out != NULL) {
        out->found = fake.election_present;
        out->election = fake.election;
      }
      return BB_OK;

    case BB_DB_NONCE_SEEN:
      if (out != NULL) {
        out->found = fake.nonce_seen;
      }
      return BB_OK;

    case BB_DB_GET_PRIOR_BALLOT:
      if (out != NULL) {
        out->found = fake.prior_found;
        out->row = fake.prior_row;
      }
      return BB_OK;

    case BB_DB_FIND_HASH:
      if (out != NULL) {
        out->found = fake.find_found;
        out->row = fake.find_row;
      }
      return BB_OK;

    case BB_DB_GET_TALLY:
      if (out != NULL) {
        memcpy(out->tally, fake.tally, sizeof(fake.tally));
      }
      return BB_OK;

    case BB_DB_GET_HASHES:
      if (out != NULL) {
        out->hash_count = fake.hash_count;
        memcpy(out->hashes, fake.hashes, sizeof(fake.hashes));
      }
      return BB_OK;

    default:
      /* Write ops: recorded above, nothing to return. */
      return BB_OK;
  }
}

/*
 * Transaction control, faked as trivial always-succeed no-ops. Real
 * bb_record_ballot wraps its whole read-check-write sequence in
 * bb_db_begin()/bb_db_commit(), which - for real - open and close a
 * db_socket_t; a fake db_exec has nothing to reuse a connection across, so
 * these just let the sequence run as if there were no transaction at all,
 * exactly the old (pre-transaction) test behaviour. Faked here, not left to
 * link the real txn.c, for the same isolation reason db_exec is faked above:
 * txn.c's real functions need a reachable SocketRunner no plain unit test
 * has.
 */
bb_result_t bb_db_begin(bb_ctx *ctx) {
  (void)ctx;
  return BB_OK;
}

bb_result_t bb_db_commit(bb_ctx *ctx) {
  (void)ctx;
  return BB_OK;
}

void bb_db_rollback(bb_ctx *ctx) {
  (void)ctx;
}

bb_cert_status_t bb_verify_cert(bb_ctx *ctx, const char *cert_name) {
  (void)ctx;
  (void)cert_name;
  fake.verify_cert_calls++;
  return fake.cert_status;
}

bb_result_t bb_decrypt_ballot(bb_ctx *ctx, const bb_ballot_t *ballot, int *option_index) {
  (void)ctx;
  fake.decrypt_calls++;
  if (fake.decrypt_result != BB_OK) {
    return fake.decrypt_result;
  }
  if (option_index != NULL) {
    *option_index = fake.decrypt_option_set
                        ? fake.decrypt_option
                        : (ballot != NULL && ballot->payload_len > 0 ? (int)ballot->payload[0] : 0);
  }
  return BB_OK;
}

bb_result_t bb_issue_receipt(bb_ctx *ctx, const bb_ballot_t *ballot, int version,
                             bb_receipt_t *out) {
  (void)ctx;
  pthread_mutex_lock(&fake.lock);
  fake.receipt_calls++;
  pthread_mutex_unlock(&fake.lock);
  if (fake.receipt_result != BB_OK) {
    return fake.receipt_result;
  }
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
    /* Distinct and predictable: the assertions only need "which ballot, which
     * version", not a real commitment. */
    snprintf(out->hash, BB_HASH_LEN, "hash-%s-v%d", ballot != NULL ? ballot->nonce : "?", version);
    snprintf(out->issued_at, BB_TIME_LEN, "1970-01-01T00:00:00Z");
  }
  return BB_OK;
}

#endif /* FAKE_BRAIN_SEAMS_H */
