# AGENTS.md

This file tells AI coding agents how to work in the PA2 Secure FTP project. The student configures an agent to read this file; the agent then knows what it may and may not do.

## What this project is

This is SUTD 50.005 Programming Assignment 2: a Secure FTP application implemented in C with OpenSSL. The starter ships a working `ClientWithoutSecurity` and `ServerWithoutSecurity` plus a shared `common.c` library. The student's task is to implement the secure variants on top of that library:

- **AP**: Authentication Protocol. Server proves its identity to the client using RSA-PSS over a nonce, with a CA-signed X.509 certificate.
- **CP1**: Confidentiality Protocol 1. Client encrypts each file chunk with the server's RSA public key.
- **CP2**: Confidentiality Protocol 2. After AP, parties agree on a symmetric session key; bulk transfer uses AES-128-CBC + HMAC-SHA256.

This is a course assignment. The student must learn the material. You are here only to help draft unit tests, not to write the protocol code.

## Build

The project builds with `make`. Requirements: GCC (or clang) and OpenSSL development headers (`libssl-dev` on Ubuntu, `brew install openssl` on macOS, `openssl-devel` on Fedora). The Makefile auto-detects Homebrew's OpenSSL prefix on macOS.

```bash
make            # builds all client/server variants
make NoSec      # builds only ClientWithoutSecurity + ServerWithoutSecurity
make AP         # builds only the AP variants
make CP1        # builds only the CP1 variants
make CP2        # builds only the CP2 variants
make clean      # removes binaries, resets recv_*/send_* directories
```

## Test

```bash
make unit             # compiles and runs every test under tests/unit/
make integration      # runs every shell script under tests/integration/test_*.sh
make test             # runs both
make ai-unit-tests MODULE=<name>   # invokes scripts/gen_unit_tests.sh
```

Unit tests use the ThrowTheSwitch Unity framework, vendored under `tests/unity/`. Integration tests are bash scripts that run `./ServerWithoutSecurity` and `./ClientWithoutSecurity` as black boxes and check transferred files byte-for-byte.

The integration test server listens on `PA2_TEST_PORT` (default 14321), overridable via environment variable. Do not write unit tests that bind to this port; they will race with the integration suite.

## Code structure

```
source/ClientWithoutSecurity.c           plain FTP client (main)
source/ServerWithoutSecurity.c           plain FTP server (main)
source/ClientWithSecurityAP.c            (student writes; not in starter)
source/ServerWithSecurityAP.c            (student writes; not in starter)
source/ClientWithSecurityCP1.c           (student writes; not in starter)
source/ServerWithSecurityCP1.c           (student writes; not in starter)
source/ClientWithSecurityCP2.c           (student writes; not in starter)
source/ServerWithSecurityCP2.c           (student writes; not in starter)
source/common.h                          shared API (helpers + crypto wrappers)
source/common.c                          shared implementation
auth/cacsertificate.crt           CA certificate (trust anchor)
auth/generate_keys.py             helper to mint server keys / CSRs
tests/unit/test_<name>.c                 unit tests (Unity)
tests/integration/                       bash integration tests
tests/integration/_lib.sh                shared helpers (not a test)
tests/unity/                             vendored Unity framework
scripts/gen_unit_tests.sh                AI test-generation wrapper
prompts/generate-unit-tests.md           prompt template
Makefile                                 build and test rules
```

## Naming convention for unit tests

Two patterns are supported:

1. **A new helper extracted by the student.** If you create `source/<name>.c` and `source/<name>.h`, the matching test file `tests/unit/test_<name>.c` will be linked against that source plus `source/common.c`.
2. **Tests for the bundled `common.c` helpers.** Tests named `tests/unit/test_<group>.c` whose `<group>` does not match any `source/<group>.c` will be linked against `source/common.c` only. Existing examples: `test_int_bytes.c`, `test_socket.c`.

Avoid naming a test `test_common.c`. The Makefile uses `$(sort)` to dedupe in this case, but splitting tests by functional area produces a cleaner suite. Prefer one test file per group of related helpers (`test_int_bytes`, `test_socket`, `test_rsa_sign_verify`, `test_session_crypto`, `test_x509`, etc.).

## Code style

- C99 or later, GCC dialect
- 4-space indentation, no tabs in `source/`
- snake_case for functions, variables, files
- Each unit test binary has its own `main()` calling `UNITY_BEGIN()`, `RUN_TEST(...)`, `UNITY_END()`
- Unity requires `setUp(void)` and `tearDown(void)` to be defined, even if empty
- When testing functions that take a socket fd, use `socketpair(AF_UNIX, SOCK_STREAM, ...)` and `signal(SIGPIPE, SIG_IGN)` rather than spinning up a TCP listener

## What you MAY modify

You may create or edit files under:

```
tests/unit/
```

That is the only writable area.

## What you MUST NOT modify

Do not modify these unless the student explicitly instructs you to in this conversation:

```
source/
Makefile
tests/integration/
tests/unity/
AGENTS.md
prompts/
scripts/
setup.sh
README.md
```

If a task seems to require modifying any of these, stop and ask the student.

## When asked to generate unit tests

1. Read `source/<name>.h` to understand the public API of the helper.
1. Read `source/<name>.c` to understand intended behaviour.
1. Read `prompts/generate-unit-tests.md` for the project's testing conventions.
1. Write tests to `tests/unit/test_<name>.c`. Overwrite if the file exists.
1. Cover normal cases, edge cases, and invalid input.
1. Do NOT write tests that bind to a TCP port, spawn a server process, call `fork()`, `execv()`, `system()`, or read from stdin. Those are integration tests, covered separately by the student.
1. For socket helpers, use `socketpair()` rather than real TCP.
1. For crypto helpers, generate keys / data in-memory or load fixtures from `auth/`. Do not write fixtures to disk inside `tests/unit/`.
1. Mark each generated test function with a comment indicating it is an AI-generated draft.

## What to avoid

- Do not generate so many tests that the student cannot reasonably review them. Aim for the smallest set that gives meaningful coverage.
- Do not test private static functions. Test only what is declared in the header.
- Do not invent behaviour. If the header does not specify what happens on invalid input, ask the student rather than guessing.
- Do not include any `.c` file that has its own `main()` (`ClientWithoutSecurity.c`, `ServerWithoutSecurity.c`, the AP/CP1/CP2 variants). The unit test has its own `main()` and the link would conflict.
- Do not introduce sleeps to "wait for crypto to settle". Crypto APIs in this project are synchronous; if a test needs a sleep, it is probably not a unit test.

## Student's responsibility

The student is responsible for every test in their submission. They must be able to explain each test during checkoff. Generate tests the student can actually understand and defend.
