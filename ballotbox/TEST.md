# BallotBox Test Plan

## Unit Test Cases

### Approach

A unit test here exercises exactly one function.
Everything that function calls out to - the store, the crypto and PKI seams, the network transport - is replaced by a programmable substitute, so the case states the answers those collaborators give and then asserts the function's return value and the commands it issued.
No unit test needs SimpleDB, keys, or a socket, and none of them is scheduled behind those milestones.

The substitutes live in `tests/unit/support/fake_brain_seams.h` (store + daemon crypto/PKI) and `tests/unit/support/fake_client_seams.h` (transport + ballot crypto).
Because the libraries are static archives, a test that defines a seam symbol keeps the real implementation out of its binary, so no production code carries test hooks.
Each test file is its own binary; `make test` builds and runs them all.

Postconditions such as "no election created" or "store unchanged" are asserted as the exact commands that reached the store, since those commands are what a real store would act on.

### Decision table: vote command (UC-3 / UC-4)

| ~                                        | 1   | 2   | 3   | 4   | 5   |
| ---------------------------------------- | --- | --- | --- | --- | --- |
| joined                                   | N   | Y   | Y   | Y   | Y   |
| has prior ballot                         | -   | N   | N   | Y   | Y   |
| election open                            | -   | N   | Y   | N   | Y   |
| **actions**                              |     |     |     |     |     |
| must join first                          | X   |     |     |     |     |
| rejected (closed)                        |     | X   |     | X   |     |
| cast (UC-3), receipt                     |     |     | X   |     |     |
| update (UC-4), supersede + fresh receipt |     |     |     |     | X   |

### ballotd unit tests

| ID   | UC     | Technique     | Unit under test                                  | Purpose                                    | Input / Setup                                                                                       | Expected Output                                                                        | Postcondition                                              | Status | Test file                                |
| ---- | ------ | ------------- | ------------------------------------------------ | ------------------------------------------ | --------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- | ---------------------------------------------------------- | ------ | ---------------------------------------- |
| U-01 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election`      | Valid config accepted                      | Title, 2 options (min valid), close = open + 1h                                                     | `BB_OK`                                                                                | One `INSERT_ELECTION`, id returned                         | Done   | `test_brain_config`, `test_brain_create` |
| U-02 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election`      | Option count below boundary                | Config with 1 option, then 0 options                                                                | `BB_ERR_CONFIG_OPTIONS`                                                                | No write reaches the store                                 | Done   | `test_brain_config`, `test_brain_create` |
| U-03 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election`      | Empty title rejected                       | Config with empty title                                                                             | `BB_ERR_CONFIG_TITLE`                                                                  | No write reaches the store                                 | Done   | `test_brain_config`, `test_brain_create` |
| U-04 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election`      | Time window boundary                       | close = open, then close = open - 1s                                                                | `BB_ERR_CONFIG_TIME`                                                                   | No write reaches the store                                 | Done   | `test_brain_config`, `test_brain_create` |
| U-05 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election`      | Minimum valid time window                  | close = open + 1s                                                                                   | `BB_OK`                                                                                | One `INSERT_ELECTION`                                      | Done   | `test_brain_config`, `test_brain_create` |
| U-06 | UC-1   | -             | `bb_transition_state`                            | Legal transition chain                     | Stored election in DRAFT, then OPEN, then CLOSED; transition to the next state each time            | Each transition succeeds                                                               | One `UPDATE_STATE` per step, carrying the target           | Done   | `test_brain_lifecycle`                   |
| U-07 | -      | ECT           | `bb_is_legal_transition` / `bb_transition_state` | Illegal transitions rejected               | One representative per illegal pair: PUBLISHED→OPEN, DRAFT→CLOSED, OPEN→DRAFT, CLOSED→OPEN          | `BB_ERR_ILLEGAL_TRANSITION`                                                            | No write in any case                                       | Done   | `test_brain_lifecycle`                   |
| U-08 | UC-5   | -             | `bb_publish_results`                             | Publish requires CLOSED                    | Stored election OPEN, DRAFT, PUBLISHED (one case each)                                              | Rejected                                                                               | No write; from CLOSED it writes `PUBLISHED`                | Done   | `test_brain_results`                     |
| U-09 | UC-2   | ECT           | `bb_join`                                        | Join: election not found                   | Store reports no such election                                                                      | `BB_ERR_NOT_FOUND`                                                                     | No session created (no write)                              | Done   | `test_brain_join`                        |
| U-10 | UC-2   | ECT           | `bb_join`                                        | Join: election not open                    | Eligible cert, election in DRAFT, CLOSED, PUBLISHED (one case each)                                 | `BB_ERR_NOT_OPEN`                                                                      | No session created                                         | Done   | `test_brain_join`                        |
| U-11 | UC-2   | ECT           | `bb_join`                                        | Join: unlisted cert refused                | Valid cert not on eligible list, election OPEN                                                      | `BB_ERR_NOT_ELIGIBLE`                                                                  | No session created, no config returned                     | Done   | `test_brain_join`                        |
| U-12 | UC-2   | ECT           | `bb_join`                                        | Join: invalid or expired cert refused      | Cert seam returns `EXPIRED`, then `INVALID`, then `NOT_ELIGIBLE`                                    | The matching refusal code                                                              | No session created                                         | Done   | `test_brain_join`                        |
| U-13 | UC-2   | ECT           | `bb_join`                                        | Join: eligible voter admitted              | Cert on eligible list, election OPEN                                                                | `BB_OK`, election config returned                                                      | Voter admitted, can cast                                   | Done   | `test_brain_join`                        |
| U-14 | UC-3   | -             | `bb_record_ballot`                               | Fresh nonce accepted                       | Ballot with unused nonce from an eligible voter                                                     | `BB_OK`, receipt issued                                                                | Hash row appended v1, nonce marked used                    | Done   | `test_brain_record`                      |
| U-15 | UC-3   | -             | `bb_record_ballot`                               | Replayed nonce rejected                    | Same ballot submitted twice, store reports the nonce as seen                                        | `BB_ERR_REPLAY`                                                                        | Exactly one ballot appended                                | Done   | `test_brain_record`                      |
| U-16 | UC-3   | BVT           | `bb_record_ballot`                               | Option index boundaries                    | 3 options; decrypted index -1, 0, 2, 3                                                              | -1 and 3 rejected; 0 and 2 accepted                                                    | Only valid ballots appended                                | Done   | `test_brain_record`                      |
| U-17 | UC-3   | -             | `bb_record_ballot`                               | Malformed ballot rejected                  | Decrypt seam reports `BB_ERR_DECRYPT`                                                               | `BB_ERR_DECRYPT`                                                                       | Store unchanged, nonce not consumed, no receipt            | Done   | `test_brain_record`                      |
| U-18 | UC-3   | -             | `bb_record_ballot`                               | Ineligible ballot rejected at record       | Well-formed ballot whose cert is not on the eligible list                                           | `BB_ERR_NOT_ELIGIBLE`                                                                  | Store unchanged, ciphertext never opened                   | Done   | `test_brain_record`                      |
| U-19 | UC-3/4 | DT rules 2, 4 | `bb_record_ballot`                               | Submit after close rejected                | Election CLOSED; once with no prior ballot, once with prior ballot                                  | `BB_ERR_CLOSED` in both cases                                                          | Store unchanged, prior ballot not superseded               | Done   | `test_brain_record`                      |
| U-20 | UC-3   | -             | `bb_record_ballot`                               | No double vote                             | Two distinct ballots from the same cert                                                             | Second stored as version 2                                                             | v1 superseded, one live ballot for that cert               | Done   | `test_brain_record`                      |
| U-21 | UC-3   | -             | `db_exec`                                        | No ballot-to-voter link in logs            | Every op rendered with a distinctive cert in the command                                            | Cert appears in no log line; hash does                                                 | Secrecy preserved, prior-ballot query binds cert           | Done   | `test_brain_secrecy`                     |
| U-22 | UC-3   | -             | `bb_record_ballot`                               | Concurrency-safe recording                 | 16 threads submit ballots for 16 distinct voters simultaneously                                     | All 16 accepted                                                                        | 16 appends, 16 distinct hashes, all v1                     | Done   | `test_brain_concurrency`                 |
| U-23 | UC-4   | -             | `bb_record_ballot`                               | Supersede on update                        | Store reports a v1 ballot for this voter                                                            | Fresh receipt hash issued, appended as v2                                              | v1 marked superseded                                       | Done   | `test_brain_record`                      |
| U-24 | UC-4   | -             | `bb_record_ballot`                               | Repeated updates, latest counts            | Voter submits v1, v2, v3                                                                            | Fresh distinct receipt each time                                                       | v1 and v2 superseded in turn, v3 live                      | Done   | `test_brain_record`                      |
| U-25 | UC-5   | -             | `bb_get_results`                                 | Published view counts live only            | Published election, store's live set is 3 rows                                                      | Tally sums to 3                                                                        | No superseded row in the view                              | Done   | `test_brain_results`                     |
| U-26 | UC-5   | -             | `bb_get_results`                                 | Results gated before publish               | Results request while OPEN, then CLOSED, then DRAFT                                                 | `BB_ERR_NOT_PUBLISHED` in every case                                                   | Tally and hashes never read from the store                 | Done   | `test_brain_results`                     |
| U-27 | UC-5   | -             | `bb_get_results`                                 | Ineligible observer refused                | Results request from a cert outside the observer set                                                | `BB_ERR_NOT_ELIGIBLE`                                                                  | No tally or hash data returned                             | Done   | `test_brain_results`                     |
| U-28 | UC-5   | BVT           | `bb_publish_results` / `bb_get_results`          | Zero-ballot publish                        | Election with 0 ballots, close + publish                                                            | Publish succeeds                                                                       | All-zero tally, empty hash list                            | Done   | `test_brain_results`                     |
| U-29 | UC-6   | ECT           | `bb_lookup_hash`                                 | Lookup: counted hash found                 | Hash of a live published ballot                                                                     | `BB_OK`, row with the choice returned                                                  | Query carried the voter's hash                             | Done   | `test_brain_lookup`                      |
| U-30 | UC-6   | ECT           | `bb_lookup_hash`                                 | Lookup: superseded hash excluded           | Hash of a superseded ballot version (not in the live set)                                           | `BB_ERR_NOT_FOUND`                                                                     | Dropped-ballot path triggered client side                  | Done   | `test_brain_lookup`                      |
| U-31 | UC-6   | ECT           | `bb_lookup_hash`                                 | Lookup: unknown hash not found             | Random hash never issued                                                                            | `BB_ERR_NOT_FOUND`, identical to U-30's answer                                         | Caller's buffer untouched, nothing leaked                  | Done   | `test_brain_lookup`                      |
| U-41 | UC-2   | -             | `bb_join`                                        | Join reports a prior ballot from the store | `out_has_ballot`/`out_ballot_version` requested; store's `GET_PRIOR_BALLOT` answers found/not found | Both out-params filled from the store's answer; refused joins skip the lookup entirely | Query issued only on `BB_OK` admission, never on a refusal | Done   | `test_brain_join`                        |

### ballotu unit tests

| ID   | UC   | Technique | Unit under test                           | Purpose                                 | Input / Setup                                                         | Expected Output                                    | Postcondition                                           | Status   | Test file                       |
| ---- | ---- | --------- | ----------------------------------------- | --------------------------------------- | --------------------------------------------------------------------- | -------------------------------------------------- | ------------------------------------------------------- | -------- | ------------------------------- |
| U-32 | UC-3 | -         | `bu_encrypt_ballot`                       | RSA-OAEP round trip                     | Known selection and election public key                               | Ciphertext decrypts to the selection               | Plaintext never leaves client buffer                    | Deferred | Crypto/PKI                      |
| U-33 | UC-6 | -         | `bu_derive_receipt`                       | Deterministic receipt KDF               | Same secret ballot key twice                                          | Same receipt hash both times                       | -                                                       | Done     | `test_voter`                    |
| U-34 | UC-6 | -         | `bu_derive_receipt`                       | Distinct keys, distinct hashes          | Two different secret keys                                             | Different hashes, no collision on test corpus      | -                                                       | Done     | `test_voter`                    |
| U-35 | UC-2 | ECT       | `bu_join`                                 | Join: connection timeout handled        | Transport seam reports a transport-level failure                      | `BU_JOIN_TIMEOUT`                                  | No session state created, client still usable           | Done     | `test_voter_flow`               |
| U-36 | UC-2 | ECT       | `bu_join`                                 | Join: not-open election handled         | Daemon returns an election with a non-open state                      | `BU_JOIN_NOT_OPEN`                                 | Election recorded locally, voter not joined             | Done     | `test_voter_flow`               |
| U-37 | UC-3 | DT rule 1 | `bu_route_vote` / `bu_submit_vote`        | Vote before join blocked                | `vote` with no joined election in the session                         | `BU_MUST_JOIN` / `BB_ERR_NOT_JOINED`               | Nothing encrypted, nothing sent                         | Done     | `test_voter`, `test_voter_flow` |
| U-38 | UC-3 | DT rule 3 | `bu_route_vote` / `bu_submit_vote`        | Cast flow selected                      | Joined, `has_ballot` false                                            | `CAST` request sent, receipt returned              | `has_ballot` true, `my_hash` stored                     | Done     | `test_voter`, `test_voter_flow` |
| U-39 | UC-4 | DT rule 5 | `bu_route_vote` / `bu_submit_vote`        | Update flow selected                    | Joined, `has_ballot` true                                             | `UPDATE` request sent, fresh receipt               | `ballot_version` incremented                            | Done     | `test_voter`, `test_voter_flow` |
| U-40 | UC-6 | ECT       | `bu_classify_check`                       | Dropped ballot flagged                  | Daemon reports the derived hash as not found                          | `BU_CHECK_DROPPED`                                 | Voter directed to the admin escalation path             | Done     | `test_voter`                    |
| U-42 | UC-2 | -         | `bu_join`                                 | Rejoin picks up a reported prior ballot | Response carries `has_prior_ballot=1`, `prior_ballot_version=3`       | `session.has_ballot=1`, `session.ballot_version=3` | `bu_route_vote` on this session now selects `BU_UPDATE` | Done     | `test_voter_flow`               |
| U-43 | UC-5 | -         | `bb_get_results` / `bb_get_results_admin` | Results carry the election title        | Loaded election has a title (both entry points share `fetch_results`) | `out.title` set from the loaded election           | -                                                       | Done     | `test_brain_results`            |

Guard cases beyond the numbered rows are in the same files: validation ordering, store-failure propagation (a failed lookup is never reported to a voter as a dropped ballot), session reset on re-join, and the client pre-validator agreeing with the daemon's validator (`test_admin`).

### Traceability

Every error state and alternative flow is claimed by at least one test case.

| UC   | Error state / alternative flow (from README)              | Covered by             |
| ---- | --------------------------------------------------------- | ---------------------- |
| UC-1 | Invalid config: no title / no options / bad time window   | U-02, U-03, U-04, I-08 |
| UC-2 | Admin IP/port unreachable (timeout)                       | U-35, I-10             |
| UC-2 | Election ID not found                                     | U-09, I-10             |
| UC-2 | Cert not on eligible list                                 | U-11, U-18, I-10       |
| UC-2 | Cert invalid or expired                                   | U-12                   |
| UC-2 | Election not `OPEN` (added to list, shown not open)       | U-10, U-36, I-10       |
| UC-3 | Not joined → must join first                              | U-37                   |
| UC-3 | Alt 1a: already has final ballot → route to UC-4          | U-39                   |
| UC-3 | Election closed mid-submit → rejected                     | U-19, I-12             |
| UC-4 | Not joined → must join first                              | U-37                   |
| UC-4 | Alt 1a: no prior ballot → route to UC-3                   | U-38                   |
| UC-4 | Election closed mid-submit → rejected                     | U-19, I-12             |
| UC-5 | Observer not eligible → refused                           | U-27                   |
| UC-5 | Election not `PUBLISHED` → results not available          | U-26, I-14             |
| UC-6 | Alt 4a: hash not found → dropped ballot flagged           | U-30, U-31, U-40, I-15 |
| UC-1 | All main, boundary, time, and duplicate-id system paths   | `UC1-E2E`              |
| UC-2 | All documented join outcomes through real services        | `UC2-E2E`              |
| UC-3 | Cast, routing, close race, and secrecy postcondition      | `UC3/4-E2E`, `UC3-LOG` |
| UC-4 | Update, reconnect, superseding, and close race            | `UC3/4-E2E`            |
| UC-5 | Observer/admin success, eligibility, and every state gate | `UC5-E2E`              |
| UC-6 | Pre/post publication, unknown, superseded, and latest key | `UC6-E2E`              |
| UC-7 | Close success and all illegal source states               | `UC7/8-E2E`            |
| UC-8 | Publish success, terminal state, and all illegal sources  | `UC7/8-E2E`            |
| All  | Complete successful election journey                      | `JOURNEY-E2E`          |

---

## Integration Test Cases

### Strategy

```
Admin machine:   ballotctl ─▶ ballotd ─▶ SimpleDB
Voter (remote):  ballotu ──libtetrissh──▶ ballotd ─▶ SimpleDB
```

- Backend, **bottom-up**: `ballotd → SimpleDB`, storage first then handlers.
  The unit cases already pin the logic against the seam contract, so these cases check that the real store honours that contract.
- Frontend, **top-down**: clients against the `mock.c` daemon first, then the real `ballotd`, so a failure after unmocking isolates to the newly integrated component.
- Network: `ballotu → libtetrissh → ballotd` is the only network/encrypted edge, where wire-secrecy (I-11) is checked.

### Backend integration (ballotd + SimpleDB, bottom-up)

Precondition: clean SimpleDB seeded per case, wiped after.

| ID   | Call-graph edge    | UC   | Purpose                           | Input / Setup                        | Expected Output             | Postcondition                                          |
| ---- | ------------------ | ---- | --------------------------------- | ------------------------------------ | --------------------------- | ------------------------------------------------------ |
| I-01 | ballotd → SimpleDB | UC-1 | Election survives restart         | Create election, restart `ballotd`   | Election reloaded           | Config and state identical to before restart           |
| I-02 | ballotd → SimpleDB | UC-1 | Draft to Open persisted           | Create then open an election         | Transition succeeds         | DB row shows `OPEN`, subsequent fetch returns `OPEN`   |
| I-03 | ballotd → SimpleDB | UC-3 | Ballot and hash stored atomically | Record one ballot                    | Receipt matches stored hash | One ballot row, one hash row                           |
| I-04 | ballotd → SimpleDB | UC-4 | Version chain in store            | Cast then update                     | Update succeeds             | v1 `superseded=true`, v2 counted, only v2 in tally     |
| I-05 | ballotd → SimpleDB | UC-3 | Parallel inserts under load       | 50 concurrent casts across 50 voters | All accepted                | 50 ballots stored, no lost writes, no duplicate hashes |
| I-06 | ballotd → SimpleDB | UC-5 | Published view is consistent      | Close then publish                   | Publish succeeds            | DB tally equals count of non-superseded ballots        |

### Frontend integration (clients + ballotd, top-down)

Precondition: real `ballotd` with seeded elections.

| ID   | Call-graph edge                 | UC       | Purpose                                 | Input / Setup                                                                                    | Expected Output                                                                                                               | Postcondition                                                                     |
| ---- | ------------------------------- | -------- | --------------------------------------- | ------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| I-07 | ballotctl → ballotd (local)     | UC-1     | Admin creates and opens                 | Valid config via ballotctl                                                                       | "instance is live" shown                                                                                                      | Daemon state `OPEN`                                                               |
| I-08 | ballotctl → ballotd (local)     | UC-1     | Invalid config surfaced                 | Config with close time before open time                                                          | Specific error shown                                                                                                          | Election remains in `DRAFT` flow                                                  |
| I-09 | ballotu → libtetrissh → ballotd | UC-2     | Eligible voter joins                    | Correct IP/port/ID, eligible cert                                                                | Options displayed, joined confirmed                                                                                           | Voter admitted to session                                                         |
| I-10 | ballotu → libtetrissh → ballotd | UC-2     | All four refusal partitions             | Wrong host; unknown ID; ineligible cert; non-open state                                          | Timeout / not found / not eligible / not open, each distinct                                                                  | No session created in any branch                                                  |
| I-11 | ballotu → libtetrissh → ballotd | UC-3     | Encrypted cast over session             | Joined voter casts a vote                                                                        | Receipt hash displayed                                                                                                        | Wire traffic is ciphertext only                                                   |
| I-12 | ballotu → libtetrissh → ballotd | UC-3/4   | Close race (DT rules 2, 4)              | Voter submits as ballotctl closes the election                                                   | Rejection shown to voter                                                                                                      | No partial ballot stored                                                          |
| I-13 | ballotu → libtetrissh → ballotd | UC-5     | Published tally displayed               | Observer requests published election                                                             | Title, id and counted tally shown (no per-ballot hash listing - each voter already saw their own receipt at cast/update time) | -                                                                                 |
| I-14 | ballotu → libtetrissh → ballotd | UC-5     | Results gated                           | Observer requests `CLOSED` election                                                              | "results not available"                                                                                                       | No tally data leaves the daemon                                                   |
| I-15 | ballotu → libtetrissh → ballotd | UC-6     | Hash lookup both branches               | Key of counted ballot; key of superseded ballot                                                  | Found and counted; not found flagged as dropped                                                                               | Choice revealed only to the key holder                                            |
| I-16 | ballotu → libtetrissh → ballotd | UC-2/3/4 | Rejoin after cast routes to update      | Cast a ballot, disconnect, fresh client + fresh session, log in and JOIN the same election again | Rejoin reports the prior ballot; a subsequent vote is a real `UPDATE`, not a silent overwrite                                 | Prior receipt superseded, new one live, exactly one counted ballot for that voter |
| I-17 | ballotctl → ballotd (local)     | UC-5     | Results wire response carries the title | `ADMIN_RESULTS` after create/open/close/publish                                                  | Response's election id and title match what was created                                                                       | -                                                                                 |

---

## Infrastructure Test Cases

### Approach

These cover the two shared components ported from tetriSH: `libtetrisdb`, the C client that drives SimpleDB through a `PipeRunner` child process, and `tetrislogd`, the logging daemon.
They are not unit tests and deliberately break the rule stated above: they use no seams, and they spawn real processes - a JVM for the D cases, the real `bin/tetrislogd` over a real Unix datagram socket for the L cases.
The point is to test the seam implementations themselves, which is exactly what a substitute cannot stand in for.

`libtetrisdb` is what will sit behind `db_exec`, so the D cases are the prerequisite evidence for the backend integration milestone (I-01..I-06): persistence across restart, injection containment, and error accounting are settled here, before any ballot-shaped case depends on them.

Each file is its own binary with its own harness rather than Unity, and `make test` builds and runs both alongside the unit tests.
The D cases that need a JVM check for `java` and the jar first and skip themselves if either is missing, so the suite still passes on a machine without a JVM - a skip is visible in the output rather than silently counted as a pass.

### libtetrisdb tests

Precondition: scratch directory removed before each case, so every case starts from no catalog and no data file.

| ID   | Technique | Unit under test                      | Purpose                           | Input / Setup                                              | Expected Output                                 | Postcondition                                        | Status | Test file |
| ---- | --------- | ------------------------------------ | --------------------------------- | ---------------------------------------------------------- | ----------------------------------------------- | ---------------------------------------------------- | ------ | --------- |
| D-01 | -         | `db_ensure_table`                    | Table created ready to scan       | Fresh directory, `log` schema                              | Returns 0, catalog line matches the schema      | `log.dat` is exactly one 4096-byte page, not 0 bytes | Done   | `test_db` |
| D-02 | -         | `db_ensure_table`                    | Create is idempotent              | Create, write a byte into `log.dat`, create again          | Second call returns 0                           | One catalog line, file not truncated, byte intact    | Done   | `test_db` |
| D-03 | -         | `db_quote`                           | Plain text wrapped                | `hello`                                                    | `'hello'`                                       | -                                                    | Done   | `test_db` |
| D-04 | -         | `db_quote`                           | Embedded quote doubled            | `it's`                                                     | `'it''s'`                                       | -                                                    | Done   | `test_db` |
| D-05 | -         | `db_quote`                           | Injection stays one literal       | `x'); insert into log values (9);--`                       | Opens and closes with a quote, no odd quote run | Parser can never see a second statement              | Done   | `test_db` |
| D-06 | BVT       | `db_quote`                           | Truncation cannot break a literal | 16 quotes into an 8-byte buffer (worst case, each doubles) | Fits the buffer, still quote-delimited          | No unterminated literal, no overflow                 | Done   | `test_db` |
| D-07 | -         | `db_start` / `db_submit` / `db_stop` | Rows survive restart              | Insert 2 rows, stop, restart with a `max(id)` probe        | `dropped=0`, `errors=0`, probe returns 2        | Clean shutdown flushed both rows to disk             | Done   | `test_db` |
| D-08 | -         | `db_submit`                          | Bad SQL counted, not swallowed    | `insert into nosuchtable ...`, then a valid insert         | `errors=1`, `dropped=0`                         | Connection survives; the later good statement lands  | Done   | `test_db` |

### tetrislogd tests

Precondition: daemon started per case against a scratch socket and log file, stopped and reaped at the end.

| ID   | Technique | Unit under test      | Purpose                           | Input / Setup                                                                                                  | Expected Output                                            | Postcondition                                                       | Status | Test file   |
| ---- | --------- | -------------------- | --------------------------------- | -------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- | ------------------------------------------------------------------- | ------ | ----------- |
| L-01 | -         | `log_send` → sink    | Every level is delivered          | One record at DEBUG, INFO, WARN, ERROR                                                                         | All four written, format args preserved                    | Timestamp prefix well-formed, sender pid recorded                   | Done   | `test_logd` |
| L-02 | ECT       | `min_level` filter   | Records below threshold dropped   | `-l warn`, then one record per level                                                                           | WARN and ERROR kept, DEBUG and INFO absent                 | `filtered=2` in the banner - counted, not lost                      | Done   | `test_logd` |
| L-03 | -         | Datagram validation  | Hostile input cannot forge a line | Undersized and oversized datagrams; unterminated `msg` carrying a newline, a fake log line, and an ANSI escape | Record written with the newline neutered, escape byte gone | `malformed=2`, `truncated=1`; no forged line of its own             | Done   | `test_logd` |
| L-04 | -         | `SIGHUP` handler     | Rotation reopens the path         | Send a record, rename the file aside, `SIGHUP`, send another                                                   | Reopen announced in the fresh file                         | New file has only post-rotate records, old file keeps its own       | Done   | `test_logd` |
| L-05 | -         | Sender reconnect     | Sender outlives the daemon        | Send with no daemon; start it; send; restart it; send again                                                    | First send fails and is counted; both later sends succeed  | Stale connected fd replaced transparently, no re-open by the caller | Done   | `test_logd` |
| L-06 | -         | Send path under load | A full queue never blocks         | `SIGSTOP` the daemon, then send 20000 records, then `SIGCONT`                                                  | Drops counted, every call returns                          | Whatever the kernel accepted still reaches the file                 | Done   | `test_logd` |
| L-07 | -         | `sink_bind`          | Refuses to clobber a regular file | Socket path pointing at an existing file holding `precious`                                                    | Daemon exits 1 with a refusal message                      | File contents untouched                                             | Done   | `test_logd` |

---

## Robustness Test Cases

### Approach

Every case above states an input and the answer it expects.
The cases here state no input at all: they name a property the code must hold on **any** input, and a fuzzer generates the inputs - millions of them, guided by which branches each one reaches.
That difference is the point.
A unit case can only assert what someone thought to write down, and the inputs that break a parser are by definition the ones nobody thought of.

The targets live in `tests/fuzz/`, one per parser, each pairing a code path with the properties its header promises.
Full detail - how to run a campaign, why the build needs two compilers, what to do with a crash - is in `tests/fuzz/FUZZING.md`; this section is the case list and the milestones.

Ranked by how much untrusted input reaches the code and how much fixed-size copying it does with it:

| Case | Target                | Code under test                                | Property asserted beyond "does not crash"                                                                                                                  |
| ---- | --------------------- | ---------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F-01 | `fuzz_htttp_request`  | `htttp_parse_request`                          | Fields NUL-terminated, header count inside its array, body slice inside the input buffer; parse -> serialize -> parse is stable                            |
| F-02 | `fuzz_htttp_response` | `htttp_parse_response`                         | The same, plus a status in 100..599                                                                                                                        |
| F-03 | `fuzz_codec_request`  | `htttp_parse_request` + `bcl_decode_request`   | Every count inside `BB_MAX_*`, `payload_len <= BB_CIPHERTEXT_MAX`; decode -> encode -> decode is a fixed point                                             |
| F-04 | `fuzz_codec_response` | `htttp_parse_response` + `bcl_decode_response` | The same, for the counted arrays a hostile daemon controls (`tally_count`, `hash_count`)                                                                   |
| F-05 | `fuzz_jwt_verify`     | `jwt_verify`                                   | **No fuzzer-invented token verifies** (the signing key is private to the target); claims zeroed on every failure; same token, same verdict                 |
| F-06 | `fuzz_rows`           | `tdb_row_count`, `tdb_row_fields`              | Every returned slice inside the reply body and free of TAB/LF; a row the count promised is readable; out-of-range rows are refused                         |
| F-07 | `fuzz_rc_line`        | `rc_classify_line`                             | The returned pointer is inside the caller's line and terminated within it; a command is trimmed; the verdict does not depend on the buffer around the line |
| F-08 | `fuzz_rc_bind`        | `rc_bind`                                      | Only in-range values reach the struct; a defect is reported as two C strings; reading one file twice gives one answer                                      |
| F-09 | `fuzz_playername`     | `name_ok`, `name_fold`                         | Nothing accepted carries LF, TAB or a quote; folding is total and idempotent; a rejected name writes nothing                                               |
| F-10 | `fuzz_ctl_frame`      | `ctl_frame_read`                               | A frame declaring more than `cap` is refused; on success exactly `*len` bytes were written and nothing past them was touched                               |

Findings so far - six bugs in the first two hours of running, all fixed, all
filed as regression inputs in `tests/fuzz/regress/`:

| Found by   | Where                           | What                                                                                                                                                                                      | Why it matters                                                                                       |
| ---------- | ------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| F-01       | `htttp.c` `copy_token`          | A NUL byte inside a header name was accepted. `"\0Cert-Name: alice"` parses, and every `htttp_header_get("Cert-Name")` then misses it                                                     | Header smuggling: the parser sees an identity header the app cannot. Same applied to method and path |
| F-02       | `htttp.c` serializers           | A message with exactly `HTTTP_MAX_HEADERS` headers serialized OK, then the generated `Date`/`Content-Length` made it one too many - a frame this library emits and its own parser rejects | Sender sees success, receiver sees a 400, nothing explains why                                       |
| F-03       | `codec.c` `body_append`         | A text field carrying `\n` or `\r` became extra body lines. A title of `"Budget\neligible=mallory"` encodes to a CREATE with an extra `eligible=` line                                    | Field injection into the ballot protocol, from an admin-supplied string                              |
| F-04       | `codec.c` `bcl_decode_response` | `hash_count=`/`tally_count=` were taken off the wire by `atoi` while the row writers stop at the array bound, so the struct could announce 78 entries in a 64-entry array                 | The daemon (or anything answering as it) chose how far past the array its client would read          |
| F-03, F-04 | `codec.c` `body_for_each`       | `body + body_len` on a bodyless message computed `NULL + 0`                                                                                                                               | Undefined behaviour on any request or response without a body                                        |
| F-06       | `rows.c` `block_count`          | The trailer's row count accumulated with no overflow guard: `said * 10 + digit`                                                                                                           | Signed overflow, undefined, on a reply from a wedged runner                                          |

Two of the six were in `libhtttp`, which `DESIGN.md` keeps byte-identical with
tetriSH — **those two fixes have to be copied to that repository**, or the next
`diff` between the trees stops being the "has this landed yet" check it exists
to be.

Two harness defects were found the same way and are worth recording, because a
fuzz target that is wrong about the contract wastes more time than no target:
`Content-Length` was held to a round-trip survival property the serializer
cannot meet (it computes the header), and duplicate header keys were counted
case-sensitively against a case-insensitive lookup. Both are documented at the
check that makes them.

### Milestones

| Milestone                                                                                                                                            | Cases      | State                                                                                         |
| ---------------------------------------------------------------------------------------------------------------------------------------------------- | ---------- | --------------------------------------------------------------------------------------------- |
| **Available today** - `make fuzz-regress` (the corpus and every filed crash, replayed under ASan) and `make fuzz-smoke` (60s of mutation per target) | F-01..F-10 | Running.                                                                                      |
| **24-hour campaign** - `make fuzz-long`, from a warm corpus, before the final presentation                                                           | F-01..F-10 | Scheduled. Report: execs/sec, corpus size, line coverage per target, crashes found and fixed. |
| **Transport and daemon** - `session_recv` behind a handshake fixture, and a scripted socket fuzzer against a live `ballotd`                          | new        | Waiting on the same integration work as I-09..I-16.                                           |

---

## Timeline and milestones

### Unit tests

All unit cases run today except U-32, and they stay green as the seams are implemented: substituting a seam is how they are written, so a real store or real keys behind it changes nothing about the case.

| Milestone                                                        | Unit cases                       | What is missing                                                                                                                                                         |
| ---------------------------------------------------------------- | -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Available today** - all logic, seams substituted (`make test`) | U-01 .. U-31, U-33 .. U-43       | Nothing.                                                                                                                                                                |
| **Crypto/PKI** - real keys behind the crypto seams               | U-32, plus a re-run of U-33/U-34 | U-32 tests the RSA-OAEP round trip itself, which is the one thing a substitute cannot stand in for. U-33/U-34 pass against the placeholder KDF and are re-run for real. |

The seam implementations themselves (SimpleDB behind `db_exec`, libtetrissh behind `bcl_send`, OpenSSL behind the crypto seams) are covered by the integration and infrastructure cases below, not by unit cases.

### Infrastructure tests

D-01..D-08 and L-01..L-07 run today under `make test`, with no milestone in front of them: both components are complete and ported.
D-07 and D-08 need a JVM and `db/dist/simpledb.jar`; on a machine with neither they skip rather than fail, so they are the only cases in this plan whose result is environment-dependent.

### Integration tests

| Prerequisite feature                                              | Integration cases | Direction                        | Gate                                                           |
| ----------------------------------------------------------------- | ----------------- | -------------------------------- | -------------------------------------------------------------- |
| SimpleDB behind `BallotStore.exec` (`db_exec`), via `libtetrisdb` | I-01..I-06        | Backend, bottom-up               | D-01..D-08 green first, then persistence and concurrency cases |
| `ballotd` assembled + local admin channel                         | I-07, I-08, I-17  | Frontend, admin path (local)     | Invalid config surfaced; election opens                        |
| Transport: `libtetrissh` behind `SecureSession.send` (`bcl_send`) | I-09..I-16        | Frontend, voter path (encrypted) | UC-2 refusal partitions + ciphertext-only wire                 |
