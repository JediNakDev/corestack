#!/usr/bin/env bash
# tests/integration/_lib.sh
#
# Shared helpers sourced by every integration test. Not invoked directly:
# the makefile globs for test_*.sh, which skips this file.
#
# Tests must be run from the project root so paths like
# `./ServerWithoutSecurity`, `files/`, and `recv_files/` resolve correctly.

# --- Configuration ----------------------------------------------------------

# Test port. The starter binaries default to 4321; we use a higher port so
# tests don't clash with a manually running server. Override with PA2_TEST_PORT.
PORT="${PA2_TEST_PORT:-14321}"

# Per-test temp directory for server / client logs.
LOG_DIR="$(mktemp -d -t pa2_test.XXXXXX)"

# Populated by start_server.
SERVER_PID=""

# --- Functions --------------------------------------------------------------

# reset_recv_files
#   Wipe and recreate recv_files/ so each test starts from a known state.
reset_recv_files() {
  rm -rf recv_files
  mkdir recv_files
}

# start_server
#   Launch ./ServerWithoutSecurity in the background on $PORT and wait
#   until it is actually bound. Returns 1 if the server dies during startup
#   or fails to bind within ~5 seconds.
start_server() {
  if [ ! -x ./ServerWithoutSecurity ]; then
    echo "ERROR: ./ServerWithoutSecurity not found or not executable."
    echo "Run 'make NoSec' first."
    return 1
  fi

  ./ServerWithoutSecurity "$PORT" localhost \
    > "$LOG_DIR/server.log" 2>&1 &
  SERVER_PID=$!

  # Poll up to 50 times at 0.1s = 5s total.
  #
  # Readiness check: try to bind() the same port ourselves. If our bind
  # fails with EADDRINUSE, the server has it bound and is ready to
  # accept(). If our bind succeeds, the server hasn't bound yet, so we
  # release the port and retry.
  #
  # Why not grep the server's stdout for "Server listening"?
  #   When stdout is redirected to a file, glibc switches printf to block
  #   buffering. The "Server listening" line stays in the buffer until the
  #   server prints something else (which only happens after a client
  #   connects). So a log-grep probe deadlocks.
  #
  # Why not /dev/tcp or nc -z?
  #   Those open a real TCP connection. The starter calls accept() exactly
  #   once and then enters the receive loop. A probe connection would be
  #   accepted, immediately closed, and read_bytes would fail, crashing the
  #   server before the real client ever connected.
  local i
  for i in $(seq 1 50); do
    # If the server crashed during startup, abort fast.
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "ERROR: server died before binding to port $PORT. Server log:"
      cat "$LOG_DIR/server.log"
      return 1
    fi

    if python3 -c "
import socket, sys
s = socket.socket()
try:
    s.bind(('localhost', $PORT))
    s.close()
    sys.exit(1)
except OSError:
    sys.exit(0)
" 2>/dev/null; then
      return 0
    fi

    sleep 0.1
  done

  echo "ERROR: server did not bind to port $PORT within 5 seconds."
  echo "Server log:"
  cat "$LOG_DIR/server.log"
  return 1
}

# stop_server
#   Terminate the server if it is still running and reap it.
stop_server() {
  if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}

# dump_logs
#   Print captured server and client logs. Useful in failure paths.
dump_logs() {
  if [ -f "$LOG_DIR/server.log" ]; then
    echo "----- server log -----"
    cat "$LOG_DIR/server.log"
  fi
  if [ -f "$LOG_DIR/client.log" ]; then
    echo "----- client log -----"
    cat "$LOG_DIR/client.log"
  fi
  echo "----------------------"
}

# cleanup
#   Trap target: kill the server and remove the log directory. Safe to call
#   multiple times.
cleanup() {
  stop_server
  if [ -n "${LOG_DIR:-}" ] && [ -d "$LOG_DIR" ]; then
    rm -rf "$LOG_DIR"
  fi
}
