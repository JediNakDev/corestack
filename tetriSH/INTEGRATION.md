# Integrating with tetriSH services

# Log integration

Send every interesting event to `tetrislogd` instead of writing your own file.
Centralising matters because tetriSH forks processes freely: a dozen of them
appending to one file would interleave half-written lines, while one datagram
is one indivisible record and exactly one process does the writing.

## What you get

- Timestamped, level-tagged lines in `var/log/tetrisd.log`.
- Optionally the same records as rows in SimpleDB (see section 2 - that is
  `tetrislogd`'s business, not yours; you send the same datagram either way).
- Sender-side safety: text is truncated, control characters are stripped by
  the daemon, and a dead daemon costs a dropped record, not a blocked write.

## Minimum integration

```c
#include "libtetrisutil/logmsg.h"

int main(void) {
    log_open("var/run/tetrislogd.sock");     /* once, at startup */

    log_send(LOG_INFO,  "listening on port %d", port);
    log_send(LOG_WARN,  "room %d full, refusing player %d", room, id);
    log_send(LOG_ERROR, "cannot open %s: %s", path, strerror(errno));
    log_send(LOG_DEBUG, "tick %lu", n);

    log_close();                             /* once, at exit */
}
```

Link with `-ltetrisutil`. There is nothing else to build: the wire format is a
fixed-size struct in the header, so both ends agree at compile time.

## Reading the socket path from `.tetrishrc`

`log_open()` does not read the `log_ipc` directive - it only reads
`log_send_attempts`.
If your daemon should honour the configured socket rather than a hardcoded
path, read it yourself:

```c
#include "libtetrisutil/rc.h"

char sock[PATH_MAX];
/* .tetrishrc if it says, the fallback if it does not */
(void)rc_get("log_ipc", "var/run/tetrislogd.sock", sock, sizeof sock);
log_open(sock);
```

`rc_get` never shows you a line you did not ask for, which is why several
independent readers share one `.tetrishrc` without stepping on each other.

## Rules

**Never check the return value to decide what to do next.**
`log_send` returns -1 when a record was dropped, and that is information for a
shutdown report, not a branch.
Logging that changes program behaviour is worse than no logging.

**After `fork` you need nothing; after `exec` you need `log_open` again.**
The socket is `FD_CLOEXEC`, so an exec'd program starts with no sender and
must open its own. A plain forked child keeps the parent's socket and can use
it as is: `log_send` stamps `getpid()` on every call, so the child's records
carry the child's pid without any re-opening.

A process that never calls `log_open` drops every record silently - there is
no built-in default path to fall back on. If your records are missing, check
that `log_open` ran and returned 0 before you look anywhere else.

**Keep messages under 255 characters.**
`LOG_MSG_MAX` is 256 including the NUL, and longer text is truncated. If the
same record is also mirrored into SimpleDB, the stored copy is cut at 128
characters (`Type.STRING_LEN`), so put the identifying part first.

**Call `log_close()` before you exit.**
Besides releasing the socket, it hands `tetrislogd` any drops it has not been
told about yet. Skip it and your process's last few dropped records go
uncounted - the one time the count matters most.

**Report drops at exit too, if you like.**

```c
unsigned long lost = log_dropped();
if (lost > 0)
    fprintf(stderr, "myd: %lu log records dropped\n", lost);
```

This is the local view, and it is optional: the daemon learns the same numbers
on its own (every record carries the count of what was lost before it) and
writes a summary line per interval. A non-zero count means the log is
incomplete, which is worth knowing before you trust it while debugging.

## Levels

| Level       | Use for                                                 |
| ----------- | ------------------------------------------------------- |
| `LOG_ERROR` | something failed and the feature did not happen         |
| `LOG_WARN`  | something unexpected, handled                           |
| `LOG_INFO`  | lifecycle: started, stopped, client joined, round ended |
| `LOG_DEBUG` | per-tick or internal per-message detail                 |

`log_level` in `.tetrishrc` filters at the daemon, so `LOG_DEBUG` costs a
datagram even when it will be discarded. Do not put `log_send` inside a 20 Hz
inner loop unless you mean it.

## What `tetrisd` must log

Not a suggestion: the brief states these five categories and calls the list
exhaustive ("Every connection event, secure session establishment, HTTTP
request and response, room state change, and admin action **must** be logged
with a timestamp"). The timestamp is not your problem - `tetrislogd` stamps
every record on arrival. Getting a `log_send` to each of these sites is.

| #   | Category                   | Where it belongs                                      | Level   |
| --- | -------------------------- | ----------------------------------------------------- | ------- |
| 1   | Connection event           | after `accept()` and on session teardown              | `INFO`  |
| 2   | Secure session established | after the handshake completes, before the first frame | `INFO`  |
| 3   | HTTTP request and response | boundary record with method + path + status           | `INFO`  |
| 4   | Room lifecycle change      | join, leave, start, game over, round reset            | `INFO`  |
| 5   | Admin action               | every `tetrisctl` command that changes something      | `INFO`  |

Notes that save a round of rework:

- **HTTTP boundaries at `INFO`, internal gameplay work at `DEBUG`.** This keeps
  player-visible requests observable while the 20 Hz mechanics remain
  filterable with `log_level`.
- **A failed action is still an event.** A rejected `JOIN` or a refused admin
  command is worth an `ERROR`/`WARN` record; only logging successes hides the
  interesting half.
- **Log one completion around the real operation.** A connection record belongs
  after `accept()` returns, so it describes an event that actually happened and
  includes the final status.
- **The forked session worker logs for itself.** `bin/session` is `exec`ed, so
  it starts with no sender and must call `log_open()` of its own - categories
  2, 3 and part of 4 are its records, not the parent's.

> **`bin/session` already opens its own sender** (`log_open_configured()` at
> the top of `main`), which also switched on the ~20 `log_send` calls
> `libtetrisauth` makes on the login path - until that landed, every one of
> them was discarded, because a sender with no socket path fails
> `sender_connect()` with `EINVAL` and counts a drop in a process that never
> reports its count. A passing build and an empty log. Add your `log_send`
> calls below that line and they work.

The parent daemon and each session worker both report; `pid` distinguishes
them in the file, and one datagram per record means their lines never
interleave.

## What happens when `tetrislogd` is not running

`log_send` fails, counts a drop, and returns. Your process is unaffected.
When the daemon comes back, the sender reconnects transparently on its next
call (that is what `log_send_attempts` controls, default 2 attempts).
You do not need reconnect logic.

The records lost meanwhile are not forgotten: the first one that gets through
carries the count with it, so the gap shows up in the daemon's log as
`dropped N records in last 30s` rather than as silence.

## Testing your integration

```sh
./bin/tetrislogd                  # foreground; log_path = - in .tetrishrc for stdout
```

Run your daemon against it and watch the records arrive. `tests/test_logd.c`
is a worked example of driving the real daemon from a test.

---

# 2. Database integration

`libtetrisdb` gives a daemon a SimpleDB table it owns.
SimpleDB is a Java library with no server, so the way in from C is to run a
runner as a separate process and speak a line protocol to it.
The library is that process, its pipes, a queue, and the one thread allowed to
drive them.

Read `db/docs/c-daemon-integration.md` once before you start. The two
invariants below come from it and are correctness requirements, not style.

**This section is about `tdb_t`, which never blocks and never fails loudly:
it drops statements rather than stall a daemon, and reading a result after
startup is not supported.** That is the right contract for logging and the
wrong one for anything a user is waiting on. The opposite contract lives in
the same library as a separate type, `tdb_socket_t` - one connection to the
shared `SocketRunner`, blocking, with a deadline and a readable result. If you
need an answer rather than a best-effort write, read
`include/libtetrisdb/socket/db.h` instead of this section; the two share the
wire and nothing else. (`include/libtetrisdb/socket/runner.h` starts the runner
that path connects to, but that is `bin/tetrisdb`'s job once per machine, not
yours.)

## The two invariants

**1. One table has exactly one owning process.**
Each `PipeRunner` caches pages in its own private `BufferPool` with no
cross-process invalidation. If your daemon writes a table another process has
ever read, one of them is serving stale data, and if both write it, the file
is corrupt. Flushing does not help: SimpleDB's FORCE-on-commit guarantees the
writer's durability, not the other process's visibility.

Concretely: `tetrislogd` owns `log`. Your daemon must never `select` from it.
If you want log data, send a datagram (section 1) and let `tetrislogd` write.

**2. One thread per pipe.**
The protocol has no request ids, so two threads writing statements interleave
on the wire and responses become unattributable. The library enforces this for
you: `tdb_submit()` is a queue push callable from anywhere, and a private
worker thread is the only thing that touches the pipe. Do not go around it.

## Step 1: name your own directory and table

```
var/db_log/    catalog.txt + log.dat        owned by tetrislogd
var/db_game/   catalog.txt + match.dat      owned by tetrisd
var/db_<you>/  catalog.txt + <table>.dat    owned by you
```

The directory is named after its owner on purpose, and `tdb_opts_default()`
leaves `dir` **empty** so you cannot accidentally inherit somebody else's.
Sharing one `catalog.txt` between daemons is allowed by the upstream document
as long as tables are disjoint, but a separate directory per daemon removes a
whole class of mistake: two daemons appending to one catalog at startup can
interleave lines, and nobody can accidentally read the wrong table.

You lose nothing by separating: cross-table joins are impossible anyway,
because each daemon is a different process with a different `BufferPool`.

## Step 2: choose a schema you can live with

```c
#define GAME_DB_DIR    "var/db_game"
#define GAME_DB_TABLE  "match"
#define GAME_DB_SCHEMA "id int, room int, winner int, score int, ts int"
```

Constraints that come from SimpleDB, not from this library:

| Want                                | Reality                                                                           |
| ----------------------------------- | --------------------------------------------------------------------------------- |
| `DATETIME`                          | Does not exist. Store epoch seconds as `int`.                                     |
| `VARCHAR(n)`                        | Only `string`, fixed at 128 characters, silently truncated.                       |
| `PRIMARY KEY`, `NOT NULL`, `UNIQUE` | Not enforced. A `pk` annotation is parsed but constrains nothing.                 |
| `CREATE TABLE`                      | No DDL at all. A table is a catalog line plus a `.dat` file.                      |
| `ALTER TABLE`                       | Does not exist. Changing a schema means deleting the `.dat` and the catalog line. |
| Multi-statement transactions        | Each line auto-commits as its own transaction.                                    |

Get the schema right before you have data, because there is no migration path.

## Step 3: create the table before starting the child

```c
#include "libtetrisdb/schema.h"    /* tdb_ensure_table, tdb_quote  */
#include "libtetrisdb/pipe/db.h"   /* tdb_t and everything below   */

if (tdb_ensure_table(GAME_DB_DIR, GAME_DB_TABLE, GAME_DB_SCHEMA) < 0)
    /* report and carry on without the database */;
```

Two headers, because creating a table and quoting a literal are not about a
transport: they are equally true for the socket path in
`include/libtetrisdb/socket/db.h`, so they do not live in either transport's
header.

Idempotent: an existing table is left completely alone, including its data.
It must happen **before** `tdb_start()`, because `PipeRunner` reads the catalog
once at JVM startup and never rereads it.

## Step 4: start it before you start any threads

```c
tdb_opts_t opts;
tdb_opts_default(&opts);
snprintf(opts.dir, sizeof(opts.dir), "%s", GAME_DB_DIR);

char body[1024];
tdb_t *db = tdb_start(&opts, "select max(id) from " GAME_DB_TABLE ";",
                      body, sizeof(body));
if (db == NULL)
    /* report and carry on without the database */;
long next_id = /* parse body: the value after the line of dashes, else 1 */;
```

The probe is the one statement whose output you can read, and it is safe only
here, before any producer thread exists. Use it to recover a sequence counter
or a high-water mark across restarts. On an empty table it returns no value,
so have a fallback.

`tdb_start` blocks for a JVM startup, roughly a second. Do it once, at
startup, never per statement, and never after your threads are running.

## Step 5: submit from wherever, cheaply

```c
char quoted[300], sql[512];
tdb_quote(quoted, sizeof(quoted), player_name);   /* untrusted text */
snprintf(sql, sizeof(sql),
         "insert into " GAME_DB_TABLE " values (%ld, %d, %d, %d, %ld);",
         next_id++, room, winner, score, (long)time(NULL));
(void)tdb_submit(db, sql);                        /* best effort, never blocks */
```

**Every string that did not come from your own source code goes through
`tdb_quote()`.** It wraps the text in quotes and doubles any quote inside, so
a crafted value stays one literal instead of becoming a second statement.
Numbers formatted with `%d` need nothing.

`tdb_submit` copies the string, so a stack buffer is fine. It returns -1 when
the queue is full or the connection is dead; treat that like `log_send`'s
return value - count it, do not branch on it.

## Step 6: shut down cleanly, or lose data

```c
unsigned long dropped, errors;
tdb_stop(db, &dropped, &errors);
```

`tdb_stop` drains the queue, then closes the child's stdin, which is what makes
SimpleDB flush its dirty pages. **This is the only supported shutdown.**
`SIGKILL` on the daemon skips it entirely, which is why `dspawn2` stops
daemons with `SIGTERM` and why you must not `kill -9` a daemon that owns a
table.

That means your daemon needs a real shutdown path: a `SIGTERM` handler that
sets a flag, loops that notice it, and a `tdb_stop` after the threads are
joined. `tdb_stop` blocks and uses locks, so it can never be called from the
signal handler itself.

## Step 7: run it under `dspawn2`

```sh
./bin/dspawn2 bin/myd
kill $(cat var/run/myd.pid)
```

`dspawn` cannot be used: it does `chdir("/")`, so `var/db_<you>` and
`db/dist/simpledb.jar` stop resolving, and it discards stderr so you never
find out. `dspawn2` also refuses to start a second copy, which is the only
thing standing between invariant 1 and a corrupt `.dat`.

## Fork safety

If your daemon forks after `tdb_start()`:

- **fork + exec** is safe. The pipe descriptors are `FD_CLOEXEC`, so the child
  loses them at `exec`. This is what `tetrisd` does for `bin/session`.
- **fork without exec is not safe.** The child inherits the pipe fd but not
  the worker thread (`fork` copies only the calling thread), so it can write
  into the pipe with nobody reading, and it may inherit a mutex that was
  locked at the moment of the fork. If you need this, `tdb_stop()` before
  forking, or make sure the child never touches the handle.

## Configuration

Follow `tetrislogd`'s pattern: prefix your keys and ask for each one by name
with `rc_get`, `rc_get_int` or `rc_get_bool`. Anything with a
shape of its own - a level, an enum - you parse yourself, next to the code that
has to be right about it. Size an ipc buffer `MAX_IPC_PATH`
(`libtetrisutil/limits.h`) and "too long to bind" is caught for you.

```
game_db       = on
game_db_dir   = var/db_game
game_db_jar   = db/dist/simpledb.jar
game_db_java  = java
game_db_queue = 256
```

Keep the database opt-in and off by default. It needs a JVM and the jar, while
the rest of your daemon does not, and a machine without java should still run
everything else.

## Checklist

|                                              |                                   |
| -------------------------------------------- | --------------------------------- |
| Own directory, not shared                    | `var/db_<you>/`                   |
| Table written by you only, read by you only  | never touch `log`                 |
| `tdb_ensure_table` before `tdb_start`        | catalog is read once              |
| `tdb_start` before any thread                | the probe depends on it           |
| `tdb_quote` on every outside string          | injection guard                   |
| `tdb_submit` result counted, not branched on | best effort                       |
| `SIGTERM` handler that reaches `tdb_stop`    | otherwise pages are never flushed |
| Started with `dspawn2`                       | cwd, stderr, single instance      |
| Schema settled before real data exists       | there is no `ALTER TABLE`         |

## Worked example

`tetrislogd` is the reference implementation, and small enough to read in one
sitting:

| Step                         | Where                                                    |
| ---------------------------- | -------------------------------------------------------- |
| options and rc keys          | `src/tetrislogd/config.c`, `include/tetrislogd/logger.h` |
| directory default            | `DEFAULT_DB_DIR` in `src/tetrislogd/main.c`              |
| create, start, seed the id   | `mirror_open()` in `src/tetrislogd/sink.c`               |
| build and submit a statement | `mirror_write()` in `src/tetrislogd/sink.c`              |
| clean shutdown and counters  | end of `logd_run()` in `src/tetrislogd/sink.c`           |
| tests, including injection   | `tests/test_db.c`                                        |
