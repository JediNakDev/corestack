# ADR 0001: tetrislogd stays on PipeRunner

Status: accepted.
Context: [issue #52](https://github.com/JediNakDev/tetriSH/issues/52), which raises the question left open by [#43](https://github.com/JediNakDev/tetriSH/issues/43): once a `SocketRunner` is in the system for `user`, a second JVM for logging needs a reason.

> **Confirmed by [ADR 0002](0002-starting-the-shared-socketrunner.md), which found the evidence.**
> "The reason that does not hold up" below is more right than it knew: `SocketRunner`'s shutdown hook calls `flushAllPages()` on `SIGTERM`, so the flush guarantee is not even exclusive to `PipeRunner`'s transport.
> [#51](https://github.com/JediNakDev/tetriSH/issues/51) does carry the wrong version of this as a supporting reason, and that item is withdrawn there.
> This ADR's conclusion and its two load-bearing reasons are untouched.

## Decision

`tetrislogd` keeps its own `PipeRunner` child over `var/db_log`.
It does not migrate to the shared `SocketRunner` that serves `var/db`.

The two directories must never converge.
Two JVMs over one `catalog.txt` and one set of `.dat` files is the corruption case that per-process `BufferPool` caching with no cross-process invalidation produces, and it is silent.

## The reason that does not hold up

"It already works, and migrating is churn" is not a reason, and this ADR exists so that it does not survive as one.

Neither does durability, which is the reason the module header implies.
`include/libtetrisdb/tetrisdb.h` describes closing `PipeRunner`'s stdin as "the only shutdown path that flushes dirty pages".
That is insurance against a case `tetrislogd` does not create:

- `Parser` auto-commits every statement submitted outside an explicit `BEGIN` (`db/src/java/simpledb/Parser.java:546`).
- Committing calls `flushPages(tid)` before releasing locks (`db/src/java/simpledb/storage/BufferPool.java:134-137`), and `flushPage` forces the log ahead of the page (`BufferPool.java:249-261`).

SimpleDB is therefore FORCE, and `tetrislogd` submits one statement at a time and never opens a transaction of its own.
At every instant between statements it has nothing unflushed.
Closing stdin flushes something only for a caller that holds a transaction open across statements, and there is no such caller here.

So the choice has to be made on failure behaviour, not on what reaches disk.

## The reasons that do hold up

### 1. The flight recorder must not share a power bus with the engine

`tetrislogd` records what happened, including the auth path failing.
If its storage is the same JVM that serves `user`, then that JVM hanging, crashing, or being restarted takes out the record of it happening, at exactly the moment the record is worth having.

The shared runner is restarted for ordinary reasons, not only for crashes: the catalog is read once at startup, so adding any table to `var/db` means a restart.
Under a merge, every such restart is also a gap in the logs.
A child `tetrislogd` forks and owns has a failure domain that does not overlap with the thing it observes.

### 2. Fire-and-forget would have to learn to reconnect, and to time out

`libtetrisdb`'s submit path inherits `logmsg.h`'s contract: never blocks, never fails loudly, drops rather than stalls.
A pipe to an owned child cannot come back from the dead.
The child is gone, the statement is dropped, `tdb_dropped()` counts it, and the daemon keeps serving.
That is the whole failure model, and it fits in a sentence.

A shared socket can go away and come back, which adds connect, retry, and backoff to the worker.
Worse, it adds a wait that has no natural bound: a connection queued behind a full `--sessions` pool (`newFixedThreadPool`, `db/src/java/simpledb/SocketRunner.java:118`) waits **indefinitely and silently**, with no error and no `<<READY>>`.
That is the hang [#48](https://github.com/JediNakDev/tetriSH/issues/48) introduced a wall-clock deadline to convert into a visible `500`.

Migrating would mean teaching deadline logic to the one path in the codebase that is documented as never blocking.
The price is new failure handling in the least forgiving place; the purchase is one fewer JVM.

## What is given up, and why it is acceptable

Two runners means two of everything operational: two launch paths, two shutdown paths, two places the `java`-on-`PATH` version trap can fire.
That cost is real and it argues the other way.

It is paid down rather than accepted whole.
Resolving and validating the `java` and jar pair belongs in `libtetrisdb`, shared by `tdb_start()` and by whatever launches the `SocketRunner`, so the version trap is fixed once even though the runners stay separate.

## Consequences

- `var/db_log` stays `tetrislogd`'s alone, and `var/db` stays the shared runner's alone.
  Whatever launches the shared runner should refuse to start if it is pointed at `var/db_log`, because the two are separate today only by convention.
- Nothing fire-and-forget ever touches the socket.
  The blocking request and response auth path is the only socket client, and it is allowed to fail loudly, because failing loudly is what produces the guest fallback.
- `tetrisdb.h`'s claim about stdin close being the only flushing shutdown path should be corrected where it stands.
  It is true of the transport and misleading about durability.

## Revisit if

- Something in `var/db_log` starts holding a transaction open across statements, which would make the flush semantics differ for real rather than on paper.
- A second writer to `var/db_log` appears, since single ownership is the constraint `PipeRunner` satisfies by construction and the shared runner satisfies by policy.
- Two JVMs become a measured problem rather than an aesthetic one.
