# ADR 0002: starting the shared SocketRunner

Status: accepted.
Context: [issue #52](https://github.com/JediNakDev/tetriSH/issues/52).
Supersedes nothing; confirms [ADR 0001](0001-tetrislogd-stays-on-piperunner.md) and withdraws one supporting item from [#51](https://github.com/JediNakDev/tetriSH/issues/51).

## Decision

A new one-shot command, `bin/tetrisdb {start|check|stop}`, launches and provisions the shared `SocketRunner` over `var/db`.
It is invoked from one `.tetrishrc` line, after `dspawn tetrislogd` and before `dspawn tetrisd`.
It is not itself a daemon, it is not run under `dspawn`, and `tetrisd` changes zero lines.

`start` performs the whole preflight in one process and then execs the JVM, so nothing else in the system can start a runner:

1. `flock(LOCK_EX|LOCK_NB)` on `<db_dir>/.runner.lock`, refusing if held.
2. Refuse if `catalog.txt` already declares a table named `log`.
3. `sem_unlink("/tetrish_register")` and recreate, so a session killed mid-registration cannot wedge registration permanently.
4. `tdb_ensure_table()` with libtetrisauth's exported schema constants.
4b. `tauth_secret_provision()` - create `auth/jwt_secret` if absent, validate it if present, refuse the start if it is unusable (added by [#58](https://github.com/JediNakDev/tetriSH/issues/58); see the amendment below).
5. Unlink the socket path, which is safe only because step 1 proved no runner owns this directory.
6. Fork, `setsid`, stderr to `var/log/tetrisdb.err`, exec the JVM.
7. Wait for `connect()` to succeed or the child to die, report, exit.

The full decision set, including configuration keys, operator visibility and the availability invariants, is in #52's resolution comment.
This ADR records only the two facts about `db/` that shaped it, because both contradict something currently written down elsewhere.

## Finding 1: running the start command twice silently corrupts the database

`SocketRunner.bind()` ([`db/src/java/simpledb/SocketRunner.java:187`](../../db/src/java/simpledb/SocketRunner.java)) calls `Files.deleteIfExists(path)` unconditionally before binding its Unix socket.
It does not check whether a live runner is listening there.

So a second `SocketRunner` pointed at `var/db` starts successfully.
It unlinks the first one's socket file and binds its own.
The first JVM keeps running, keeps serving the connections it already holds, and keeps its own `BufferPool`; it simply never receives another connection.

That is two JVMs over one `catalog.txt` and one set of `.dat` files, with two page caches and no cross-process invalidation.
It is the corruption case that the integration doc's invariant 1 exists to prevent, and it is reached by running the ordinary start command a second time, with no error printed anywhere.

The comment on that line is reasoning about a socket file left behind by a previous run, and it is correct about that case.
It just does not distinguish a stale socket from a live one, and until now nothing else did either.

### Why the guard is a `flock` on the directory and not a probe on the socket

The obvious guard is to `connect()` first and refuse if something answers.
It was the first answer here and it is wrong twice.

It races.
Two launchers can both probe, both see nothing, and both proceed to bind.

More importantly it guards the wrong noun.
The invariant is about the *data directory*: two runners over `var/db` reached through two different `db_ipc` paths corrupt it just as thoroughly, and a socket-level check cannot see that at all.

A BSD `flock` belongs to the open file description, which `fork` shares and `exec` preserves, and it is released only when every descriptor referring to it is closed.
Holding it across the exec therefore makes the JVM hold the lock for exactly its own lifetime, including a `SIGKILL` or a panic, with no cleanup path that can be got wrong.

The cost is stated rather than hidden: the JVM holds a lock it does not know about, which is cleverness a reader has to re-derive, and the map's standing constraint 1 warns against exactly that.
It is bought deliberately, because the alternative is a correct-looking guard with a race and the wrong granularity standing in front of silent corruption.
It is paid down by writing the child's pid into the lock file, so `cat var/db/.runner.lock` names the owner and `stop` has its input, and by a comment at the `flock` call carrying this reasoning.

The `log`-table check in step 2 is not redundant with the lock: `tetrislogd`'s `PipeRunner` is a forked child that takes no lock, so the two guards catch different mistakes.
The check is on the table rather than on the path, because comparing `db_dir` against the literal `var/db_log` only catches the spelling of today's mistake.

## Finding 2: SocketRunner does flush on shutdown

The shutdown hook ([`SocketRunner.java:204-231`](../../db/src/java/simpledb/SocketRunner.java)) closes the listener, calls `pool.shutdownNow()` with a five second `awaitTermination`, calls `Database.getBufferPool().flushAllPages()`, and deletes the socket file.

`SIGTERM` therefore carries the same durability guarantee that closing `PipeRunner`'s stdin carries, which is what makes `tetrisdb stop` a real answer to #52's shutdown question rather than a hope.

Two consequences beyond that:

- Because the hook removes the socket file on a clean exit, a socket file that exists means either a live runner or an unclean death.
- `SIGKILL` remains unclean, and the recovery pass at the next `start` is what covers it.
  This is the second reason `--no-recover` is not exposed as a configuration key.

### What this corrects

[#51](https://github.com/JediNakDev/tetriSH/issues/51) lists, among its reasons for keeping `tetrislogd` where it is, that `PipeRunner`'s stdin-close flush guarantee has no `SocketRunner` equivalent.
**That item is withdrawn.**

ADR 0001 does not make that mistake, and this finding strengthens it.
It had already argued that the durability angle was insurance against a case `tetrislogd` does not create, and it explicitly moved the decision onto failure behaviour rather than onto what reaches disk.
Its two load-bearing reasons, the flight recorder not sharing a failure domain with the engine and fire-and-forget having to learn to reconnect and to time out, are untouched, and they were the right things to decide on.

`include/libtetrisdb/tetrisdb.h` describes closing stdin as "the only shutdown path that flushes dirty pages".
That should be corrected where it stands.
It is true of `PipeRunner`'s transport and misleading as a statement about SimpleDB.

## Amendment: step 4b, the JWT secret ([#58](https://github.com/JediNakDev/tetriSH/issues/58))

`start` also provisions `auth/jwt_secret`: create at `0600` if absent (32 bytes of `RAND_bytes`, written to a `mkstemp` temp file and `link`ed into place), validate if present, and **refuse the whole start** with a `LOG_ERROR` and a non-zero exit if it is loose, malformed or not a regular file.
`check` calls the same validation a session calls and exits non-zero if the secret is unusable, even when the socket answers.

This is where [#46](https://github.com/JediNakDev/tetriSH/issues/46)'s "fail hard if group- or other-readable" lives, because this is the only process where hard failure happens in front of an operator and before any session exists.

The reason it is here and not in the session process is that `O_CREAT|O_EXCL` does not make create-and-fill atomic: a creator that dies between the create and the write leaves a zero-length file that #46's minimum-length rule then correctly refuses forever.
Provisioning has one writer, holds the lock from step 1, and fails loudly, so the hazard is moved rather than handled.
Nothing is lost by tying the secret's lifecycle to the database launcher's, because nothing can reach a token mint without first reaching the runner.

Two costs, stated:

- `tetrisdb` now provisions two things that are not the database (this and the `/tetrish_register` semaphore in step 3), so its name is imprecise. Not renamed; the churn buys no behaviour.
- Because step 1 refuses when the lock is held, a deleted secret cannot be re-provisioned under a live runner. Recovery is `tetrisdb stop && tetrisdb start`, the same answer this ADR already gives for adding a table.

## Consequences

- Adding a table to `var/db` means `tetrisdb stop && tetrisdb start`, because the catalog is read once.
  Live players see nothing unless they authenticate in that window, in which case they get a `500` and play as a guest.
  No drain, no handoff, no announcement.
- Nothing anywhere may cache whether the runner is reachable.
  That rule is what makes recovery automatic, and it is why `start`'s readiness wait reports but gates nothing.
- The JVM's stderr has a home at `var/log/tetrisdb.err`.
  Under `dspawn` it would go to `/dev/null`, which is how `tetrisd`'s `execl` failure once stayed invisible (`src/tetrisd/tetrisd.c:28-35`).

## Amended by ADR 0003

[ADR 0003](0003-resolving-the-project-root.md) corrects two things above.
The rc line is sequenced between `dspawn2 tetrislogd` and `dspawn2 tetrisd`, not `dspawn`, which is the daemoniser this project stopped using.
And `start` and `check` gain a preflight refusal: on a missing or unresolvable `.tetrishrc`, on an unparseable value, and on any unknown `db_*` or `auth_*` key.
`tetrisdb start` is the validator for both namespaces because it already links both libraries and runs before `tetrisd` with a human watching.

## Revisit if

- `db/` gains a bind path that refuses to unlink a live socket, which would make the directory lock a belt beside a brace rather than the only guard.
- A second process needs to write `var/db` outside a session, since the lock currently encodes "one runner per directory" and not "one writer per directory".
- The runner starts needing a supervisor, which it does not today precisely because the game stays playable without it.
