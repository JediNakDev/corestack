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
