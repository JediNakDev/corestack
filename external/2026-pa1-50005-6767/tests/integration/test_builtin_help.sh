#!/usr/bin/env bash
# tests/integration/test_builtin_help.sh
#
# Verifies that `help` prints the names of all seven required builtins.
# Does not enforce the exact format; just checks each name appears.

set -euo pipefail

OUTPUT=$(printf "help\nexit\n" | timeout 3s ./cseshell)

MISSING=""
for cmd in cd help exit usage env setenv unsetenv; do
  if ! echo "$OUTPUT" | grep -F "$cmd" > /dev/null; then
    MISSING="$MISSING $cmd"
  fi
done

if [[ -n "$MISSING" ]]; then
  echo "FAIL: help did not mention these builtins:$MISSING"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

echo "PASS: help lists all seven builtins"
