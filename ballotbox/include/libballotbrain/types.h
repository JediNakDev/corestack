#ifndef BALLOTBRAIN_TYPES_H
#define BALLOTBRAIN_TYPES_H

/*
 * libballotbrain - canonical domain model.
 *
 * These are the server-authoritative shapes for BallotBox (the `BallotdService` and
 * `Election`/`Ballot`/`BallotHash`/`Receipt` classes in the README class
 * diagram). This header is the single source of truth for the model; the
 * client library (libballotclient) reuses these types, and the eventual
 * SimpleDB layer persists them. The demo `mock.h` copies in ballotu/ballotctl
 * are intentionally left independent until UI integration.
 */

#include <stddef.h>
#include <stdint.h>

/* Fixed capacities. These mirror the demo mocks and are deliberately small;
 * they grow (or move to dynamic storage) when SimpleDB replaces the DB seam. */
#define BB_ID_LEN 16
#define BB_TITLE_LEN 64
#define BB_OPTION_LEN 32
#define BB_CERT_LEN 32
#define BB_TIME_LEN 32
#define BB_HASH_LEN 65 /* 64 hex chars + NUL (SHA-256) */
#define BB_NONCE_LEN 33
#define BB_MAX_OPTIONS 16
#define BB_MAX_VOTERS 64
#define BB_CIPHERTEXT_MAX 256

/* Election lifecycle. Legal transitions: DRAFT->OPEN->CLOSED->PUBLISHED. */
typedef enum {
  BB_STATE_DRAFT,
  BB_STATE_OPEN,
  BB_STATE_CLOSED,
  BB_STATE_PUBLISHED
} bb_state_t;

/* Certificate verification outcome (Certificate/CertStatus in the class diagram). */
typedef enum {
  BB_CERT_INVALID,
  BB_CERT_EXPIRED,
  BB_CERT_NOT_ELIGIBLE,
  BB_CERT_VALID
} bb_cert_status_t;

/*
 * Result codes. Every fallible bb_* function returns one of these; data comes
 * back through out-parameters. Codes are specific enough to carry the exact
 * error language the use cases require (e.g. UC-1 "specific error", UC-2
 * refusal partitions).
 */
typedef enum {
  BB_OK = 0,

  /* UC-1 config validation (specific per README error state 2a) */
  BB_ERR_CONFIG_TITLE,   /* empty title */
  BB_ERR_CONFIG_OPTIONS, /* fewer than 2 options */
  BB_ERR_CONFIG_TIME,    /* close time <= open time */
  BB_ERR_CONFIG_ID_TAKEN, /* caller-supplied election id already exists */

  /* lifecycle */
  BB_ERR_ILLEGAL_TRANSITION, /* not a legal state edge */
  BB_ERR_NOT_OPEN,           /* action requires OPEN election */
  BB_ERR_CLOSED,             /* ballot arrived after close */
  BB_ERR_NOT_PUBLISHED,      /* results requested before publish */

  /* eligibility / certs */
  BB_ERR_NOT_ELIGIBLE,
  BB_ERR_CERT_INVALID,
  BB_ERR_CERT_EXPIRED,

  /* ballot recording */
  BB_ERR_BAD_OPTION, /* option index outside [0, option_count) */
  BB_ERR_REPLAY,     /* nonce already seen */
  BB_ERR_DECRYPT,    /* payload failed RSA-OAEP decryption */

  /* lookup */
  BB_ERR_NOT_FOUND, /* election or hash not found */

  /* client-side session state */
  BB_ERR_NOT_JOINED, /* the voter has not joined an election yet */

  /* infrastructure */
  BB_ERR_DB,             /* DB seam reported a failure */
  BB_ERR_NOT_IMPLEMENTED, /* deferred behind a stubbed seam (no readback yet) */
  BB_ERR_RETRY /* internal: SimpleDB aborted a transaction to break a deadlock
                * (<<END retry>>). Never returned across the daemon boundary -
                * db.c's transaction wrapper retries the whole transaction
                * itself and this code never escapes libballotbrain. Kept in
                * the public enum (not a private one) only so bb_result_t
                * stays the single vocabulary db_exec ever returns. */
} bb_result_t;

/* Configuration supplied by an admin to create an election (UC-1). */
typedef struct {
  char title[BB_TITLE_LEN];
  char options[BB_MAX_OPTIONS][BB_OPTION_LEN];
  int option_count;
  char eligible[BB_MAX_VOTERS][BB_CERT_LEN];
  int eligible_count;
  char open_time[BB_TIME_LEN];  /* ISO-8601 string; compared lexicographically */
  char close_time[BB_TIME_LEN];
} bb_config_t;

/* An election as held by the daemon (the Election entity class). */
typedef struct {
  char id[BB_ID_LEN];
  char title[BB_TITLE_LEN];
  bb_state_t state;
  char options[BB_MAX_OPTIONS][BB_OPTION_LEN];
  int option_count;
  char eligible[BB_MAX_VOTERS][BB_CERT_LEN];
  int eligible_count;
  char open_time[BB_TIME_LEN];
  char close_time[BB_TIME_LEN];
  int tally[BB_MAX_OPTIONS];
} bb_election_t;

/* An encrypted ballot submitted by a voter (UC-3/UC-4). */
typedef struct {
  char cert_name[BB_CERT_LEN]; /* used for eligibility/version, never stored beside the hash */
  char nonce[BB_NONCE_LEN];    /* anti-replay */
  uint8_t payload[BB_CIPHERTEXT_MAX];
  size_t payload_len;
} bb_ballot_t;

/*
 * A recorded ballot's hash row. This is the only thing published in results;
 * it deliberately carries no voter identity, so the tally is verifiable
 * without being traceable (R2).
 */
typedef struct {
  char hash[BB_HASH_LEN];
  int option_index;
  int version;
  int superseded;
} bb_ballot_hash_t;

/*
 * The published view of an election (UC-5): the tally plus the hash rows that
 * back it. Like bb_ballot_hash_t, it carries no voter identity.
 */
typedef struct {
  char title[BB_TITLE_LEN]; /* same free reuse of the already-loaded election
                              * as options[] below - so a results screen can
                              * show the title next to the id the caller
                              * already typed, not just the id alone */
  int tally[BB_MAX_OPTIONS];
  int option_count;
  char options[BB_MAX_OPTIONS][BB_OPTION_LEN]; /* names, parallel to tally[] -
                                                 * bb_get_results already loads
                                                 * the election to gate on
                                                 * PUBLISHED/eligibility, so
                                                 * this costs no extra read */
  bb_ballot_hash_t hashes[BB_MAX_VOTERS];
  int hash_count;
} bb_results_t;

/* The receipt handed back to a voter after a successful cast/update. */
typedef struct {
  char hash[BB_HASH_LEN];
  char issued_at[BB_TIME_LEN];
} bb_receipt_t;

#endif /* BALLOTBRAIN_TYPES_H */
