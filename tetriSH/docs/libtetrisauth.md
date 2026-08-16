# libtetrisauth

Reference for the things that do not fit in a header.

The design lives in the headers, not here.
[`core/include/libtetrisauth/tetrisauth.h`](../include/libtetrisauth/tetrisauth.h) is the seam and the reason for it, [`jwt.h`](../include/libtetrisauth/jwt.h) is the token layer, [`provision.h`](../include/libtetrisauth/provision.h) is the schema and the secret, and [`authstatus.h`](../include/libtetrisauth/authstatus.h) is the one rule both sides of the wire implement.
Read those first.
This file carries only the five things that are genuinely tabular or cross-cutting: the status codes, the attempt rule, the `user` schema, the startup ordering and the `.tetrishrc` keys.

Reasoning is not repeated here.
Every decision links to the ticket that took it, and the tickets are the archive.
[ADR 0001](adr/0001-tetrislogd-stays-on-piperunner.md) and [ADR 0002](adr/0002-starting-the-shared-socketrunner.md) record the runner topology and launcher.
[ADR 0003](adr/0003-resolving-the-project-root.md) records why the proposed shared root helper was rejected.

## Status codes

Five statuses reach the client, and every one of them is a code `htttp_reason()` already knows.
That matters mechanically: `htttp_serialize_response` silently refuses a status it has no phrase for.

| Status | Cause                                                                                                                    | What the client does                                                                                                                    |
| ------ | ------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------- |
| `200`  | `LOGIN`, `REGISTER` or `GUEST` succeeded                                                                                 | Proceed. The `LOGIN`/`REGISTER` body carries a JWT, which the client discards ([#46](https://github.com/JediNakDev/tetriSH/issues/46)). |
| `400`  | Malformed body, empty field, username outside the allowlist, password outside 8..128 at `REGISTER`                       | Back to the credential form. The client validates locally first, so a `400` from `tetrisu` is a bug in `tetrisu`.                       |
| `401`  | Wrong password                                                                                                           | Back to the form, re-opened on the **password** field.                                                                                  |
| `404`  | No such user                                                                                                             | Back to the form, re-opened on the **username** field.                                                                                  |
| `409`  | Duplicate username on `REGISTER`, or any credential offered after the exchange has already resolved                      | Separated by whether the client has authenticated yet, which it knows. See below.                                                       |
| `500`  | Account service unreachable: runner down, socket missing, deadline expired, `.tetrishrc` unreadable, JWT secret unusable | "Could not reach the account service, continuing as a guest", then send `GUEST` and proceed.                                            |

`404` and `401` are deliberately distinguishable, overturning an earlier decision that made both `401` at equal latency ([#47](https://github.com/JediNakDev/tetriSH/issues/47) decision 13).
`REGISTER` must answer `409` on a duplicate, so username existence is already discoverable by attempting a registration; hiding it at `LOGIN` closes half an oracle while the other half stays open by necessity.

Two mechanical consequences, both of which look like bugs to someone who does not know the decision:

- **There is no dummy salt and no PBKDF2 on the miss path.**
  A miss returns early at roughly the cost of the query, about 7 ms, against about 65 ms for a hit.
  The timing split is deliberate and matches the status split.
  The early return carries a comment naming the decision, because a future reader gets this wrong in either direction: by deleting the return to "fix a timing leak", or by leaving it and never noticing there was a decision.
- **`GUEST` cannot fail from the runner being down.**
  A guest opens no connection at all, so the `500` fallback's assumption that `GUEST` will succeed is sound rather than optimistic.

### The two meanings of `409`

The status line cannot separate them, but the client never has to guess: it knows whether its pre-auth exchange has resolved, and that is exactly the line the two meanings fall on.

| When                                           | `409` means                                                   |
| ---------------------------------------------- | ------------------------------------------------------------- |
| Before the exchange resolves (`REGISTER`)      | Duplicate username, only ever that.                           |
| After the exchange resolves (any of the three) | Already authenticated. Identity is fixed for this connection. |

`LOGIN` can never be answered `409` before the exchange resolves, because `tauth_login()` runs before any state exists to conflict with.
A client that sees one has desynced.

Identity being fixed for the life of the connection is what removes the ambiguity, and it is not free.
A player who chose guest, or who was dropped to guest by a `500`, cannot become an account without reconnecting, and [#61](https://github.com/JediNakDev/tetriSH/issues/61) records that `tetrisu` has no reconnect - so today that means quitting the client.
The recovery line is therefore an instruction rather than a hedge:

> **Already signed in on this connection.** Quit and reconnect to log in as somebody else.

The superseded design kept guest non-terminal ([#48](https://github.com/JediNakDev/tetriSH/issues/48) decision 3) and paid for it with a genuinely ambiguous cell - a `REGISTER` upgrade could be a duplicate name or a state conflict, and the server could not say which - plus a `shutdown()` on the session fd to carry a drop that `session_dispatch()`'s `void` return could not.
Both are deleted.
Reasoning: [#48](https://github.com/JediNakDev/tetriSH/issues/48), amending [#57](https://github.com/JediNakDev/tetriSH/issues/57) section 2.

## The attempt rule, which is implemented twice

**A response counts toward `auth_max_attempts` if and only if it means the credentials were wrong: `401` and `404`.
`400`, `409` and `500` do not count, and nothing resets the counter for the life of the connection.**

That is the entire rule, and it is short on purpose.
Both implementations consume `AUTH_STATUS_COUNTS()` from [`authstatus.h`](../include/libtetrisauth/authstatus.h) rather than writing the list a second time.

|            | Server                                                                       | Client                                         |
| ---------- | ---------------------------------------------------------------------------- | ---------------------------------------------- |
| Where      | File-static in libtetrisauth                                                 | `auth_fails` on `Client`, beside `last_reject` |
| Scope      | One process is one connection                                                | One process is **not** one connection          |
| Moved by   | `tauth_login()` only. `tauth_offer()` refuses with `409`, which never counts | Every auth screen                              |
| At the cap | `tauth_login()` returns `TAUTH_DROP` and the caller closes                   | Nothing. The client never predicts the drop.   |

Both halves are asserted, and the split is the same one the table draws: `tests/test_auth.c` for the server's enforcement, and `tests/test_authbudget.c` for the shared rule both columns call — `libtetrisutil/authbudget.c`, which is pure, so it needs no server, no socket and no terminal.

The server's counter has exactly one writer because identity is fixed by the pre-auth exchange, so no credential can be offered after it.
That is what deleted the `shutdown()` on the session fd: with an upgrade path there had to be a way to drop a connection from inside a `void` dispatch function, and without one there is nothing to drop.

**The asymmetry is the trap, and it is why the client does not mirror the server's shape.**
On the server, "file-static" and "per connection" are the same statement.
In `tetrisu` they are not: one process is one user, and `screen_connect()` already loops over `client_connect()` after a refused certificate or a dead route.
A file-static in the client would survive a connection the server has forgotten.

The client's invariant is "no connection implies the count is zero", and it is enforced on both edges: `client_connect()`'s opening `memset` zeroes it, and `client_disconnect()` plus the `CLI_EV_DISCONNECT` arm clear it.
Today's code is already right, but only by accident of a `memset` written for another purpose, which is precisely the correctness-that-must-be-re-derived the map's first constraint names.

The counting is scoped by `auth_pending`, the method awaiting a response.
A `401` answering a `JOIN` is a desync, not a bad password, and spending one of the user's attempts on it is the wrong response to a desync.

**The client reports the count it knows and never the allowance it cannot know.**
From the second counted failure onward:

> Attempt failed (2 failed so far on this connection).
> The server closes the connection after `auth_max_attempts` failed attempts on one connection.

`tetrisu` holds **no copy of `auth_max_attempts`**: no constant, no second rc key, nothing to keep in sync.
`X-Auth-Attempts-Remaining` was named as the escape hatch and deliberately not built.
Reasoning: [#56](https://github.com/JediNakDev/tetriSH/issues/56), [#57](https://github.com/JediNakDev/tetriSH/issues/57) section 3.

## The `user` table

```
user (id int, name string, salt string, digest string, iters int, created_at int)
```

Created by `tetrisdb start`, from `TETRISAUTH_DB_SCHEMA` in [`provision.h`](../include/libtetrisauth/provision.h), before the JVM is spawned.

| Column       | Rule                                                                                                                                                            |
| ------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `id`         | `max(id) + 1`, allocated as the first statement of the registration transaction. An empty result means `1`. Kept because a token's `sub` is an integer user id. |
| `name`       | `[A-Za-z0-9_-]` only, 1..15 characters, folded to lowercase on input and stored folded. Anything else is `400`.                                                 |
| `salt`       | 16 bytes from `RAND_bytes`, lowercase hex, 32 characters.                                                                                                       |
| `digest`     | PBKDF2-HMAC-SHA256, 32-byte output, lowercase hex, 64 characters.                                                                                               |
| `iters`      | The iteration count **this row** was hashed at. Default 600,000.                                                                                                |
| `created_at` | Unix seconds.                                                                                                                                                   |

Notes that are not obvious from the column list:

- **The allowlist is one rule, not three exclusions.**
  LF would break the body split, TAB would shift every field of a tab-separated select reply, and `'` would survive `tdb_quote()`'s doubling into storage as `o''brien`.
  Three exclusions for three unrelated reasons is a paragraph a reader has to hold; one allowlist is a line of code and a line of explanation.
  It also happens to sit inside `htttp`'s `valid_field` range, and to be shell- and URL-safe.
- **`tdb_quote()` is still called and can never fire.**
  Kept because the validator and the SQL builder are separate code that can drift, and because it turns the failure into a stored oddity instead of SQL injection.
  A doubled quote in `user.dat` is proof the validator was bypassed.
- **Typed casing is not preserved.**
  A player who registers `JediNakDev` is `jedinakdev` on the roster forever, because SimpleDB has no `UPDATE`.
- **The password minimum is enforced at `REGISTER` only, never at `LOGIN`.**
  If `LOGIN` rejected a short password before hashing, raising the minimum later would make existing accounts permanently unloggable.
- **600,000 iterations costs 65 ms**, roughly ten times the entire database exchange.
  It runs outside both the semaphore and the database deadline, so a slow hash cannot produce a spurious `500`.

The hazards that a future writer of this table must know (the semaphore convention, id recycling, the 2038 wrap, why `iters` is not dead weight) are in `provision.h`, beside the schema constant, rather than here.
Reasoning: [#47](https://github.com/JediNakDev/tetriSH/issues/47), [#44](https://github.com/JediNakDev/tetriSH/issues/44).

## Startup ordering

One line in `.tetrishrc`, sequenced between the two daemons.
The shell runs rc commands synchronously, so the ordering is real rather than hopeful.

```
dspawn2 tetrislogd      # first, so the launcher's own report has somewhere to go
tetrisdb start          # not under dspawn: it needs the child's exit status and its stderr
dspawn2 tetrisd         # preserves the project root for its session children
```

`tetrisdb start`, in order.
Steps 4 and 4b are libtetrisauth's; the rest is the launcher's and is recorded in [ADR 0002](adr/0002-starting-the-shared-socketrunner.md).

1. `flock(LOCK_EX|LOCK_NB)` on `<db_dir>/.runner.lock`, held across the fork and exec so the JVM holds it for exactly its own lifetime. Refuse if held.
2. Refuse if `catalog.txt` already declares a table named `log`.
3. `sem_unlink("/tetrish_register")`, then recreate.
4. `tdb_ensure_table(db_dir, TETRISAUTH_DB_TABLE, TETRISAUTH_DB_SCHEMA)`.
5. `tauth_secret_provision(root, ...)`. Refuse the start if the secret is unusable.
6. Unlink the socket path, safe only because step 1 proved no runner owns this directory.
7. Fork, `setsid`, stderr appended to `var/log/tetrisdb.err`, exec the JVM - `tdb_runner_spawn()`, which takes the already-opened stderr fd and inherits the step 1 lock into the child.
8. Poll `connect()` until it succeeds, the child dies, or about 10 s elapse - `tdb_runner_wait()`. Report, exit 0 or 1.

Steps 7 and 8 are `core/include/libtetrisdb/socket/runner.h`'s, not the launcher's own code.
Only the mechanics moved: the lock, the catalog refusal, the semaphore, the provisioning, the socket unlink, the exit codes and the operator's report are all still `bin/tetrisdb`'s, because they are true of a tetriSH installation rather than of starting a runner.
The reason for the split is [ADR 0001](adr/0001-tetrislogd-stays-on-piperunner.md)'s: resolving and validating the `java`/jar pair is shared with `tdb_start()`, so the version trap has one home rather than two.

**Why the ordering is correct by construction rather than by convention.**
SimpleDB reads `catalog.txt` once at runner startup and never again, so `user` must exist before the JVM starts.
Steps 4 and 7 sit eleven lines apart in one process, and nothing else in the system can spawn a runner, because step 1 refuses.
There is no ordering for anyone to remember and no race to lose.

**Nothing probes the runner and nothing gates on it.**
Step 8 decides what gets logged and what exit code is returned; it gates nothing.
Reachability is discovered by the login that needs it, and **no component may hold state describing whether the runner is reachable**.
A probe's answer expires immediately, and code that caches it can refuse a login that would have worked, which is a degradation that never recovers.
The cost, on the record: while the runner is down, every login attempt pays the full `db_timeout` before its `500`.

**`tetrisd` changes zero lines**, and no C process touches the runner at boot.
The direction "tetrisd starts anyway, auth is broken, everything else works, guests can still play" therefore needs no enforcement code at all: it is satisfied by an absence.
The design work was deciding where the temptation to add a gate lives and refusing it there.

Restarting for a schema change is `tetrisdb stop && tetrisdb start`.
Live players see nothing unless they are authenticating in that window, in which case they get a `500` and play as a guest.
No game is interrupted, because no session holds a connection while playing.

## `.tetrishrc`

Three namespaces, one validator each.
Each asks `rc_get_int()` / `rc_get()` for the keys it owns, by name.
`rc_get()` answers `RC_NO_FILE` when there is no `.tetrishrc` at all - positive, so a reader testing `< 0` for a bad value never mistakes it for one - which is the one case this library must not paper over: with no file, `LOGIN` and `REGISTER` answer `500` and `GUEST` is unaffected.
`bin/session` reads `.tetrishrc` from `$TETRISH_ROOT`, falling back to its working directory.
The supported `dspawn2` launch path preserves the project root as the working directory.

An unparseable value **refuses to start** rather than keeping a default.
Keeping a default after an operator has demonstrably tried to set something else is the same silent-wrong-configuration bug as a missing file, scoped to one line.

A MISSPELLED KEY IS NOT CAUGHT, and cannot be: a reader that asks for `auth_token_ttl` by name never learns that `auth_token_tll` is in the file.
Nothing else reads it either, so it applies nothing and says nothing.

| Namespace      | Validator            | Why it can see the whole list                                                                 |
| -------------- | -------------------- | --------------------------------------------------------------------------------------------- |
| `log_`         | `bin/tetrislogd`     | Owns every `log_` key.                                                                        |
| `db_`, `auth_` | `bin/tetrisdb start` | Already links libtetrisdb and libtetrisauth, and runs before `tetrisd` with a human watching. |

`bin/session` validates nothing.
It is a reader, not a validator, and a warning from a process serving a client is noise nobody sees.
Its keys are checked by `tetrisdb start` moments earlier out of the same file, which is also what finally verifies that the two `db_ipc` readers agree.

### `auth_` keys

Owner libtetrisauth, read by `bin/session` inside `tauth_login()` on its first call.

| Key                 | Type               | Default           | Missing | Unparseable                                        |
| ------------------- | ------------------ | ----------------- | ------- | -------------------------------------------------- |
| `auth_max_attempts` | int 1..100         | `5`               | default | `500` on `LOGIN`/`REGISTER`; the counter runs on 5 |
| `auth_token_ttl`    | int 60..31536000 s | `604800` (7 days) | default | `500` on `LOGIN`/`REGISTER`                        |
| `auth_pbkdf2_iters` | int 1..10000000    | `600000`          | default | `500` on `LOGIN`/`REGISTER`                        |

`auth_max_attempts` is the only `auth_` key that touches a guest-reachable path, since the counter also governs `tauth_offer()`.
When the file is missing it runs on 5 and the session logs why, because dropping a guest's connection over an unreadable config file would trade one silent failure for a louder wrong one.

`auth_token_ttl` governs no tetriSH behaviour, because nothing here verifies a token.
It is headroom for the reuse project and should be read that way rather than as a tuned value.

**`auth_pbkdf2_iters` is a security knob that points downward**, and the mitigation is visibility rather than a floor.
A hard floor would have to sit below whatever a test wants and above nothing that matters in production, so it stops nobody while looking like it stops someone.
The key exists because `iters` is stored per row specifically so the count can be raised without invalidating every account, and a compile-time constant makes that justification false.

**Not a key: the JWT secret path.**
`<root>/auth/jwt_secret`, a compile-time constant.
`auth/private_key.pem` and `auth/server_signed.crt` use the same `$TETRISH_ROOT` or working-directory convention in `bin/session`.

### `db_` keys libtetrisauth reads

The full `db_` and `log_` tables are the launcher's and `tetrislogd`'s.
Two of them are read by `bin/session` on the auth path:

| Key          | Type              | Default                 | Notes                                           |
| ------------ | ----------------- | ----------------------- | ----------------------------------------------- |
| `db_ipc`     | path              | `var/run/tetrisdb.sock` | Refused if the joined path exceeds `sun_path`.  |
| `db_timeout` | int 100..60000 ms | `2000`                  | Whole exchange: connect, `<<READY>>`, response. |

`db_timeout` covers the exchange only.
The wait for the registration semaphore has its own separate budget, deliberately: at 254 simultaneous registrations the tail waits about 1.2 s for its turn, and charging that against the same budget would deterministically `500` the tail of a herd while the server is perfectly healthy.
Mixing "how many people are ahead of me" with "is the database alive" also means tuning either drags the other.

`sample.tetrishrc` is the documented surface, installed by `make` when `.tetrishrc` does not exist and never overwritten.
`tests/test_rc.c` asserts both directions against the exported key tables, so drift fails `make test`.

## What has no test, and why

Named so a green suite is not read as a broader claim than it makes.

- **`tauth_login()`'s scrub of its own stack buffer.**
  Review-only. The buffer is a local in a function with one exit, which is what makes the scrub correct and also what makes it unreachable. A test would read indeterminate memory to assert a security property.
- **`session_recv()`'s freed heap copy.**
  Untestable in principle, not just in practice: reading a freed block is undefined behaviour, so a test asserting it is scrubbed would be UB that happens to pass. Closed by review of the diff.
  If anyone retries the obvious residue probe, note that its positive control fails on macOS, so a clean result from it is meaningless.
- **The JWT secret creation race.**
  There is no creation in the session process, so there is no race. The property was deleted rather than tested.
- **Equal login latency.**
  The property does not exist any more. Asserting it structurally would assert a behaviour the design forbids, and asserting it by timing would be a flaky test guarding something traded away on purpose.
- **`<<END retry>>` actually firing.**
  Reasoned from `LockManager`, never observed in 320 measured concurrent logins. Asserting a deadlock occurs is asserting a scheduler outcome.

**No test in the suite asserts on elapsed wall-clock time.**
Both deadlines are pinned by asserting a `500` under an `alarm()` watchdog, in about 300 ms and with no JVM.

Two properties are load-bearing rather than coverage, and deleting either leaves a green suite:

- **The four-process registration sweep over pre-fill counts `0..15`.**
  Four children against an _empty_ table all land on page 0, which has free slots, and the transaction alone is sufficient there. The semaphore's entire reason for existing is the append onto a _full_ last page, so a test that does not sweep past a page boundary passes on a build with the semaphore deleted. The sweep does not encode the page size, because that would go silently wrong the day a column is added.
- **`GUEST` succeeding with the database provably absent.**
  This and the prompt-`500` case are the only tests in the suite that _require_ the runner's absence, so they are the only ones that cannot be skipped into uselessness on a CI box with no JVM.

On that last point: **the permanent skip is not a risk, it already happened.**
`db/dist/simpledb.jar` is gitignored, CI has no JDK step, and the database round-trip tests have therefore never run on a push.
The fix is `setup-java` plus `ant dist`, and `TETRISH_REQUIRE_RUNNER=1` turning a skip into a failure in a job that is not permitted to skip.
A printed skip line is not a mechanism; this repository is the proof.
Reasoning: [#54](https://github.com/JediNakDev/tetriSH/issues/54).

**A second JDK trap, distinct from the missing jar.**
If `db/dist/simpledb.jar` exists but was built with a newer JDK than the `java` on `PATH` (for example built under JDK 25 while `PATH` resolves JDK 17), the runner fails to start and `make test` fails outright rather than skipping - the jar is present, so the skip logic never engages.
The jar is gitignored, so this is invisible until someone runs `ant dist` locally with a stray `JAVA_HOME`.
Pin `JAVA_HOME` to the JDK version CI uses before running `ant clean dist`, or delete `db/dist/simpledb.jar` and let the runner-absent skip take over.
