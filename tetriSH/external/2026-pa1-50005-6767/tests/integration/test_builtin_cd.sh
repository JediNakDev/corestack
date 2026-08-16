#!/usr/bin/env bash
# tests/integration/test_builtin_cd.sh
#
# Verifies that `cd <dir>` changes the working directory inside the shell,
# such that a subsequent `ld` shows the contents of the new directory.

set -euo pipefail

OUTPUT=$(printf "cd files\nld\nexit\n" | timeout 3s ./cseshell)

if ! echo "$OUTPUT" | grep -F "file1.txt" > /dev/null; then
  echo "FAIL: after 'cd files', expected ld to show file1.txt"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

echo "PASS: cd changes the shell's working directory"
