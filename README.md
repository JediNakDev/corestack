# corestack

Two systems built on one core.

- **[tetriSH](tetriSH/)** - a multiplayer Tetris server, client and control-plane dashboard.
- **[ballotbox](ballotbox/)** - a secure electronic voting system.

They started as separate repositories and grew the same foundations: a shell, a
logging daemon, a database daemon, and the libraries underneath all three.
Those foundations now live once, in `core/`, and both projects compile against
that single copy.

## Layout

```
corestack/
├── core/                  the code both projects share - one copy, no forks
│   ├── include/           libhtttp, libtetrisauth, libtetrisdb,
│   │                      libtetrissh, libtetrisui, libtetrisutil
│   └── src/               those six libraries, plus three programs:
│                          tetrish (the shell), tetrisdb, tetrislogd
├── db/                    SimpleDB - the Java storage engine both use
├── external/              vendored coursework the projects build against
├── shared.mk              the build rules for everything in core/
├── tetriSH/               libtetrisbrain, tetrisd, tetrisctl, tetrisu
└── ballotbox/             libballotbrain, libballotclient, ballotd,
                           ballotctl, ballotu
```

Each project keeps its own `bin/`, `lib/`, `obj/`, `auth/`, `var/` and
`.tetrishrc`: they are two running systems with their own daemons, ports and
state, not two build configurations of one. `db` inside each project is a
symlink to the shared `db/` at the root, which is what keeps the `db/dist/
simpledb.jar` paths baked into the C sources and the test fixtures resolving.

## Building

```bash
cd tetriSH && make all
```

```bash
cd ballotbox && make all
```

`make` at the root does both. Each project's own Makefile is where its targets
live - `make test-fast`, `make start`, `make fuzz-regress` and the rest are
documented in the project READMEs.

## How the sharing works

`shared.mk` is included by each project's Makefile and compiles `core/src`
**per project**, into that project's `obj/shared/`. The same file becomes two
different objects, because each is built with that project's flags and, more
importantly, against that project's `include/`, which is searched **before**
`core/include`.

That search order is the seam. Where the shared code needs an answer only the
owning project can give, it includes a header that only the project provides:

- `core/src/tetrisdb/main.c` includes `tetrisdb/provision.h` for the list of
  tables to create at startup. tetriSH's copy adds `history`; ballotbox's adds
  nothing, because `ballotd` provisions its own six tables.

Anything that needs to differ between the two belongs behind a seam like that
one - not behind an `#ifdef`, and never behind a second copy of the file.

The flip side is that `core/include` has no seam at all, and a constant there
is a decision made for both projects at once. Raising `MAX_SESSIONS` from 254
to 1024 in `libtetrisutil/limits.h` was a tetriSH change, but `tetrisdb` bounds
its `db_sessions` directive by that same constant, so ballotbox now accepts a
wider range for a setting nobody there asked about. Small and harmless this
time. Worth a look before the next one.

## Secrets

`auth/private_key.pem`, `auth/server_signed.crt` and `auth/certificate_request.csr`
were tracked in both projects until 2026-09-14. `tetriSH/auth/.gitignore` had
said to exclude them since long before that, which changed nothing, because
gitignore does not apply to files that are already committed. They are untracked
now, ballotbox has the same rule, and `make cert` provisions a local chain so a
fresh clone still runs - see `scripts/provision_auth.sh`.

**The key is still in history**, across 24 commits, on a public remote.
Untracking does not undo that and nothing here pretends otherwise. Rotating is
not possible, since the certificate was signed by a course CA nobody here holds
the key for. What is true is that the certificate expired on 2026-09-05 and the
CA is shared with the whole cohort, so what leaked is a dead key to a server
that no longer verifies. If you want it gone properly, that is `git filter-repo`
and a force push, which rewrites every hash and needs the whole team to reclone.

`auth/jwt_secret` was never tracked. `ballotbox/tests/fuzz/jwt_fuzz_secret.h`
is a fuzzing fixture, not a credential, and its bytes literally spell
`fuzz-only-secret-not-a-real-key`.

## Performance

tetriSH carries the load and latency work: `tetriSH/tests/bench_report.sh`
measures p50/p99/p99.9 and finds the rate at which p99 leaves its SLO. Results
and the reasoning behind the method are in
[tetriSH's README](tetriSH/README.md#concurrency-and-performance). Short
version: 0.5 ms p99 at 254 clients, and a knee around 30k req/s where a 1%
increase in offered load triples the p99.

Nothing equivalent exists for ballotbox yet. The harness is not tetriSH-shaped
by accident, though - the probe is one request that gets one reply, which
`ballotd` also has.
