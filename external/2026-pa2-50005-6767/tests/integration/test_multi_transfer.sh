#!/usr/bin/env bash
# tests/integration/test_multi_transfer.sh
#
# Verifies that the client's while-loop is functional: two files sent in
# the same session both arrive intact. This catches regressions where the
# loop exits after one file, or where state from the first file leaks into
# the second.

set -euo pipefail

source ./tests/integration/_lib.sh
trap cleanup EXIT

SRC1="files/file.txt"
SRC2="files/squeak.wav"
DST1="recv_files/recv_file.txt"
DST2="recv_files/recv_squeak.wav"

for f in "$SRC1" "$SRC2"; do
  if [ ! -f "$f" ]; then
    echo "FAIL: fixture $f missing"
    exit 1
  fi
done

reset_recv_files
start_server

set +e
printf '%s\n%s\n-1\n' "$SRC1" "$SRC2" | timeout 20s \
  ./ClientWithoutSecurity "$PORT" localhost > "$LOG_DIR/client.log" 2>&1
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

for pair in "$SRC1 $DST1" "$SRC2 $DST2"; do
  src=$(echo "$pair" | awk '{print $1}')
  dst=$(echo "$pair" | awk '{print $2}')
  if [ ! -f "$dst" ]; then
    echo "FAIL: expected $dst was not created"
    ls -la recv_files/
    dump_logs
    exit 1
  fi
  if ! cmp -s "$src" "$dst"; then
    echo "FAIL: $dst differs from $src"
    echo "Source size:      $(wc -c < "$src")"
    echo "Destination size: $(wc -c < "$dst")"
    dump_logs
    exit 1
  fi
done

echo "PASS: two files transferred in one session, both byte-for-byte"
