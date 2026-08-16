#!/usr/bin/env bash
# Wrapper for AI-assisted unit test generation.
#
# Usage:
#   bash scripts/gen_unit_tests.sh <module>
#
# Looks for source/<module>.h and source/<module>.c, builds a prompt that
# includes:
#   - The prompt template in prompts/generate-unit-tests.md
#   - The contents of AGENTS.md
#   - The header and source for the named module
#
# Then either:
#   - Pipes the prompt to your configured AI agent via $CSESHELL_AGENT_CMD, or
#   - Prints the prompt to stdout for you to paste into a chat UI manually.
#
# To configure an agent, set CSESHELL_AGENT_CMD before running make:
#
#   export CSESHELL_AGENT_CMD="claude"
#   export CSESHELL_AGENT_CMD="aider --message-file -"
#   export CSESHELL_AGENT_CMD="tee /tmp/prompt.txt"   # save to disk
#
# Without CSESHELL_AGENT_CMD set, the prompt is printed and you paste it
# into your AI tool yourself, then save the response to
# tests/unit/test_<module>.c.

set -euo pipefail

MODULE="${1:-}"
if [[ -z "$MODULE" ]]; then
  echo "Usage: $0 <module>" >&2
  exit 1
fi

HEADER="./source/${MODULE}.h"
SOURCE="./source/${MODULE}.c"
PROMPT_TEMPLATE="./prompts/generate-unit-tests.md"
AGENTS_DOC="./AGENTS.md"

for f in "$HEADER" "$SOURCE" "$PROMPT_TEMPLATE" "$AGENTS_DOC"; do
  if [[ ! -f "$f" ]]; then
    echo "Error: $f not found." >&2
    if [[ "$f" == "$HEADER" || "$f" == "$SOURCE" ]]; then
      echo "Create the header and implementation for your helper first." >&2
    fi
    exit 1
  fi
done

build_prompt() {
  cat "$PROMPT_TEMPLATE"
  echo ""
  echo "## Project rules (from AGENTS.md)"
  echo ""
  cat "$AGENTS_DOC"
  echo ""
  echo "## Target module: ${MODULE}"
  echo ""
  echo "### Header: source/${MODULE}.h"
  echo '```c'
  cat "$HEADER"
  echo '```'
  echo ""
  echo "### Implementation: source/${MODULE}.c"
  echo '```c'
  cat "$SOURCE"
  echo '```'
  echo ""
  echo "## Output file"
  echo ""
  echo "tests/unit/test_${MODULE}.c"
}

AGENT_CMD="${CSESHELL_AGENT_CMD:-}"

if [[ -z "$AGENT_CMD" ]]; then
  echo "================================================================" >&2
  echo "CSESHELL_AGENT_CMD not set. Printing prompt to stdout." >&2
  echo "Copy everything below and paste it into your AI tool." >&2
  echo "Then save the response to tests/unit/test_${MODULE}.c" >&2
  echo "================================================================" >&2
  echo "" >&2
  build_prompt
  exit 0
fi

build_prompt | eval "$AGENT_CMD"
