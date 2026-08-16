#!/usr/bin/env bash
# tests/integration/test_system_programs_bundled.sh
#
# Verifies that the three bundled system programs (ld, ldr, find) run
# correctly when invoked from inside the shell. These programs are part of
# the starter and should work as soon as the shell loop and fork are in
# place.

set -euo pipefail

OUTPUT=$(printf "ld\nfind . file1\nldr\nexit\n" | timeout 5s ./cseshell)

# ld should list the 'files' directory.
if ! echo "$OUTPUT" | grep -F "files" > /dev/null; then
  echo "FAIL: ld did not show the 'files' directory"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

# find should find file1.txt under the current directory.
if ! echo "$OUTPUT" | grep -F "file1.txt" > /dev/null; then
  echo "FAIL: find did not match file1.txt"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

# ldr should recursively list contents including files under files/.
# We check for a known fixture file appearing with a path component.
if ! echo "$OUTPUT" | grep -F "files/file1.txt" > /dev/null; then
  echo "FAIL: ldr did not recursively show files/file1.txt"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

echo "PASS: ld, find, and ldr work through the shell"
