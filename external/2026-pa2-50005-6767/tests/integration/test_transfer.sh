#!/usr/bin/env bash
# tests/integration/test_transfer.sh
#
# Verifies that a small text file sent through the client arrives at the
# server byte-for-byte under recv_files/recv_<basename>.
#
# Wire protocol exercised:
#   [MSG_FILENAME=0][len][bytes]
#   [MSG_FILE_DATA=1][len][bytes]
#   [MSG_CLOSE=2]

set -euo pipefail

source ./tests/integration/_lib.sh
trap cleanup EXIT

SRC="files/file.txt"
DST="recv_files/recv_file.txt"

if [ ! -f "$SRC" ]; then
  echo "FAIL: fixture $SRC missing. Did you delete files/?"
  exit 1
fi

reset_recv_files
start_server

# Send one file then exit.
set +e
printf '%s\n-1\n' "$SRC" | timeout 10s ./ClientWithoutSecurity \
  "$PORT" localhost > "$LOG_DIR/client.log" 2>&1
CLIENT_RC=$?
set -e

# Wait for server to terminate.
for i in $(seq 1 20); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
  sleep 0.1
done

if [ "$CLIENT_RC" -ne 0 ]; then
  echo "FAIL: client exited with status $CLIENT_RC"
  dump_logs
  exit 1
fi

if [ ! -f "$DST" ]; then
  echo "FAIL: expected destination $DST was not created"
  echo "Contents of recv_files/:"
  ls -la recv_files/
  dump_logs
  exit 1
fi

if ! cmp -s "$SRC" "$DST"; then
  echo "FAIL: $DST differs from $SRC"
  echo "Source size:      $(wc -c < "$SRC")"
  echo "Destination size: $(wc -c < "$DST")"
  dump_logs
  exit 1
fi

echo "PASS: text file transferred byte-for-byte"
