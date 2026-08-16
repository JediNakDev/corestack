#!/usr/bin/env bash
# tests/integration/test_exit.sh
#
# Verifies that typing "exit" terminates the shell within a reasonable time.
# If exit is broken, timeout will kill the shell and return 124.

set -euo pipefail

if ! timeout 3s bash -c 'printf "exit\n" | ./cseshell' > /dev/null; then
  echo "FAIL: shell did not exit cleanly within 3 seconds"
  exit 1
fi

echo "PASS: exit terminates the shell"
