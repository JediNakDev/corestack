# ADR 0004: what Phase 4 changed about ADRs 0001 to 0003

Status: accepted.
Context: the `jedi/auth` branch - [#44](https://github.com/JediNakDev/tetriSH/issues/44), [#52](https://github.com/JediNakDev/tetriSH/issues/52), [#53](https://github.com/JediNakDev/tetriSH/issues/53), [#55](https://github.com/JediNakDev/tetriSH/issues/55), [#58](https://github.com/JediNakDev/tetriSH/issues/58), [#60](https://github.com/JediNakDev/tetriSH/issues/60).
Amends [ADR 0002](0002-starting-the-shared-socketrunner.md); confirms [ADR 0001](0001-tetrislogd-stays-on-piperunner.md); supersedes [ADR 0003](0003-resolving-the-project-root.md).

## Decision

Phase 4 changed five things that ADRs 0001 to 0003 had written down.
They are recorded here, in one place, rather than by editing those three.

That is the point of this ADR.
While the work was in flight the three earlier records were edited in place to match the code: ADR 0003 was rewritten from "accepted" to "rejected before full implementation" with its three findings deleted, ADR 0002 grew a self-authored implementation note, and both 0001 and 0002 had their file references silently updated.
Those edits have been reverted.
An ADR is what was decided and why, not what the tree currently does, and a decision record that is rewritten to agree with the code cannot be used to review the code.

That applies to ADR 0003 in both directions, which change 4 is about.
Its decision is wrong and is reversed here; it is not rewritten there.
ADR 0003 stays as it was accepted, findings and all, because a reader needs to see what was decided in order to judge what this ADR says about it.

## Change 1: `tetrisdb.h` is now `pipe.h`

`core/include/libtetrisdb/tetrisdb.h` is `core/include/libtetrisdb/pipe.h`.

libtetrisdb grew a second contract in #44, so a header named after the library no longer said which of the two it was.
The names now match the split: `pipe.h` is a child process and a lossy queue, `socket.h` is one connection under a deadline.

ADR 0001 and ADR 0002 both refer to the old name.
They are correct as written and are left alone; this line is the forwarding address.

Both also ask that `tetrisdb.h`'s claim about closing stdin being "the only shutdown path that flushes dirty pages" be corrected where it stands.
That correction is made, in `pipe.h`.

## Change 2: `tdb_status_t` belongs to the wire, not to a transport

The two transports share the line protocol (`db/docs/c-daemon-integration.md` section 4), and `core/src/libtetrisdb/wire.c` implements it once for both.

Its header reached the statement-outcome enum by including `libtetrisdb/socket.h`, so `pipe/proc.h` transitively saw `tdb_socket_t`, `tdb_socket_exec` and `tdb_row_count`.
Both `wire.h` and `proc.h` carried comments claiming the compiler enforced the separation between the two paths.
It did not: the dependency ran one way, and only one way.

`tdb_status_t` now lives in `core/include/libtetrisdb/status.h`, which both transports and the wire include, and neither transport's header includes the other's.
The markers are the protocol, so the enum belongs to the protocol.

## Change 3: steps 6 to 8 of ADR 0002 live in libtetrisdb, not in the launcher

ADR 0002's step list reads as though `bin/tetrisdb` writes all eight steps.
It does not.
`tdb_runner_spawn()` and `tdb_runner_wait()` in `core/include/libtetrisdb/runner.h` own the argv, the fork, the `setsid` and the readiness poll.

The deciding reason is ADR 0001's own condition for accepting two runners at all: resolving and validating the `java` and jar pair belongs in `libtetrisdb`, shared with `tdb_start()`, so the version trap has one home.
Keeping the spawn in the launcher would have put one of the two spawn paths outside the library and given that trap two homes again.

The launcher keeps everything that is true of a tetriSH installation rather than of starting a runner: the lock, the `log`-table refusal, the semaphore, `tdb_ensure_table()`, `tauth_secret_provision()`, the socket unlink, the exit codes and the report.

Two consequences carry:

- The lock from step 1 reaches the JVM because `tdb_runner_spawn()` closes nothing but the standard descriptors, so the caller's lock fd must not be `FD_CLOEXEC` and must not be fd 0, 1 or 2.
  This is a prose obligation across a seam, and it forces an `F_DUPFD` dance in `core/src/tetrisdb/main.c`.
  It is a cost, not a design: see "Revisit if" below.
- `tests/test_db.c` starts its runner through the same two functions rather than its own `fork`/`exec`, so the argv the tests exercise is the argv the launcher uses.

## Change 4: ADR 0003's decision is wrong, and is reversed

ADR 0003 decided that "every path tetriSH opens is resolved against one root, worked out by one helper in libtetrisutil", and named `tetrish_root()`, `tetrish_path()` and `rc_load_root()`.

That decision is reversed.
There is no `core/include/libtetrisutil/root.h` and no `core/src/libtetrisutil/root.c`, and no reader resolves a root.

Its three findings are not what is being reversed.
Findings 1 and 2 are facts about this tree and are still true; finding 3 is right and is kept below.
What does not follow from them is the remedy, for three reasons.

### It does not produce one root, it produces one more

The premise is "one root, worked out by one helper".
The helper was built, and the tree then held three: `resolve_root()` at [`tetrisd.c:511`](../../src/tetrisd/tetrisd.c), `ctl_socket_path()` at [`control_plane.h:117`](../../include/tetrisctl/control_plane.h), and libtetrisutil's.

`resolve_root()` is the function finding 1 is *about*: the one whose `readlink("/proc/self/exe")` at `:520` has never run on this project's platforms, and whose `setenv("TETRISD_ROOT", root, 1)` at `:533` caches the resulting `"."` into the environment.
ADR 0003 diagnosed it and left it running.
libtetrisutil's resolver then took `$TETRISD_ROOT` as its first and highest-priority candidate, so under `dspawn tetrisd` the new resolver's answer was the old resolver's bug, arriving through the environment and relabelled "absolute, validated".

A finding about a broken resolver is an argument for fixing or deleting that resolver.
It was used as an argument for adding a second one beside it, and the first one is still there.

### Finding 2 argues against the decision, not for it

Finding 2 establishes that the supported launcher is `dspawn2` and that `dspawn2` does not `chdir("/")`.
That is the finding that removes the problem rather than relocating it.

ADR 0003 reads it the other way.
It grants that a bare `RC_PATH` is correct under `dspawn2`, then objects that it is correct "because the working directory happens to be the project root, which is a property of how an operator started the process rather than of the code".

That objection applies to every candidate in ADR 0003's own resolution order.
`$TETRISD_ROOT` is a property of how an operator started the process; so is the executable's location; so is the working directory.
No scheme derives the root from anything but how the process was launched.
The resolver only changes which launch mistake gets named, and it buys that with a marker-file convention, a `_NSGetExecutablePath`/`/proc/self/exe` split and 235 lines.

### The validation's only exercised path was its own escape hatch

The marker is `sample.tetrishrc`: a directory is a tetriSH checkout when it contains one.

The two suites that isolate themselves then had to plant a fake `sample.tetrishrc` in their fixture directory and export `$TETRISD_ROOT` at it to get back out of the mechanism.
`tests/test_auth.c` already `chdir()`ed into that directory and needed neither.
A check whose only exercised path is the override is not catching an operator error; it is charging the tests for a guarantee production never collects.

### What is kept

Finding 3 stands on its own and needs no root resolved.
`rc_load()` keeps its `int` return and its `warn_unused_result`, and `-Werror=unused-result` stays in `CFLAGS`, so a missing rc file is a refusal in `bin/tetrisdb start` and a 500 in the auth path rather than a silent run on every built-in default.

Readers open `RC_PATH` relative to the working directory.
`bin/tetrisdb` passes `"."` to `tauth_secret_provision()` and `tauth_secret_load()`; those two keep their `root` parameter, which is what lets the fixtures aim them at a directory without a production API existing to be overridden.

The cost is accepted and is ADR 0003's own: a wrong working directory is not detected at startup and fails at the first `open()` instead.
Under `dspawn2` that is a launch nobody supports.
Under `dspawn` it is finding 1's bug, which belongs to `resolve_root()` and should be fixed where it lives.

## Change 5: #53's one file pair is restored

`core/src/libtetrisauth/jwt_helper/` is gone.

base64url and the JSON reader are statics inside `core/src/libtetrisauth/jwt.c`, which is what #53 decided: "One file pair, `core/src/libtetrisauth/jwt.c` + `core/include/libtetrisauth/jwt.h`, with base64url and the JSON layer as statics inside it."
The split had exported `json_iter_t`, `json_member_t` and `json_kind_t` across two headers with one consumer each, against the same ticket's "never builds a value model".

`bin/tests/test_jwt` still compiles the source and links `-lcrypto` only, so the portability claim stays a build failure rather than a review comment.

## Consequences

- `rc_load()` gains a typed layer, `rc_bind()`, because three readers - `core/src/tetrislogd/config.c`, `core/src/libtetrisdb/socket/runner.c` and `core/src/libtetrisauth/authconf.c` - had each rebuilt the same whole-string `strtol`, range check, fixed-size copy and first-bad-value report from the string callback.
  A reader declares an `rc_key_t` table instead of writing a callback.
  All three are converted.
- The player-name rule has one owner, `core/include/libtetrisutil/playername.h`.
  It was written three times and the three disagreed: the client used `isalnum()`, which is locale-sensitive, so the client could accept a name the server then refused.
  `jwt.c` keeps a deliberate fourth copy, because it may include nothing outside OpenSSL.
- The `db_` namespace's defaults and bounds are in `core/include/libtetrisdb/dbconf.h`, once.
  They had been written three times across two libraries, with a comment in `authconf.c` saying plainly that nothing checked the copies agreed.
  A key its owner validates but does not consume - `db_timeout` for the launcher, `log_send_attempts` for tetrislogd - is `check_only` in the key table rather than a local called `ignored`.

## Revisit if

- The lock fd contract in change 3 causes a bug.
  It is the only part of ADR 0002's mechanism that crosses a seam as prose, and the alternative - `runner.h` owning the lockfile and the whole 1 to 8 ordering, with the launcher rendering a returned result - would also make the ordering testable by a call instead of by `tests/test_tetrisdb.c` re-encoding it as exit codes 20 to 28.
- Anything outside `bin/tetrisdb` needs a second `SocketRunner` spawn path, which is the condition change 3 was decided against.
- A launcher that does not preserve the working directory becomes supported, which is the single premise change 4 rests on.
  That is the argument that reopens ADR 0003, and the only one.
- `resolve_root()` in `src/tetrisd/tetrisd.c` is fixed or deleted, which is where finding 1's bug actually lives.
  Until then this project has one broken root resolver rather than one broken and one correct, and change 4's third reason is the record of why adding the second did not help.
