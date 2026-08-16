#!/usr/bin/env bash
# tests/integration/test_loop.sh
#
# Verifies that the shell:
#   - Loops over multiple commands in one session.
#   - Tolerates empty input (just pressing enter).
#   - Prints an error for an unknown command but does not die.
#
# Fails against the unmodified starter because the starter exits after
# one command (no fork + no loop).

set -euo pipefail

INPUT=$(printf "ld\n\n\nnotacommand_xyz\nld\nexit\n")
OUTPUT=$(echo "$INPUT" | timeout 3s ./cseshell)

# ld should have run at least twice (once before and once after the bad command).
COUNT=$(echo "$OUTPUT" | grep -c "files" || true)
if [[ "$COUNT" -lt 2 ]]; then
  echo "FAIL: expected ld to run at least twice, got $COUNT occurrences of 'files'"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

# Unknown command should have produced an error message.
if ! echo "$OUTPUT" | grep -F "not found" > /dev/null; then
  echo "FAIL: expected 'not found' error for unknown command"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

echo "PASS: shell loops correctly and survives unknown commands"
