# ADR 0003: resolving the project root

Status: accepted.
Context: [issue #60](https://github.com/JediNakDev/tetriSH/issues/60).
Amends [ADR 0002](0002-starting-the-shared-socketrunner.md) in two places; supersedes nothing.

## Decision

Every path tetriSH opens is resolved against one root, worked out by one helper in libtetrisutil:

```c
const char *tetrish_root(void);                            /* absolute, validated, or NULL */
int tetrish_path(char *buf, size_t cap, const char *rel);  /* <root>/rel, or rel if absolute */
int rc_load_root(rc_directive_fn fn, void *ctx);           /* directive count, or -1 */
```

Root is `$TETRISD_ROOT`, else the parent of this executable's directory, else the working directory.
Each candidate is made absolute and accepted only if it contains `sample.tetrishrc`.
An explicitly set `$TETRISD_ROOT` that fails that check is a hard error rather than a fall-through, because a human set it deliberately.

`rc_load()` returns the number of directives applied or `-1` when the file could not be opened, and both it and `rc_load_root()` are `warn_unused_result` with `-Werror=unused-result` in `CFLAGS`.
A reader that cannot read `<root>/.tetrishrc` refuses to start if it has a startup to fail at, and otherwise fails the operation that needed the configuration.
The full key set, the owner of each namespace and the per-key missing-versus-unparseable behaviour are in #60's resolution comment.

This ADR records only the three facts that shaped the decision, because each one contradicts something currently written down.

## Finding 1: `resolve_root()`'s middle step has never run on this project's platform

`src/tetrisd/tetrisd.c:245` tries `$TETRISD_ROOT`, then the parent of `/proc/self/exe`, then `"."`.

There is no `/proc` on macOS.
The development machine is darwin and CI is `macos-latest` ([`.github/workflows/ci.yml:13`](../../.github/workflows/ci.yml)), so the `readlink` has always failed and every run has taken the `"."` fallback.
The comment above the function describes step 2 as "what makes `dspawn tetrisd` work without the caller knowing anything", and on the only platforms this project is built on it does nothing at all.

The consequence is not merely that a step is dead.
The `setenv` at `:267` sits inside that fallback branch, so tetrisd exports `TETRISD_ROOT="."` - a *relative* root, meaningful only against a working directory the daemon inherited and does not control.
Session processes read that variable (`session.c:327`) and treat any non-empty value as an explicit operator override, so the child cannot resolve better than its parent guessed and has no way to know it is holding a guess.
Under `dspawn`, which does `chdir("/")`, that root is `/`.

So root resolution already had this ticket's bug, one level above the rc file: it succeeded silently with a wrong answer and then cached the wrong answer into the environment.
That is why the helper resolves root rather than the rc path, and why it validates the answer instead of taking the first candidate.

## Finding 2: the documented launcher is `dspawn2`, so the `chdir("/")` premise is narrower than the map assumed

#48 introduced `$TETRISD_ROOT/.tetrishrc` to work around `dspawn`'s `chdir("/")`, and #52, #55 and #58 all reasoned from that premise.

`README.md:146` starts both daemons with `dspawn2`, and [`dspawn2.c:71`](../../src/tetrish/system_programs/dspawn2.c) carries an explicit `no chdir("/")` comment.
The README's own table already lists `.tetrishrc` as the first thing `dspawn` breaks.

This does not retire the trap, it relocates it.
Under `dspawn2` the bare `RC_PATH` finds the right file *because the working directory happens to be the project root*, which is a property of how an operator started the process rather than of the code.
`src/tetrislogd/main.c:107` and `src/libtetrisutil/logmsg.c:148` both open bare `RC_PATH` today, and both are correct only for as long as that holds.

Correctness by inherited working directory is standing constraint 1's named failure mode: it is an invariant the reader has to hold in their head, and nothing in either file states it.

## Finding 3: a missing rc file and a file with nothing to say are the same answer

`rc_load()` returns `void` and documents a missing file as "not an error - fn is simply never called" ([`include/libtetrisutil/rc.h:32`](../../include/libtetrisutil/rc.h)).

That is a reasonable rule for a file shared by independent readers, where most directives belong to somebody else.
It is the wrong rule for the file's *absence*, because the caller cannot distinguish it from a file that simply carried nothing in its namespace.
The result is that a reader pointed at the wrong path applies zero directives, keeps every built-in default, reports nothing, and runs to completion looking healthy on a configuration nobody wrote.

The fix is one `int` and a compiler flag rather than a mechanism, and it is deliberately not a hard failure inside libtetrisutil: `bin/session` must degrade rather than die, since a guest reaching a playable game with the database absent is #52's invariant A and configuration absent is the same kind of outage.
The library reports; the reader decides; the compiler refuses to let the reader not look.

## Amendments to ADR 0002

1. **Step ordering wording.** The rc line is sequenced after `dspawn2 tetrislogd` and before `dspawn2 tetrisd`. ADR 0002 and #52 both say `dspawn`, which is the daemoniser the project stopped using and which leaves no process running.
2. **`start` and `check` gain a preflight refusal.** Both refuse on a missing or unresolvable `.tetrishrc`, on any unparseable value in a key they read, and on any unknown `db_*` or `auth_*` key. `tetrisdb start` is the validator for both namespaces because it already links libtetrisdb and libtetrisauth and runs before `tetrisd` with a human watching, which makes it the one moment where a typo in the shared file can be reported to somebody.
