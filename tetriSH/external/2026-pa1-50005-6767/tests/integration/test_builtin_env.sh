#!/usr/bin/env bash
# tests/integration/test_builtin_env.sh
#
# Verifies the env / setenv / unsetenv builtin trio:
#   - setenv KEY=VALUE makes the variable visible in subsequent `env`.
#   - unsetenv KEY removes it.

set -euo pipefail

# setenv then env should show the new variable.
OUTPUT=$(printf "setenv CSESHELL_TEST=hello\nenv\nexit\n" | timeout 3s ./cseshell)
if ! echo "$OUTPUT" | grep -F "CSESHELL_TEST=hello" > /dev/null; then
  echo "FAIL: setenv did not register CSESHELL_TEST=hello in env output"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

# setenv then unsetenv then env should NOT show the variable.
OUTPUT=$(printf "setenv CSESHELL_TEST=hello\nunsetenv CSESHELL_TEST\nenv\nexit\n" | timeout 3s ./cseshell)
if echo "$OUTPUT" | grep -F "CSESHELL_TEST=hello" > /dev/null; then
  echo "FAIL: unsetenv did not remove CSESHELL_TEST"
  echo "----- shell output -----"
  echo "$OUTPUT"
  echo "------------------------"
  exit 1
fi

echo "PASS: env / setenv / unsetenv work as expected"
