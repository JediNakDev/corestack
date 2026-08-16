# ADR 0004: tetrisctl grows a TUI and a lifecycle role

Status: accepted.
Context: no issue; settled in a design review, recorded in [TETRISCTL_PLAN.md](../../TETRISCTL_PLAN.md).
Supersedes nothing; notes an unpaid debt in [ADR 0003](0003-resolving-the-project-root.md).

## Decision

`bin/tetrisctl` keeps its argv CLI unchanged and gains two things.

A **live console**, opened by a bare invocation on a tty: a hand-drawn dashboard
above a fixed action list, refreshed once a second, with `libtetrisui` widgets
used as full-screen pages for forms, confirmations and progress. It never exits
on its own — a lost daemon greys the last snapshot and retries on a backoff.

A **lifecycle role**: `tetrisctl start|stop tetrisd|tetrislogd`, available from
both faces. Starting execs `bin/dspawn2` and confirms by observation. Stopping
tetrisd sends `SHUTDOWN` over the existing control plane. Stopping tetrislogd,
which has no control channel, sends `SIGTERM` to the pidfile pid — but only after
a live `var/run/tetrislogd.sock` corroborates that the pid is what it claims.

`shutdown` survives as an alias for `stop tetrisd`, routed to the same call site.

The full decision set — fifteen locked decisions, eleven assumed defaults, the
file split and the test plan — is in `TETRISCTL_PLAN.md`. This ADR records only
the three facts that shaped it, because each one contradicts something currently
written down.

## Finding 1: "tetrisu is the only binary that draws" stops being true

`Makefile:144` scopes the UI libraries deliberately:

```make
# tetrisu is the only binary that draws, so -ltetrisui/-lncurses are scoped here
# rather than added to the global LDLIBS.
$(BIN_DIR)/tetrisu: LDLIBS += -ltetrisui -lncurses
```

Putting the console in the same binary as the CLI invalidates that sentence.
`bin/tetrisctl` now links both libraries unconditionally, and the comment has to
be rewritten rather than quietly left standing next to a second `LDLIBS` line
that contradicts it.

### Why one binary and not two

A separate `bin/tetrisctltui` would have preserved the scoping exactly, and there
is a real argument for it: `tetrisctl shutdown` is the command that has to work
when the public listener is saturated (`REQUIREMENTS.md:450`), and a
break-glass tool with fewer dependencies is a better break-glass tool.

It was rejected because the seam it creates is worse than the one it removes.
Both faces need the same transport, the same JSON decode and the same lifecycle
code; splitting the binaries means that shared half becomes a library or a set of
duplicated sources, and the tool most likely to be reached for in an emergency
becomes the one whose behaviour is defined in two places. A bare invocation
currently prints usage and exits 2, so attaching the console to it costs nothing
that anything scripted can observe.

The dependency is bounded rather than eliminated: the argv path never calls
`initscr()`, so the cost is link-time and startup-time, not runtime. That is
recorded as a standing risk (R4 in the plan), and separating the binaries stays
available if it ever bites.

## Finding 2: `dspawn2`'s exit status cannot tell you whether the daemon started

`dspawn2` daemonises before it execs, which means its parent
`exit(EXIT_SUCCESS)` fires after the **first** fork
(`core/src/tetrish/system_programs/dspawn2.c:63`), well before `execvp` runs at
line 127. A caller that waits on it learns only that forking worked.

This is correct behaviour for a daemoniser and wrong to rely on. `execvp`
failing, the daemon dying in its own startup, and the daemon coming up healthy
are indistinguishable from the exit status.

So `ctl_start` confirms by observation instead: tetrisd by a `STATUS` that
answers on the control socket, tetrislogd by its pidfile appearing and
`kill(pid,0)` succeeding, both polled to a ~3 s ceiling. The progress panel
reports which of those two steps failed, and a failure points at
`var/log/<name>.err` — where `dspawn2` has been redirecting the daemon's own
complaint since line 82, and where nobody was looking.

### The second trap in the same function

`already_running()` prints to **stderr, in the parent, before any fork**
(`dspawn2.c:22-27`). Called from inside a running ncurses screen with fds
inherited, that text lands on the terminal and corrupts the display.

`ctl_start`'s child therefore `dup2`s a pipe onto fds 1 and 2 as its first act,
before `chdir` and before `execvp`, and the parent drains that pipe and surfaces
its contents through a message box. The most useful line an operator can read —
`already running (pid N)` — is thereby recovered rather than smeared.

The child also `chdir`s to the resolved project root first, because `dspawn2`
deliberately does not `chdir("/")` (line 71) and resolves `var/run/<name>.pid`
against whatever cwd it inherits. A tetrisctl launched from a subdirectory would
otherwise scatter pidfiles where nothing will look for them.

## Finding 3: ADR 0003 is accepted but unimplemented

ADR 0003 specifies that every path in tetriSH resolves against one root via three
helpers in libtetrisutil:

```c
const char *tetrish_root(void);
int tetrish_path(char *buf, size_t cap, const char *rel);
int rc_load_root(rc_directive_fn fn, void *ctx);
```

None of the three exists. A grep for all three names across `src/` and
`include/` returns nothing.

The live idiom is the one in `control_plane.h:117-134`: read
`$TETRISD_ROOT`, fall back to `"."`, then join `.tetrishrc` and `ctl_ipc` by
hand. That is exactly the "fall through to the working directory" behaviour ADR
0003's Finding 1 identifies as a bug, and it is what both ends of the control
plane currently rely on.

This matters here because the lifecycle code needs the root for three new things
— locating `bin/dspawn2`, reading `var/run/*.pid`, and setting the child's cwd —
and so must pick a side.

It follows the live idiom, factored out as `ctl_root()` next to
`ctl_socket_path()`. Two reasons. Writing new code against helpers that do not
exist means writing the helpers, and ADR 0003 is a project-wide change touching
every reader of every path; smuggling it in underneath a TUI feature would make
both changes harder to review and harder to revert. And a `ctl_root()` that
disagrees with `ctl_socket_path()` about where the project is would produce a
tetrisctl that finds the daemon's socket but writes pidfiles somewhere else,
which is a worse failure than sharing a known-imperfect answer.

The debt is recorded rather than paid: when ADR 0003 is implemented,
`ctl_root()` and `ctl_socket_path()` are two of its call sites, and they are
adjacent in one header on purpose.

## Consequence not covered above

`tetrisctl players` already answers 500 on a fully loaded server, and the
dashboard inherits it. `g_body` is 32 KB (`control_plane.c:483`); 254 sessions
(`MAX_SESSIONS`) at up to ~187 bytes each — 16-byte names, six bytes per byte
worst case through `json_escape` — is roughly 47 KB, and `BODY_APPEND` returns
`-1` rather than truncate.

That is the right failure: `control_plane.c:476-481` argues explicitly that half
a JSON document is worse than an error. But it is a failure, it predates this
work, and the console will render it as an error where the player list belongs.
Enlarging `g_body` toward `CTL_MAX_FRAME` or paginating the verb is a separate
change and is not attempted here.
