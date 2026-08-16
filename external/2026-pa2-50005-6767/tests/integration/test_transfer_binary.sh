#!/usr/bin/env bash
# tests/integration/test_transfer_binary.sh
#
# Verifies that a binary file (containing arbitrary byte values including
# NULs and high bytes) is transferred without corruption. This catches
# regressions where someone uses strlen() / null-terminated APIs on the
# file payload.

set -euo pipefail

source ./tests/integration/_lib.sh
trap cleanup EXIT

SRC="files/cbc.bmp"
DST="recv_files/recv_cbc.bmp"

if [ ! -f "$SRC" ]; then
  echo "FAIL: fixture $SRC missing"
  exit 1
fi

reset_recv_files
start_server

set +e
printf '%s\n-1\n' "$SRC" | timeout 20s ./ClientWithoutSecurity \
  "$PORT" localhost > "$LOG_DIR/client.log" 2>&1
CLIENT_RC=$?
set -e

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
  ls -la recv_files/
  dump_logs
  exit 1
fi

# cmp is binary-safe: it compares byte-for-byte and exits non-zero on any
# diff. We DON'T use diff here because diff treats binary files differently
# on some systems.
if ! cmp -s "$SRC" "$DST"; then
  echo "FAIL: $DST differs from $SRC"
  echo "Source size:      $(wc -c < "$SRC")"
  echo "Destination size: $(wc -c < "$DST")"
  # Show the first differing byte for debugging.
  cmp "$SRC" "$DST" || true
  dump_logs
  exit 1
fi

echo "PASS: binary file transferred byte-for-byte ($(wc -c < "$SRC") bytes)"
