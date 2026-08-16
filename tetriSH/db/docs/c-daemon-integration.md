# Integrating a C daemon with SimpleDB

This document specifies how a C daemon should talk to SimpleDB.
It is written so an agent implementing the C side does not need any other context.

There are two runners, and which one you want depends on a single question: **do two daemons need to touch the same table?**

| | `SocketRunner` | `PipeRunner` |
|---|---|---|
| Clients per JVM | many | one |
| Daemons can share a table | yes | no |
| Transport | Unix socket or loopback TCP | stdin/stdout pipes to a child process |
| Concurrent transactions | yes, one per connection | one at a time |
| Failure blast radius | all daemons | that daemon only |

Both speak the identical wire protocol (Section 4), so a daemon can move between them by changing only how it obtains its two file descriptors.

**Use `SocketRunner` unless you have a reason not to.** It is the general case; `PipeRunner` is the special case where a daemon owns its tables outright and you would rather not have a shared process to keep alive.

## 1. Why one JVM can now serve everybody

A `BufferPool` and its `LockManager` live inside a single JVM.
Two JVMs have two page caches and two lock tables that know nothing about each other, so if daemon A writes a table that daemon B has cached, B serves or overwrites stale data.
There is no cross-process cache invalidation, and no amount of flushing fixes it: SimpleDB commits under FORCE, which guarantees the *writer's* durability, not visibility to a second process holding its own cache.

That is why the pipe-per-daemon design required table ownership to be strictly disjoint.

Inside one JVM the picture is different, because page-level locking is implemented ([`LockManager`](../src/java/simpledb/storage/LockManager.java)):
readers share, writers exclude, and a transaction that would wait on a cycle is aborted rather than left to hang.
So a single process can serve many concurrent clients over the same tables safely.
`SocketRunner` is that process.

```
  gamed   (C process) ──┐
                        ├──> unix socket ──> one JVM: SocketRunner
  loggerd (C process) ──┘                     ├── one Catalog
                                              ├── one BufferPool + LockManager
                                              └── one thread + Parser per connection
```

## 2. Hard invariants

These are correctness requirements, not style preferences.

1. **All daemons that share a table must connect to the same `SocketRunner` process.**
   Running two `SocketRunner` processes over the same `.dat` files reintroduces exactly the problem Section 1 describes. One process per database, not one per daemon.

2. **One connection carries one session, and only one thread may drive it.**
   The protocol is strictly ordered and half-duplex: one line in, one response block out, in lock-step, with no request IDs to multiplex on. Two threads writing to the same connection interleave their statements on the wire and their responses become unattributable.
   This is a property of the wire format, not of the database. To get concurrency, **open more connections**, one per thread. That is the intended design and costs nothing but a socket.

3. **A client must be prepared to retry.**
   With several daemons writing concurrently, a transaction can lose a deadlock and be aborted through no fault of its own. This is reported as `<<END retry>>` and is not an error (Section 4). A client that treats it as one will silently drop writes.

4. **Concurrent sessions x pages touched per transaction must fit in the BufferPool.**
   SimpleDB is NO STEAL: a dirty page cannot be evicted ([`BufferPool.evictPage`](../src/java/simpledb/storage/BufferPool.java:279)). If the concurrent working set exceeds the pool, statements fail outright rather than slowing down. `SocketRunner` sizes the pool from `--sessions` by default; raise `--pages` if transactions are large.

## 3. Running `SocketRunner`

```
java -cp dist/simpledb.jar simpledb.SocketRunner <catalogFile> <socketPath|port> [options]
```

The second argument is a Unix domain socket path if it contains a `/`, otherwise a TCP port bound to loopback only.
Prefer the Unix socket: access is controlled by ordinary filesystem permissions and there is no port to collide.

| Option | Default | Meaning |
|---|---|---|
| `--sessions=N` | 8 | Concurrent sessions served. Further connections are accepted but wait for a free slot, so an extra client is queued rather than refused; it simply does not see `<<READY>>` until its turn. |
| `--pages=N` | 16 x sessions | BufferPool size. See invariant 4. |
| `--no-recover` | off | Skip the startup recovery pass. |

On startup the process prints one line to **stderr** (`SocketRunner listening on ...`) and nothing to stdout; per-connection output goes to the connections.

### Recovery on startup

Both runners run [`LogFile.recover()`](../src/java/simpledb/storage/LogFile.java:558) before accepting work.
This matters: [`Transaction.transactionComplete`](../src/java/simpledb/transaction/Transaction.java:45) flushes a transaction's pages *before* writing its commit record, so a crash in that window leaves flushed pages on disk belonging to a transaction that never committed.
The recovery pass undoes exactly those. It is a no-op on a log with no records, so it is safe to leave enabled always.

Anything committed before a crash is on disk (FORCE on commit) and needs no replay.
It is safe to restart the process and resume submitting statements.

## 4. Wire protocol

Identical for both runners. Per connection:

- **Startup**: the server sends one line, `<<READY>>`. Wait for it before sending the first statement.
- **Request**: exactly one SQL statement per line, ending in `;`, UTF-8, newline-terminated (`\n`). Blank lines are ignored and draw no response.
- **Response**: whatever text the parser prints for that statement, then exactly one marker line:

| Marker | Meaning | What the client should do |
|---|---|---|
| `<<END ok>>` | Statement succeeded. | Continue. |
| `<<END retry>>` | The transaction was aborted to break a deadlock. Nothing is wrong with the statement. | Resubmit it, ideally after a short backoff. |
| `<<END error>>` | Statement failed: parse error, unknown table, unsupported statement. | Do not retry; the outcome will not change. |

  The body above the marker carries the error text, but its exact format is not a stable contract. Check the marker, not the body.

- **stderr**: for `PipeRunner`, anything the parser prints to stderr is folded into that statement's response body, so the parent need not drain the child's stderr pipe. For `SocketRunner` the same is true of the connection; the process's own stderr carries only operator messages.
- **Shutdown**: closing the connection ends that session. Any transaction the client left open is rolled back and its locks released, so a client that dies mid-transaction cannot wedge the others. For `PipeRunner`, closing stdin (EOF) additionally flushes all dirty pages and exits(0). Prefer closing over signaling.

There is no framing beyond the `<<END ...>>` line, so the read loop must buffer until it sees a line starting with `<<END ` - on line boundaries, not by pattern-matching inside partial reads.

### Transactions

By default each statement is its own transaction and auto-commits.
To span several statements:

```sql
set transaction read write;
insert into accounts values (1, 100);
insert into audit values (1, 100);
commit;
```

`rollback;` aborts instead. Note the opening keyword is `set transaction read write` - the parser's grammar has no `BEGIN` or `START TRANSACTION`.
A transaction stays open on its connection until committed or rolled back, holding its page locks the whole time, so keep them short.

## 5. C client (illustrative, not production code)

This sketch shows one connection driven by one thread, with retry handling. Error recovery, reconnection, and backpressure policy are left out; fill those in for your actual daemon.

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef struct { FILE *rx, *tx; } db_conn_t;

/* --- connect ---------------------------------------------------------- */

int db_connect(db_conn_t *c, const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        return -1;

    c->rx = fdopen(dup(fd), "r");
    c->tx = fdopen(fd, "w");

    /* block for the greeting before returning */
    char line[64];
    if (!fgets(line, sizeof line, c->rx))            return -1;
    if (strncmp(line, "<<READY>>", 9) != 0)          return -1;
    return 0;
}

/* --- one request/response, called only from this connection's thread --- */

typedef enum { DB_OK, DB_RETRY, DB_ERROR } db_status_t;

db_status_t db_exec(db_conn_t *c, const char *sql /* ends in ';' */) {
    fprintf(c->tx, "%s\n", sql);
    fflush(c->tx);

    /* real code accumulates the body into a growable buffer here */
    char line[8192];
    while (fgets(line, sizeof line, c->rx)) {
        if (strncmp(line, "<<END ", 6) != 0)
            continue;                        /* body line */
        if (strstr(line, "ok"))    return DB_OK;
        if (strstr(line, "retry")) return DB_RETRY;
        return DB_ERROR;
    }
    return DB_ERROR;                         /* connection died mid-response */
}

/* --- retry wrapper: invariant 3 ---------------------------------------- */

db_status_t db_exec_retrying(db_conn_t *c, const char *sql, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        db_status_t st = db_exec(c, sql);
        if (st != DB_RETRY)
            return st;
        usleep(50000 << attempt);            /* back off, then resubmit */
    }
    return DB_ERROR;
}
```

To run statements concurrently, give each worker thread its own `db_conn_t` from its own `db_connect`.
Never share one connection between threads (invariant 2).

If a daemon instead wants a fixed pool of connections shared by many producer threads, put the connections behind a bounded queue with one consumer thread per connection.
The queue is the daemon's own concern; the database does not care how work reaches a connection, only that one thread owns it at a time.

## 6. `PipeRunner`: the single-client case

For a daemon that owns its tables and does not want to depend on a shared server process, `PipeRunner` runs as a child process over a pipe pair:

```
java -cp dist/simpledb.jar simpledb.PipeRunner <catalogFile> [--no-recover]
```

Same protocol, same markers, same recovery behaviour.
The whole of Section 2 still applies except that "open more connections" is not available: one child process serves one caller, so a `PipeRunner`-based daemon needs the many-producers/one-consumer queue described at the end of Section 5.

The constraint that makes this the special case: **table ownership must be strictly disjoint.**
Every table must be written by exactly one `PipeRunner` process and read only by that same process, for the reason in Section 1.
Do not run one `PipeRunner` per table either - if a daemon owns `foo` and `bar`, one process serves both; splitting multiplies JVM startup cost and shares nothing.

The moment two daemons need the same table, switch them both to `SocketRunner`.
Do not patch around it with an external file lock or a shared mutex file; those coordinate access but do nothing about the stale pages already sitting in each JVM's BufferPool.

## 7. Operational notes

- **Build**: `ant dist` from the project root produces `dist/simpledb.jar`, which includes both runners.
- **Adding a table**: convert source data with `java -jar dist/simpledb.jar convert file.txt <numFields>` to produce `file.dat`, then add a line to `catalog.txt` of the form `tablename (field1 type1, field2 type2, ...)`. The `.dat` file is resolved as `<tablename>.dat` in the same directory as `catalog.txt` (see [`Catalog.loadSchema`](../src/java/simpledb/common/Catalog.java:160)). There is no `CREATE TABLE` statement.
- **Known limitations**: one statement per line; no `UPDATE` statement (delete then insert); no `CREATE TABLE`; the catalog is read once at startup, so adding a table means restarting the runner.
