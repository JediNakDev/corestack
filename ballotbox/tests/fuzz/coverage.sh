#!/bin/sh
#
# coverage.sh - what the corpus actually reaches.
#
# "We fuzzed the parser for 24 hours" is not a claim about the parser; it is a
# claim about the clock. Line coverage of the file under test is the claim
# worth making, and the one that says where to look next: an uncovered branch
# after a long campaign is either dead code or a shape the mutator cannot
# reach from the corpus (usually a missing dictionary token).
#
# Replays the corpus with -runs=0 - no mutation, no new inputs - so the number
# describes the corpus you have committed, not a run nobody can repeat.
#
# Usage: tests/fuzz/coverage.sh [target ...]     (default: all built targets)

set -u

here=$(dirname "$0")
root=$(cd "$here/../.." && pwd)
bin="$root/bin"
corpus="$root/tests/fuzz/corpus"
covdir="$root/tests/fuzz/cov"

llvm_prefix=$(brew --prefix llvm 2>/dev/null)
profdata="$llvm_prefix/bin/llvm-profdata"
cov="$llvm_prefix/bin/llvm-cov"

if [ ! -x "$profdata" ] || [ ! -x "$cov" ]; then
    echo "coverage.sh: needs Homebrew LLVM's llvm-profdata/llvm-cov (brew install llvm)" >&2
    exit 1
fi

mkdir -p "$covdir"

if [ $# -gt 0 ]; then
    targets=""
    for t in "$@"; do targets="$targets $bin/$t"; done
else
    targets=$(find "$bin" -maxdepth 1 -type f -perm -u+x -name 'fuzz_*' 2>/dev/null | sort)
fi

if [ -z "$targets" ]; then
    echo "coverage.sh: no fuzz targets built - run 'make fuzz-build' first" >&2
    exit 1
fi

for f in $targets; do
    [ -x "$f" ] || continue
    name=$(basename "$f")
    short=${name#fuzz_}
    c="$corpus/$short"

    if [ ! -d "$c" ]; then
        echo "== $name: no corpus at $c, skipped"
        continue
    fi

    raw="$covdir/$name.profraw"
    merged="$covdir/$name.profdata"

    # LLVM_PROFILE_FILE is where the instrumented binary writes counters.
    LLVM_PROFILE_FILE="$raw" "$f" -runs=0 "$c" >/dev/null 2>&1

    if [ ! -f "$raw" ]; then
        echo "== $name: no profile written, skipped"
        continue
    fi

    "$profdata" merge -sparse "$raw" -o "$merged" || continue

    echo "=============================================================="
    echo "== $name"
    echo "=============================================================="
    # Only the sources under test - the harness's own coverage is noise, and
    # so is anything the target merely links.
    "$cov" report "$f" -instr-profile="$merged" \
        -ignore-filename-regex='tests/fuzz|external/' 2>/dev/null

    # An annotated listing of the worst-covered file, for the actual work of
    # "why is this branch cold".
    "$cov" show "$f" -instr-profile="$merged" \
        -ignore-filename-regex='tests/fuzz|external/' \
        -format=html -output-dir="$covdir/$name" >/dev/null 2>&1 && \
        echo "  annotated listing: tests/fuzz/cov/$name/index.html"
    echo ""
done
