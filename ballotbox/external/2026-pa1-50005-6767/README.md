[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/CBJ_yXMW)
# CSEShell

CSEShell is a simple, custom shell for Unix-based systems, designed to provide an interface for executing system programs. This project includes a basic shell implementation, a set of system programs (`find`, `ld`, `ldr`), and some test files.

## Directory Structure

The project is organized as follows:

- `bin/` - Contains compiled executables for system programs.
  - `find` - Program to find files.
  - `ld` - Program for listing the contents of the current directory.
  - `ldr` - Program for listing the contents of the current directory recursively.
- `cseshell` - The main executable for the CSEShell.
- `files/` - Contains various test files used with the shell and system programs.
  - `combined.txt`, `file1.txt`, `file2.txt`, ... - Test text files.
  - `notes.pdf` - A PDF file for testing.
  - `ss.png` - An image file.
- `makefile` - Makefile for building the CSEShell and system programs.
- `source/` - Source code for the shell and system programs.
  - `libs/` - Contain matching `foo.h` and `foo.c` helper functions, unit-testable.
  - `shell.c` and `shell.h` - Source and header files for the shell.
  - `system_programs/` - Source code and header for the system programs.
- `tests/` - Unit and integration tests.
  - `unit/` - C unit tests using the Unity framework.
  - `integration/` - Bash scripts that run `./cseshell` as a black box.
  - `unity/` - Vendored Unity test framework (`unity.c`, `unity.h`, `unity_internals.h`).
- `scripts/` - Helper scripts, including the AI-assisted test generator.
- `prompts/` - Prompt templates used by the AI test generator.
- `AGENTS.md` - Guide for AI coding agents working in this project.

## Building the Project

To build the CSEShell and system programs, run the following command in the root directory:

```bash
make
```

This will compile the source code and place the executable files in the appropriate directories.

## Running CSEShell

After building, you can start the shell by running:

```bash
./cseshell
```

From there, you can execute built-in commands and any of the included system programs (e.g., `find`, `ld`, `ldr`).

## System Programs

- `find.c` - Searches for files in a directory.
- `ld.c` - List the contents of the curent directory.
- `ldr.c` - List the contents of the current directory recursively.

Each program can be executed from the CSEShell once it is running. This starter code only allows the shell to execute a command once before exiting because `execv` replace the entire process' address space. Students need to fix this and allow the shell to prompt for more commands in Programming Assignment 1.

## Files Directory

The `files/` directory contains various text, PDF, and image files for testing the functionality of the CSEShell and its system programs.

## Makefile

The Makefile contains rules for compiling the shell and system programs. You can clean the build by running:

```bash
make clean
```

## Source Directory

Contains all the necessary source code for the shell and system programs. It is divided into the shell implementation (`shell.c`, `shell.h`) and system programs (`system_programs/`).

## Testing

This project ships with two layers of tests:

- **Unit tests** in `tests/unit/`. Small C programs that exercise pure helper functions directly, using the Unity framework. You can create any matching `test_foo.c` under `tests/unit` to test any libs under `source/libs/foo.c` by including the matching `source/libs/foo.h` header file in the unit test. See `tests/unit/test_perms.c` (or `test_rc_parser.c`) for example.
- **Integration tests** in `tests/integration/`. Bash scripts that run the compiled `./cseshell` as a subprocess, feed it `stdin`, and check `stdout`.
  - You should create your own integration tests. These samples given are just samples, adjust it accordingly.

Run all tests:

```bash
make test
```

Run only unit tests:

```bash
make unit
```

Run only integration tests (requires that `make` has been run first so `cseshell` and the system program binaries exist):

```bash
make integration
```

For an explanation of what to test and how to structure your testable code, see the testing handout.

### AI-Assisted Unit Test Generation

This project includes an optional wrapper for drafting unit tests with an AI agent. After writing a helper, you can run:

```bash
make ai-unit-tests MODULE=<helper_name>
```

This invokes `scripts/gen_unit_tests.sh`, which builds a prompt from `prompts/generate-unit-tests.md` and `AGENTS.md`. The script either pipes the prompt to your configured agent (via the `CSESHELL_AGENT_CMD` environment variable) or prints it to stdout for you to paste into a chat interface. See `AGENTS.md` for setup details and the rules agents follow.

You remain responsible for every test in your submission. You must be able to explain each test during checkoff.
