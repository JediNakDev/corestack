[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/28F8jX7q)
# 50.005 Programming Assignment 2 (C / OpenSSL)

This assignment requires knowledge from Network Security and basic knowledge in C.

## Secure FTP != HTTPS

Note that you will be implementing Secure FTP as your own whole new application layer protocol. In NO WAY we are relying on HTTP/s. Please do not confuse the materials, you don't need to know materials in Week 11 and 12 before getting started.

## Running the code

### Install required libraries

This assignment requires `gcc` (or `clang`) and the OpenSSL development headers.

```bash
# Ubuntu / Debian
sudo apt-get install build-essential libssl-dev

# macOS (Homebrew) — the Makefile auto-detects the path
brew install openssl

# Fedora / RHEL
sudo dnf install gcc openssl-devel
```

### Run `./setup.sh`

Run this in the root project directory:

```
chmod +x ./setup.sh
./setup.sh
```

This will create 3 directories: `/recv_files`, `/recv_files_enc`, and `/send_files_enc` in the project root. They are all empty directories that can't be added in `.git`.

### Build

```
make
```

This compiles all source files. The Makefile automatically detects macOS vs Linux and finds the OpenSSL headers.

### Run server and client files

In two separate shell sessions, run (assuming you're in root project directory):

```
./ServerWithoutSecurity
```

and:

```
./ClientWithoutSecurity
```

### Using different machines

You can also host the Server file in another computer:

```sh
./ServerWithoutSecurity [PORT] 0.0.0.0
```

The client computer can connect to it using the command:

```sh
./ClientWithoutSecurity [PORT] [SERVER-IP-ADDRESS]
```

## Testing

This project ships with two layers of tests:

- **Unit tests** in `tests/unit/`. Small C programs that exercise pure helper functions directly, using the Unity framework. Each test binary is one `.c` file plus `tests/unity/unity.c` plus `source/common.c` (and optionally a paired `source/<name>.c` if you have extracted one).
- **Integration tests** in `tests/integration/`. Bash scripts that build `./ClientWithoutSecurity` and `./ServerWithoutSecurity`, drive a real client / server pair over a loopback socket, and verify that received files match the originals byte-for-byte.

Run all tests:

```bash
make test
```

Run only unit tests:

```bash
make unit
```

Run only integration tests (depends on `NoSec`, which is built automatically):

```bash
make integration
```

The integration server binds to port `14321` by default. Override it if that port is in use:

```bash
PA2_TEST_PORT=15555 make integration
```

### Adding your own unit tests

Two patterns are supported:

1. If you extract a pure helper into `source/<name>.h` and `source/<name>.c`, write its tests in `tests/unit/test_<name>.c`. The Makefile links them together along with `source/common.c` and Unity.
2. To add more tests for the bundled helpers in `common.c`, create `tests/unit/test_<group>.c` with a name that does not match any `source/<group>.c`. The Makefile falls back to linking the test with `source/common.c` and Unity only. Existing examples: `test_int_bytes.c`, `test_socket.c`.

Naming tip: avoid `test_common.c`. The Makefile uses `$(sort)` to dedupe in that case, but splitting tests by functional area (`test_session_crypto.c`, `test_rsa_sign_verify.c`, etc.) produces a cleaner suite.

### Adding your own integration tests

Integration scripts live in `tests/integration/test_*.sh`. Each script:

1. Sources `tests/integration/_lib.sh` (provides `start_server`, `stop_server`, `reset_recv_files`, `dump_logs`, `cleanup`).
2. Sets `trap cleanup EXIT`.
3. Calls `reset_recv_files` and `start_server`.
4. Drives the client, then asserts on the contents of `recv_files/`.

The `_lib.sh` helper waits for the server to bind by attempting to bind the same port itself; when the bind fails with `EADDRINUSE`, the server is up. This avoids the buffering and TCP-probe pitfalls described in the comments inside `_lib.sh`.

### AI-Assisted Unit Test Generation

This project includes an optional wrapper for drafting unit tests with an AI agent. After writing a helper, you can run:

```bash
make ai-unit-tests MODULE=common         # tests for the bundled helpers
make ai-unit-tests MODULE=<your_helper>  # tests for a helper you extracted
```

This invokes `scripts/gen_unit_tests.sh`, which builds a prompt from `prompts/generate-unit-tests.md`, `AGENTS.md`, and the named module's header + source. The script either pipes the prompt to your configured agent (via the `PA2_AGENT_CMD` environment variable) or prints it to stdout for you to paste into a chat interface. See `AGENTS.md` for setup details and the rules agents follow.

You remain responsible for every test in your submission. You must be able to explain each test during checkoff.

## Intellisense Setup (VS)

This is just for quality of life. We use VSCode as example.

If `common.h` shows red squiggly lines on `#include <openssl/rsa.h>` and similar lines in VS Code, the editor's C extension **cannot** find the OpenSSL headers. The compiler can (the Makefile handles that), but the IntelliSense engine reads a separate config.

Create `.vscode/c_cpp_properties.json` in the project root with the include paths for your platform.

**Apple Silicon Macs (M1, M2, M3, M4)**, Homebrew installs to `/opt/homebrew`:

```json
{
  "configurations": [
    {
      "name": "Mac",
      "includePath": [
        "${workspaceFolder}/**",
        "/opt/homebrew/opt/openssl/include"
      ],
      "defines": [],
      "compilerPath": "/usr/bin/clang",
      "cStandard": "c11",
      "intelliSenseMode": "macos-clang-arm64"
    }
  ],
  "version": 4
}
```

**Intel Macs**, Homebrew installs to `/usr/local`:

```json
{
  "configurations": [
    {
      "name": "Mac",
      "includePath": [
        "${workspaceFolder}/**",
        "/usr/local/opt/openssl/include"
      ],
      "defines": [],
      "compilerPath": "/usr/bin/clang",
      "cStandard": "c11",
      "intelliSenseMode": "macos-clang-x64"
    }
  ],
  "version": 4
}
```

**Linux** (Ubuntu / Debian / Fedora), OpenSSL headers ship under `/usr/include/openssl`, which is on the default system include path. Usually no config is needed. If squiggles persist:

```json
{
  "configurations": [
    {
      "name": "Linux",
      "includePath": ["${workspaceFolder}/**", "/usr/include"],
      "defines": [],
      "compilerPath": "/usr/bin/gcc",
      "cStandard": "c11",
      "intelliSenseMode": "linux-gcc-x64"
    }
  ],
  "version": 4
}
```

If you don't know which Mac you have, run `brew --prefix openssl` in the terminal. That prints the actual include parent (append `/include` for the JSON above). On Apple Silicon you will see `/opt/homebrew/opt/openssl`; on Intel you will see `/usr/local/opt/openssl`. Both are symlinks into `Cellar/openssl@3/<version>/`, so pointing at the `opt/openssl` symlink is more stable than hardcoding the versioned Cellar path.

Reload the VS Code window after editing (Cmd/Ctrl+Shift+P, then "Developer: Reload Window") for IntelliSense to pick up the change.

This only affects the editor. The Makefile already detects Homebrew's prefix via `brew --prefix openssl` and passes the include path to the compiler, so `make` works regardless of whether you set this up.
