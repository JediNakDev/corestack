#!/usr/bin/env bash
# tests/integration/test_exit.sh
#
# Verifies the clean-exit handshake:
#   1. Client typing "-1" sends the MSG_CLOSE opcode.
#   2. Server's main loop hits case MSG_CLOSE, prints "Closing connection...",
#      breaks out, and exits.
#   3. Client exits with status 0.

set -euo pipefail

source ./tests/integration/_lib.sh
trap cleanup EXIT

reset_recv_files
start_server

# Send "-1" to exit on first prompt.
set +e
printf -- '-1\n' | timeout 10s ./ClientWithoutSecurity "$PORT" localhost \
  > "$LOG_DIR/client.log" 2>&1
CLIENT_RC=$?
set -e

# Give the server up to 2s to terminate after it receives the exit opcode.
for i in $(seq 1 20); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "FAIL: server still running 2s after client sent the exit opcode"
  dump_logs
  exit 1
fi

if [ "$CLIENT_RC" -ne 0 ]; then
  echo "FAIL: client exited with status $CLIENT_RC (expected 0)"
  dump_logs
  exit 1
fi

# The server should have logged "Closing connection..." before exiting.
if ! grep -F "Closing connection" "$LOG_DIR/server.log" > /dev/null; then
  echo "FAIL: server did not log 'Closing connection...'"
  dump_logs
  exit 1
fi

echo "PASS: client -1 cleanly exits both client and server"
