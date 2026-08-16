# AGENTS.md

This file tells AI coding agents how to work in the PA1 CSEShell project. The student configures an agent to read this file; the agent then knows what it may and may not do.

## What this project is

CSEShell is a simple custom Unix shell, implemented in C, as part of SUTD 50.005 Programming Assignment 1. The student’s task is to expand a one-shot starter shell into a full interactive shell with `fork`/`exec`, seven builtin commands, `.cseshellrc` processing at startup, and four additional system programs.

This is a course assignment. The student must learn the material. You are here only to help draft unit tests, not to write the shell.

## Build

The project builds with `make`. There is no autoconf, cmake, or package manager step. The compiler is GCC. There are no third-party C dependencies beyond libc and a vendored copy of the Unity test framework.

```bash
make            # builds ./cseshell and ./bin/*
make clean      # removes built binaries and unit test binaries
```

## Test

```bash
make unit             # compiles and runs every test under tests/unit/
make integration      # runs every shell script under tests/integration/
make test             # runs both
make ai-unit-tests MODULE=<name>   # invokes scripts/gen_unit_tests.sh
```

Unit tests use the ThrowTheSwitch Unity framework, vendored under `tests/unity/`. Integration tests are bash scripts that run `./cseshell` as a black box.

## Code structure

```
source/shell.c               main(), the shell loop, fork, exec, waitpid
source/shell.h               shared declarations
source/<helper>.c            pure helpers extracted for unit testing
source/<helper>.h            their public APIs
source/system_programs/      one .c file per external program
tests/unit/test_<helper>.c   one test file per helper, paired by name
tests/integration/           bash scripts
tests/unity/                 vendored Unity framework
scripts/                     wrappers (e.g., gen_unit_tests.sh)
prompts/                     prompt templates for AI tools
makefile                     build and test rules
.cseshellrc                  shell startup configuration
```

## Naming convention for unit tests

A pure helper at `source/foo.c` plus `source/foo.h` is tested by `tests/unit/test_foo.c`. The makefile uses this convention to compile each test binary; deviating breaks the build.

## Code style

- C99 or later, GCC dialect
- 4-space indentation, no tabs in `source/`
- snake_case for functions, variables, files
- Each unit test binary has its own `main()` calling `UNITY_BEGIN()`, `RUN_TEST(...)`, `UNITY_END()`
- Unity requires `setUp(void)` and `tearDown(void)` to be defined, even if empty

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
makefile
tests/integration/
tests/unity/
AGENTS.md
prompts/
scripts/
.cseshellrc
README.md
```

If a task seems to require modifying any of these, stop and ask the student.

## When asked to generate unit tests

1. Read `source/<name>.h` to understand the public API of the helper.
1. Read `source/<name>.c` to understand intended behaviour.
1. Read `prompts/generate-unit-tests.md` for the project’s testing conventions.
1. Write tests to `tests/unit/test_<name>.c`. Overwrite if the file exists.
1. Cover normal cases, edge cases, and invalid input.
1. Do NOT write tests that call `fork()`, `execv()`, `execvp()`, `waitpid()`, or anything that reads from stdin. Those are integration tests, covered separately by the student.
1. Mark each generated test function with a comment indicating it is an AI-generated draft.

## What to avoid

- Do not generate so many tests that the student cannot reasonably review them. Aim for the smallest set that gives meaningful coverage.
- Do not test private static functions. Test only what is declared in the header.
- Do not invent behaviour. If the header does not specify what happens on invalid input, ask the student rather than guessing.
- Do not include `main()` from `source/shell.c` in test files. The unit test has its own `main()`.

## Student’s responsibility

The student is responsible for every test in their submission. They must be able to explain each test during checkoff. Generate tests the student can actually understand and defend.
