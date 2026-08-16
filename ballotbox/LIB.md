# BallotBox Libraries

This document is the reference for the two logic libraries that hold the real BallotBox behaviour.
It is written for the teammates wiring in SimpleDB and the transport layer behind the seams described below.
Terminology, use cases (UC-1 to UC-6), and the class diagram are defined in [README](README.md).

## Scope and status

These libraries contain the **logic only**.
The three things the logic depends on from the outside world - persistence, transport, and cryptography - each sit behind a narrow seam whose implementation lands later.
Nothing here is integrated yet: `ballotd` does not call `libballotbrain`, and the `ballotu`/`ballotctl` TUIs still run on their own `mock.c` demo state.
The libraries build (`make`) and are archived to `lib/libballotbrain.a` and `lib/libballotclient.a`, ready to be linked and tested in isolation.

The logic itself is **complete**: every rule is enforced here against the seam contract, including the ones that need stored state.
What is stubbed is only the far side of each seam.
A function that asks the store for an election gets `BB_ERR_NOT_IMPLEMENTED` from today's stub and reports it, and gets a real row once SimpleDB lands - the function does not change either way.
That is also what makes each function unit-testable now: a test substitutes the seam and states the answer it wants (see [TEST](TEST.md)).

## `libballotbrain` - daemon-side authoritative logic

This is the `BallotdService` control class from the README solution class diagram.
Public umbrella header: `include/libballotbrain/ballotbrain.h`.
Prefix: `bb_*`.
All fallible functions return a `bb_result_t` code; data comes back through out-parameters.

### Canonical model (`include/libballotbrain/types.h`)

`libballotbrain` owns the single source of truth for the domain model, and `libballotclient` reuses it.
The demo `mock.h` copies in `ballotu`/`ballotctl` are intentionally left independent until UI integration.

- `bb_state_t` - `DRAFT`, `OPEN`, `CLOSED`, `PUBLISHED`.
- `bb_cert_status_t` - `INVALID`, `EXPIRED`, `NOT_ELIGIBLE`, `VALID`.
- `bb_result_t` - `BB_OK` plus specific error codes (e.g. `BB_ERR_CONFIG_TIME`, `BB_ERR_NOT_ELIGIBLE`, `BB_ERR_REPLAY`, `BB_ERR_NOT_IMPLEMENTED`).
- `bb_config_t`, `bb_election_t`, `bb_ballot_t`, `bb_ballot_hash_t`, `bb_receipt_t` - the config, election, ballot, published hash row, and receipt shapes.

### API

| Function | UC | Status | Notes |
| --- | --- | --- | --- |
| `bb_validate_config` | UC-1 | Complete (pure) | Title, option-count, and time-window checks; returns a specific `BB_ERR_CONFIG_*`. |
| `bb_create_election` | UC-1 | Complete | Validates, allocates a placeholder id, inserts via the DB seam. |
| `bb_is_legal_transition` | lifecycle | Complete (pure) | The `DRAFT->OPEN->CLOSED->PUBLISHED` table. |
| `bb_transition_state` | UC-1 | Complete | Reads the current state back, then enforces legality before writing. The caller does not supply `from`, so there is no check-then-act gap. |
| `bb_verify_cert` | UC-2 | Seam (placeholder) | Real X.509 verification arrives with PKI; callers treat its verdict as authoritative. |
| `bb_check_eligibility` | UC-2 | Complete (pure) | Scans the election's eligible list. |
| `bb_join` | UC-2 | Complete | Existence, cert verdict, eligibility, then `OPEN` - each with its own refusal code. |
| `bb_record_ballot` | UC-3/4 | Complete | Open-state, eligibility, decrypt, option range and replay gates, then receipt, append, supersede and nonce consume, under the instance write lock. |
| `bb_publish_results` | UC-5 | Complete | A `CLOSED -> PUBLISHED` lifecycle transition, gated by the stored state. |
| `bb_get_results` | UC-5 | Complete | Published-state and observer-eligibility gates run before any tally or hash is read. |
| `bb_lookup_hash` | UC-6 | Complete | Live-rows-only lookup; a superseded and an unissued hash give the same answer. |

## `libballotclient` - client-side logic

Shared by `ballotu` (voter) and `ballotctl` (admin), one artifact.
It reuses the `libballotbrain` model rather than redefining it.
Headers: `include/libballotclient/{client,voter,admin}.h`.
Prefixes: `bcl_*` shared core, `bu_*` voter, `bc_*` admin.

### Core (`client.h`)

- `bcl_ctx` - per-client context (log sink today, the session handle later).
- `bcl_request_t` / `bcl_response_t` - the request/response shapes for every daemon operation (`bcl_op_t`: JOIN, CAST, UPDATE, RESULTS, CHECK, CREATE, OPEN, CLOSE, PUBLISH).
- `bcl_send` - the transport seam (see below).

### Voter (`voter.h`)

- `bu_session_t` - client-local voter session (mirrors `VoterSession`).
- `bu_route_vote` - **complete, pure**: the vote decision-table routing (rules 1/3/5 -> `MUST_JOIN` / `CAST` / `UPDATE`).
- `bu_classify_join` - **complete, pure**: maps a join response to a UC-2 outcome (timeout / not-found / not-eligible / not-open / admitted).
- `bu_classify_check` - **complete, pure**: maps a check response to counted / dropped / unavailable, so a failed lookup is never shown to a voter as a lost ballot.
- `bu_join`, `bu_submit_vote` - **complete**: the session flows. They send through the transport seam and move local session state only on a successful reply.
- `bu_encrypt_ballot`, `bu_derive_receipt` - the client crypto seam (placeholder, see below).

### Admin (`admin.h`)

- `bc_prevalidate_config` - **complete**: client-side pre-validation that delegates to the authoritative `bb_validate_config` (no rule duplication).
- `bc_build_create`, `bc_build_transition` - assemble CREATE and OPEN/CLOSE/PUBLISH requests.

## The three seams

Each seam is one narrow function, and each lives in its own translation unit.
When its backing implementation lands, only that function changes; the logic above it is untouched.
The one-function-per-file split is also what lets a test binary substitute a seam by defining the symbol itself, without any test hook in production code.

### 1. DB seam (`include/libballotbrain/db.h`)

`db_exec(ctx, cmd, out)` takes a **typed, parameterized** `bb_db_cmd_t` - never a raw SQL string, which keeps it type-safe and injection-safe by construction - and today only logs the operation it would run.

- Write ops (`INSERT_ELECTION`, `UPDATE_STATE`, `APPEND_BALLOT`, `MARK_SUPERSEDED`, `NONCE_MARK`) log a SQL-ish line and return `BB_OK`.
- Read ops (`GET_ELECTION`, `GET_TALLY`, `GET_HASHES`, `FIND_HASH`, `NONCE_SEEN`, `GET_PRIOR_BALLOT`) log and return `BB_ERR_NOT_IMPLEMENTED`.

To implement SimpleDB: translate each `bb_db_cmd_t` into a parameterized SQL statement and fill `bb_db_result_t` for the reads.
Secrecy invariant (R2, test U-21): no rendered log line carries the submitting cert, so none of them links a voter to a ballot.
`GET_PRIOR_BALLOT` is the one query keyed by voter identity; it binds the cert as a parameter and renders it as `cert=?`.

### 2. Transport seam (`include/libballotclient/client.h`)

`bcl_send(ctx, req, resp)` sends a request over the secure session.
Stub today: it logs the intended request and returns `BB_ERR_NOT_IMPLEMENTED`.
To implement: wire it to the teammate's `libtetrissh` / `libhtttp` session layer.

### 3. Crypto seam (`include/libballotbrain/crypto.h`, voter half in `voter.h`)

Deterministic, well-formed hex placeholders so the logic can run; not cryptographically meaningful yet.

- Daemon: `bb_decrypt_ballot`, `bb_issue_receipt`.
- Voter: `bu_encrypt_ballot`, `bu_derive_receipt`.

Placeholder wire convention: `payload[0]` carries the chosen option index, so the voter's encrypt stub and the daemon's decrypt stub round-trip an option end to end.
To implement: call the OpenSSL helpers already present in `external/2026-pa2-50005-6767/source/libs/common.c` (RSA-OAEP, AES/HMAC, X.509, SHA-256) once keys are distributed with the transport layer.

## Testing

Every function above is unit tested today, with its collaborators substituted: `make test`.
Two things in the libraries' shape make that possible.
Context handles (`bb_ctx`, `bcl_ctx`) give each case an isolated instance with no hidden file-scope state.
Each seam is one function in one translation unit, so a test binary can define that symbol and the linker leaves the real one out of the archive pull - no dependency injection plumbing, and nothing in production code that exists only for tests.
The single case that a substitute cannot stand in for is the RSA-OAEP round trip itself (U-32), which is the crypto implementation rather than a caller of it.
See [TEST](TEST.md) for the full plan.
