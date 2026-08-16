# Test Inventory

One line per test case, grouped by file. For the test *plan* (decision
tables, traceability to use cases, milestones) see `../TEST.md` - this file
is just "what does `make test` actually run and what does each case check."

Run everything: `make test` (from repo root). Each `tests/unit/*.c` file is
its own Unity binary; `tests/test_*.c` files bring their own harness and
print `ok`/`FAILED` per case.

Not listed here: the fuzz targets under `tests/fuzz/`. They have no case list
by design - each one states a property and lets a fuzzer generate the inputs -
so they are documented in `fuzz/FUZZING.md` and run by `make fuzz-regress`
(seconds, deterministic) and `make fuzz-smoke` (a minute of mutation each).

---

## Unit tests (`tests/unit/`)

Pure logic, seams substituted (`tests/unit/support/fake_*_seams.h`) - no
daemon, no SimpleDB, no network, no sockets.

### `test_admin.c` - libballotclient admin logic (ballotctl)

| Test | Detail |
| --- | --- |
| `test_build_transition_accepts_lifecycle_ops` | OPEN/CLOSE/PUBLISH all build a valid request |
| `test_build_transition_rejects_non_lifecycle_op` | JOIN/CREATE refused with `BB_ERR_ILLEGAL_TRANSITION` |
| `test_prevalidate_matches_brain_validator` | Client pre-check returns the same code as the daemon's validator, every case |
| `test_build_create_valid_config` | Valid config builds a `BCL_CREATE` request |
| `test_build_create_rejects_invalid_with_same_error` | Invalid config refused, no request built |
| `test_fold_eligible_lowercases_every_entry` | Mixed-case eligible names fold to lowercase |
| `test_fold_eligible_dedupes_case_insensitively_keeping_first_order` | `Alice, alice, ALICE, bob, Alice` → `alice, bob` |
| `test_fold_eligible_rejects_illegal_name_and_reports_it` | Illegal entry rejected, offending value reported, list untouched |
| `test_fold_eligible_rejection_with_null_bad_entry_does_not_crash` | `bad_entry` is optional |
| `test_fold_eligible_empty_list_is_a_noop` | Zero entries in, zero out, no error |

### `test_brain_concurrency.c` - `bb_record_ballot` under concurrency (U-22)

| Test | Detail |
| --- | --- |
| `test_U22_concurrent_recording` | 16 threads cast for 16 distinct voters simultaneously → 16 appends, 16 distinct hashes, all v1, no lost write |

### `test_brain_config.c` - `bb_validate_config` (UC-1)

| Test | Detail |
| --- | --- |
| `test_U01_valid_config_accepted` | Title + 2 options + valid window → `BB_OK` |
| `test_U02_option_count_below_boundary` | 1 option, then 0 → `BB_ERR_CONFIG_OPTIONS` |
| `test_U03_empty_title_rejected` | Empty title → `BB_ERR_CONFIG_TITLE` |
| `test_U04_time_window_boundary_rejected` | close = open, then close < open → `BB_ERR_CONFIG_TIME` |
| `test_U05_minimum_valid_window_accepted` | close = open + 1s → `BB_OK` |
| `test_validation_order_title_first` | Title is checked before options/time |

### `test_brain_create.c` - `bb_create_election` (UC-1, postcondition half)

| Test | Detail |
| --- | --- |
| `test_U01_valid_config_creates_draft_election` | One `INSERT_ELECTION`, id returned |
| `test_U05_minimum_window_creates_election` | Minimum valid window still creates |
| `test_U02_U03_U04_invalid_config_creates_nothing` | No write reaches the store for any invalid config |
| `test_desired_id_used_when_free` | An admin-supplied id is honoured if unused |
| `test_desired_id_refused_when_taken` | `BB_ERR_CONFIG_ID_TAKEN` if the id already exists |
| `test_store_failure_is_propagated` | A store error surfaces as itself, not swallowed |

### `test_brain_eligibility.c` - `bb_check_eligibility` (UC-2, pure scan)

| Test | Detail |
| --- | --- |
| `test_U11_unlisted_cert_not_eligible` | Cert not on the list → not eligible |
| `test_U13_listed_cert_eligible` | Cert on the list → eligible |
| `test_eligibility_partition_table` | Full partition sweep (empty list, one entry, many, not found) |
| `test_empty_eligible_list_admits_nobody` | Zero-length list rejects every cert |

### `test_brain_join.c` - `bb_join` (UC-2, admission decision)

| Test | Detail |
| --- | --- |
| `test_U09_election_not_found` | Unknown id → `BB_ERR_NOT_FOUND`, no session |
| `test_U10_election_not_open` | DRAFT/CLOSED/PUBLISHED all refused as not open |
| `test_U11_unlisted_cert_refused` | Valid but unlisted cert → `BB_ERR_NOT_ELIGIBLE` |
| `test_U12_invalid_or_expired_cert_refused` | EXPIRED/INVALID/NOT_ELIGIBLE cert statuses each refused distinctly |
| `test_U13_eligible_voter_admitted` | Eligible + OPEN → `BB_OK`, config returned |
| `test_join_reports_prior_ballot_from_store` | `GET_PRIOR_BALLOT` result surfaced via the optional out-params, only on admission |
| `test_cert_is_checked_before_state` | A bad cert is refused before election state is even considered |
| `test_store_failure_is_propagated` | `GET_ELECTION` failure surfaces as itself |
| `test_store_failure_in_prior_ballot_lookup_is_propagated` | A failure in the prior-ballot lookup also surfaces as itself |

### `test_brain_lifecycle.c` - election state transitions

| Test | Detail |
| --- | --- |
| `test_U06_legal_transition_chain` | DRAFT→OPEN→CLOSED, one `UPDATE_STATE` per step |
| `test_U07_illegal_transitions_rejected` | PUBLISHED→OPEN, DRAFT→CLOSED, etc. all refused, no write |
| `test_current_state_comes_from_the_store` | Decision reads the stored state, not a caller-supplied one |
| `test_unknown_election_not_found` | Unknown id → not found |
| `test_published_is_terminal` | Nothing transitions out of PUBLISHED |
| `test_no_self_transitions` | A state cannot transition to itself |

### `test_brain_lookup.c` - `bb_lookup_hash` (UC-6)

| Test | Detail |
| --- | --- |
| `test_U29_counted_hash_found` | Live published hash → found, choice returned |
| `test_U30_superseded_hash_excluded` | Superseded version → not found |
| `test_U31_unknown_hash_not_found` | Never-issued hash → identical not-found answer to U-30 |
| `test_works_regardless_of_election_state` | Unlike results, lookup is not gated on PUBLISHED |
| `test_unknown_election_not_found` | Unknown election id → not found |
| `test_store_failure_is_not_a_miss` | A store error is never reported as "hash not found" |

### `test_brain_record.c` - `bb_record_ballot` (UC-3/UC-4)

| Test | Detail |
| --- | --- |
| `test_U14_fresh_nonce_accepted` | Unused nonce → `BB_OK`, receipt issued |
| `test_U15_replayed_nonce_rejected` | Seen nonce → `BB_ERR_REPLAY` |
| `test_U16_option_index_boundaries` | -1 and out-of-range rejected; 0 and last valid index accepted |
| `test_U17_malformed_ballot_rejected` | Decrypt failure → store untouched, no receipt |
| `test_U18_ineligible_ballot_rejected_at_record` | Cert not eligible → ciphertext never opened |
| `test_U19_submit_after_close_rejected` | CLOSED election refuses cast and update alike |
| `test_U20_no_double_vote` | Second distinct ballot lands as v2, not a duplicate v1 |
| `test_U23_supersede_on_update` | Existing v1 is marked superseded on update |
| `test_U24_repeated_updates_latest_counts` | v1→v2→v3, each prior version superseded in turn |
| `test_unknown_election_rejected` | Unknown election id → not found |

### `test_brain_results.c` - `bb_publish_results` / `bb_get_results` (UC-5)

| Test | Detail |
| --- | --- |
| `test_U08_publish_requires_closed` | Only CLOSED can publish; write is `PUBLISHED` |
| `test_U25_tally_counts_live_ballots_only` | Superseded rows never reach the published view |
| `test_U26_results_gated_before_publish` | OPEN/CLOSED/DRAFT all refuse results, no data read |
| `test_U27_ineligible_observer_refused` | Non-eligible cert gets no tally or hash data |
| `test_U28_zero_ballot_publish` | Zero ballots still publishes: all-zero tally, not an error |
| `test_results_carry_the_election_title` | `fetch_results` fills `out.title` from the loaded election |
| `test_unknown_election_not_found` | Unknown id → not found, for publish and results alike |

### `test_brain_secrecy.c` - no ballot-to-voter link in logs (R2)

| Test | Detail |
| --- | --- |
| `test_U21_no_cert_appears_in_any_log_line` | A distinctive cert never appears in a rendered log line; the hash does |
| `test_U21_prior_ballot_query_binds_the_cert` | The prior-ballot query carries the cert as a bound parameter, not rendered |

### `test_codec.c` - HTTTP wire codec (`libballotclient/codec.c`)

| Test | Detail |
| --- | --- |
| `test_create_request_roundtrip` | CREATE request survives encode→parse→decode |
| `test_lifecycle_requests_roundtrip` | OPEN/CLOSE/PUBLISH requests round-trip |
| `test_cast_request_roundtrip` | CAST request (with ballot payload) round-trips |
| `test_check_request_roundtrip` | CHECK request round-trips |
| `test_create_response_roundtrip_ok` | CREATE response carries the new election id |
| `test_create_response_roundtrip_error_has_no_election` | A failed CREATE carries no election data |
| `test_join_response_roundtrip` | JOIN response (election + options) round-trips |
| `test_cast_response_roundtrip` | CAST/UPDATE response (receipt) round-trips |
| `test_results_response_roundtrip` | RESULTS response (title, id, tally, hashes) round-trips |
| `test_check_response_roundtrip_found` | CHECK found-case round-trips |
| `test_check_response_roundtrip_not_found` | CHECK not-found-case round-trips |
| `test_check_found_option_does_not_leak_into_election_options` | CHECK's `found_option` key never collides with JOIN's `option` key |
| `test_decode_request_rejects_unknown_method` | Unknown HTTP method → decode failure |
| `test_decode_request_rejects_missing_election_id` | Missing required field → decode failure |
| `test_decode_request_rejects_cast_without_payload` | CAST with no ballot payload → decode failure |
| `test_decode_request_rejects_bad_hex_payload` | Malformed hex ciphertext → decode failure |
| `test_decode_response_rejects_missing_status` | Response with no `status=` line → decode failure |
| `test_decode_response_rejects_unrecognised_status_name` | Unknown status name → decode failure |
| `test_http_status_bucket_is_always_a_known_reason` | Every `bb_result_t` maps to a real HTTP status bucket |

### `test_voter.c` - pure voter decision functions (ballotu)

| Test | Detail |
| --- | --- |
| `test_U37_vote_before_join_blocked` | Voting unjoined → `BU_MUST_JOIN` |
| `test_U38_cast_flow_selected` | Joined, no ballot → cast |
| `test_U39_update_flow_selected` | Joined, has ballot → update |
| `test_classify_join_partition_table` | Full `bu_classify_join` partition sweep |
| `test_U40_dropped_ballot_flagged` | Hash not found → dropped-ballot classification |
| `test_classify_check_partition_table` | Full `bu_classify_check` partition sweep |
| `test_U33_receipt_kdf_deterministic` | Same secret key → same receipt hash, twice |
| `test_U34_distinct_keys_distinct_hashes` | Different keys → different hashes, no collision |

### `test_voter_flow.c` - voter session flows: `bu_join` / `bu_submit_vote`

| Test | Detail |
| --- | --- |
| `test_U35_join_timeout_creates_no_session` | Transport failure → `BU_JOIN_TIMEOUT`, no session, client still usable |
| `test_join_survives_cert_name_aliasing_session` | `bu_join`'s cert_name arg may alias the session field it clears |
| `test_U36_not_open_election_recorded_locally` | Non-open election remembered locally, voter not joined |
| `test_join_refusals_leave_no_session` | NOT_FOUND/NOT_ELIGIBLE/CERT_EXPIRED all leave no session |
| `test_admission_resets_the_session` | A fresh JOIN clears any stale ballot state from a prior election |
| `test_join_reports_prior_ballot_into_session` | `has_prior_ballot`/`prior_ballot_version` from the response land in the session |
| `test_U37_vote_before_join_sends_nothing` | Nothing encrypted or sent before joining |
| `test_U38_cast_flow_records_receipt` | Cast sends `CAST`, receipt lands in the session |
| `test_U39_update_flow_advances_version` | Update sends `UPDATE`, version increments, hash replaced |
| `test_refused_submission_does_not_move_the_session` | A refused submit leaves the session exactly as it was |
| `test_encrypt_failure_sends_nothing` | Encryption failure stops the submission before the transport |

---

## Integration tests (`tests/`)

Real `ballotd`, real SimpleDB, real client library - the closest thing this
project has to end-to-end. Cases marked "(live store)" need the shared
SocketRunner (`java` + `db/dist/simpledb.jar`); everything else runs
against a deliberately-unreachable store to check clean failure.

### `test_ballotd.c` - the real daemon, both channels

| Test | Detail |
| --- | --- |
| `ctl channel rejects voter op (JOIN)` | Admin socket refuses a voter-shaped op |
| `ctl CREATE invalid config refused` | Real validation error over the real admin socket |
| `ctl CREATE with no reachable store fails cleanly` | Unreachable DB → clean `BB_ERR_DB`, no hang |
| `ctl malformed HTTTP gets 400` | Garbage bytes on the admin socket → 400, connection survives |
| `ctl oversized frame closed, no reply` | Over-limit frame closes the connection, no reply sent |
| `SIGTERM shutdown while idle` | Clean exit with no connections open |
| `SIGTERM shutdown with a worker attached` | Clean exit with a voter session mid-flight |
| `voter handshake + wrong-channel CREATE rejected (live store)` | tetrissh handshake succeeds; CREATE over the voter channel is refused |
| `ctl CREATE succeeds (live store)` | Real election row written |
| `two CREATEs share one admin thread (live store)` | Concurrent admin requests serialize correctly |
| `CREATE then JOIN round trips (live store)` | Full admin-create → voter-join path, real auth, real DB |

### `test_client_transport.c` - the real client library (`bcl_connect`/`bcl_send`)

| Test | Detail |
| --- | --- |
| `bcl_connect succeeds against a real ballotd` | tetrissh handshake completes |
| `bcl_connect fails cleanly against a closed port` | No daemon listening → clean `BB_ERR_DB` |
| `bcl_send admin op fails cleanly with no ctl_path set` | Admin op with no ctl path configured, not silently routed elsewhere |
| `bcl_send(CREATE) via ctl: invalid config refused` | Pure validation path, no DB dependency |
| `bcl_send after bcl_disconnect fails cleanly` | Send on a torn-down connection fails, doesn't crash |
| `bcl_send(CREATE) via ctl succeeds (live store)` | Real election created via the real admin path |
| `bu_join admits an eligible voter (live store)` | Full real-auth JOIN, `BU_JOIN_ADMITTED` |
| `rejoin after cast reports prior ballot (live store)` | Cast, disconnect, fresh session, re-login, JOIN again → prior ballot reported, routes to update |
| `ADMIN_RESULTS includes the election title (live store)` | Real create→open→close→publish→results, title present on the wire |

### `test_system_e2e.c` - strict path-complete system suite

This executable requires Java, `db/dist/simpledb.jar`, and all BallotBox runtime executables.
It fails instead of skipping when a dependency is unavailable.

Each named case creates a private temporary root, SimpleDB files, JVM runner, account store, JWT secret, daemon, control socket, TCP port, log, and client processes.
The case tears down every process and file after either success or failure.
Run one path independently with `bin/test_system_e2e CASE-ID`.

| Use case | Independently named cases |
| --- | --- |
| UC-1 | `UC1-MAIN`, `UC1-2A-TITLE`, `UC1-2A-OPTIONS-ZERO`, `UC1-2A-OPTIONS-ONE`, `UC1-2A-TIME-EQUAL`, `UC1-2A-TIME-REVERSED`, `UC1-ID-TAKEN` |
| UC-2 | `UC2-MAIN`, `UC2-4A-DRAFT`, `UC2-4A-CLOSED`, `UC2-4A-PUBLISHED`, `UC2-2A`, `UC2-2B`, `UC2-3A` |
| UC-3 | `UC3-MAIN`, `UC3-1B`, `UC3-1A`, `UC3-4A`, `UC3-LOG` |
| UC-4 | `UC4-MAIN`, `UC4-1B`, `UC4-1A`, `UC4-4A`, `UC4-RECONNECT` |
| UC-5 | `UC5-MAIN`, `UC5-2A`, `UC5-3A-DRAFT`, `UC5-3A-OPEN`, `UC5-3A-CLOSED` |
| UC-6 | `UC6-MAIN`, `UC6-4A` |
| UC-7 | `UC7-MAIN`, `UC7-ERR-DRAFT`, `UC7-SELF-CLOSED`, `UC7-ERR-PUBLISHED` |
| UC-8 | `UC8-MAIN`, `UC8-ERR-DRAFT`, `UC8-ERR-OPEN`, `UC8-SELF-PUBLISHED` |
| Shared lifecycle | `LC-DRAFT-OPEN`, `LC-SELF-OPEN`, `LC-ERR-CLOSED-OPEN`, `LC-ERR-PUBLISHED-OPEN` |
| Full journey | `JOURNEY` |

The UC-3 and UC-4 close-race cases block the submitting thread after encryption and before transport.
The admin closes the election through the live control socket, then the test releases the request and verifies rejection and unchanged persistence.
The lifecycle cases restart `ballotd` and recreate the admin client after legal transitions to prove that state survives both reconnections.
The logging case requires the successful receipt in the real daemon log and rejects any line that links the voter name to an option.

---

## Infrastructure tests (`tests/`)

Shared components ported from tetriSH (`libtetrisdb`, `tetrislogd`,
`libtetrisauth`, `bin/tetrisdb`).
No seams - these test the seam
*implementations* themselves, spawning real processes (a JVM for the D/auth
runner cases, the real `bin/tetrislogd`/`bin/tetrisdb`). D/auth-runner cases
need `java` + the built jar and skip visibly without them.

### `test_db.c` - `libtetrisdb` (table creation, quoting, socket protocol)

| Test | Detail |
| --- | --- |
| `creates catalog entry and one-page data file` | Fresh table is exactly one 4096-byte page |
| `second create leaves existing table alone` | `db_ensure_table` is idempotent, no truncation |
| `refuses to run without a data directory` | No dir configured → clean refusal |
| `quotes plain text` | `db_quote` wraps a plain string |
| `doubles embedded quotes` | `it's` → `'it''s'` |
| `keeps an injection payload inside one literal` | SQL injection payload stays one closed literal |
| `truncates without breaking the literal` | Worst-case quote doubling still fits the buffer, still closed |
| `counts rows and splits fields of a select reply` | Row/field parsing of a select reply |
| `tells no rows from no result table` | Empty result vs. "not a select" are distinguishable |
| `does not mistake the trailer for a row` | The "N rows." trailer line isn't parsed as data |
| `reports a row with more fields than expected` | Schema mismatch is reported, not silently truncated |
| `refuses to start a runner it cannot start` | Bad jar/java path → clean refusal |
| `gives up waiting for a runner that is not coming` | Startup deadline is honoured |
| `fails at once when no runner is listening` | No retry storm against a dead socket |
| `gives up on a connection that is never greeted` | Missing `<<READY>>` → clean timeout |
| `keeps returning DB_TIMEOUT once the deadline has passed` | Deadline is sticky, not re-armed per call |
| `reports a deadlock abort as DB_RETRY, not an error` | Deadlock victim is a distinct, retryable outcome |
| `returns the body of a rejected statement` | Error body is preserved for the caller |
| `reports a runner that hangs up as DB_IO` | Peer disconnect mid-exchange is its own status |
| `refuses a statement carrying a newline` | Multi-statement injection via embedded newline is refused |
| `writes rows and reads them back after restart` | Data survives a clean runner restart |
| `counts a rejected statement and keeps going` | One bad statement doesn't kill the connection |
| `reads rows over a socket and survives a bad statement` | Real SocketRunner round trip, real jar |
| `commits and rolls back across statements` | Multi-statement transaction semantics, real runner |

### `test_logd.c` - `tetrislogd`

| Test | Detail |
| --- | --- |
| `basic delivery` | DEBUG/INFO/WARN/ERROR all delivered, args preserved |
| `level filter` | Records below `-l` threshold are dropped and counted |
| `malformed + log injection` | Hostile datagrams (newline, ANSI escape, oversized) can't forge a line |
| `SIGHUP rotation` | `SIGHUP` reopens the log path after a rename |
| `sender survives restart` | Sender reconnects transparently after the daemon restarts |
| `burst never blocks sender` | 20000 records while the daemon is `SIGSTOP`'d never blocks the caller |
| `refuses non-socket path` | Daemon refuses to bind over an existing regular file |

### `test_rc.c` - `.tetrishrc` configuration contract

| Test | Detail |
| --- | --- |
| `db table and defaults` | `db_*` keys and their defaults |
| `db loads a valid file` | Real file parses into the expected struct |
| `auth table and valid file` | `auth_*` keys and defaults |
| `log table, defaults, and valid file` | `log_*` keys and defaults |
| `sample matches exported surface` | The shipped sample rc covers every documented key |
| `invalid values and unknown owned keys` | Bad values and unknown keys in an owned table are refused |
| `socket path bounds` | Overlong socket paths are rejected, not truncated silently |
| `missing files are distinct` | A missing file is distinguishable from an empty one |
| `invalid log config refuses startup` | A bad `log_*` value stops the daemon from starting |

### `test_tetrisdb.c` - `bin/tetrisdb` CLI

| Test | Detail |
| --- | --- |
| `start provisions everything before the runner binds` | Tables/secret exist before the socket is live |
| `start, check, stop, check, and restart through the CLI` | Full lifecycle round trip |
| `a duplicate start preserves the live runner` | Second `start` against a live one is a no-op, not a restart |
| `invalid configuration changes no startup state` | Bad config → no partial state left behind |
| `a log-table collision stops before provisioning` | Conflicting schema caught before any table is touched |
| `a missing jar refuses before the child is forked` | Checked before `fork`/`exec`, not after |
| `start recovers a semaphore left wedged at zero` | A stuck semaphore from a prior crash is repaired |
| `a loose secret refuses before socket unlink and stop` | Bad permissions on the JWT secret stop startup early |
| `a child startup failure is reported and releases the lock` | Failed child doesn't leave the lock held |
| `a malformed live lockfile never supplies a pid to signal` | Garbage lockfile content can't be used to signal an arbitrary pid |
| `check is read-only and rejects a missing secret` | `check` never mutates state |
| `check rejects invalid configuration without changing state` | Same, for bad config |
| `an unlocked stale lockfile never supplies a pid to signal` | Same pid-safety property, unlocked case |
| `a killed runner can still be stopped and restarted` | Recovery after an unclean death |
| `start creates the socket and stderr directories it owns` | Parent directories created as needed |
| `an unreadable lockfile reports a permission failure` | Permission error surfaced, not misread as "no runner" |
| `a shutdown timeout never escalates to SIGKILL` | `stop` respects the graceful-shutdown contract |
| `runner: the real jar starts through bin/tetrisdb start` | Real JVM, real jar, skips without them |

### `test_jwt.c` - JWT mint/verify (`libtetrisauth/jwt.c`)

| Test | Detail |
| --- | --- |
| `mints and verifies its own token` | Round trip with a real secret |
| `matches the known-answer vector` | Output matches a fixed known-good token |
| `round trips every allowed name length` | Every legal username length mints and verifies |
| `rejects a flipped byte in each segment` | Single-bit corruption anywhere invalidates the token |
| `rejects a token signed under another secret` | Wrong key → verification fails |
| `rejects a signature that is not 32 bytes` | Malformed signature length rejected |
| `rejects malformed segment shapes` | Structurally broken tokens rejected |
| `rejects alg:none carrying a valid HS256 signature` | Algorithm-confusion attack blocked |
| `rejects other algs and a missing alg` | Only HS256 is accepted |
| `rejects an unknown header parameter` | Strict header shape enforced |
| `ignores an unknown claim` | Forward-compatible: extra claims don't break verification |
| `expires on or after exp, with no leeway` | Expiry is exact, no grace window |
| `treats a missing exp as a failure, not as eternal` | No `exp` claim ≠ never expires |
| `rejects a payload re-serialized with reordered keys` | Signature covers exact bytes, not semantic JSON |
| `rejects malformed and out-of-range claims` | Bad `sub`/`iat`/`exp` values rejected |
| `refuses to mint a name outside #47's allowlist` | Minting enforces the same username charset |
| `refuses to mint an unterminated name` | No NUL-termination in the name → refused |
| `refuses a secret shorter than the hash output` | Weak secret length rejected at mint time |
| `refuses an output buffer too small` | Undersized output buffer is a clean refusal, not an overflow |

### `test_auth.c` - `libtetrisauth` entry point (`auth_login`/`auth_offer`)

| Test | Detail |
| --- | --- |
| `no socket: absent secret -> 500` | Missing JWT secret file → 500, no crash |
| `no socket: 0644 secret -> 500` | World-readable secret refused |
| `no socket: 31-byte secret -> 500` | Undersized secret refused |
| `no socket: 65-byte secret -> 500` | Oversized secret refused |
| `no socket: a FIFO is refused on S_ISREG, not read` | Non-regular-file secret refused before it's ever read |
| `no socket: 32-byte 0600 secret loads` | The one accepted shape actually loads |
| `no socket: chmod 0600 recovers in the same process, no restart` | Fixing permissions live is picked up without a restart |
| `provision: creates a usable 32-byte 0600 secret` | Fresh provisioning produces a loadable secret |
| `provision: twice in a row does not rotate the key` | Idempotent: existing secret is never replaced |
| `provision: never repairs a loose existing secret` | A bad existing secret is reported, not silently fixed |
| `wire: GUEST succeeds with the database provably absent` | GUEST needs no DB at all |
| `wire: a client that hangs up is a drop` | Mid-exchange disconnect handled cleanly |
| `wire: an unparseable frame is 400 and the exchange carries on` | One bad frame doesn't kill the session |
| `wire: a pre-auth 401 does not spend an attempt` | Failed LOGIN before budget applies doesn't count against the cap |
| `wire: malformed credentials are 400 before any connection` | Body validation happens before any DB round trip |
| `wire: a legal 15-character name is not refused` | Boundary-length username accepted |
| `wire: a runner that never greets is a 500, not a hang` | Store connect timeout → clean 500 |
| `wire: no .tetrishrc fails LOGIN and leaves GUEST alone` | Missing config degrades LOGIN/REGISTER only, not GUEST |
| `wire: every auth method after login is 409, and the body is scrubbed` | Post-auth LOGIN/REGISTER attempts are refused, credentials still scrubbed |
| `runner: REGISTER mints a token the secret verifies` | Real SocketRunner, real token, real verification |
| `runner: usernames are folded to lowercase` | `Kenji` stores and matches as `kenji` |
| `runner: a duplicate registration is 409 and writes nothing` | No partial row on a taken username |
| `runner: the right password is 200` | Real PBKDF2 check succeeds |
| `runner: 401 and 404 both count, and the cap drops the connection` | Failed attempts count toward the budget and the cap is enforced |
| `runner: a held registration semaphore is a 500, not a hang` | Contention degrades to a clean error, not a stall |
| `runner: four-process registration sweep over a growing table` | Concurrent registrations against a real, growing table |
