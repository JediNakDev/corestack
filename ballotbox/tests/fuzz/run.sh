#!/bin/sh
#
# run.sh - run every fuzz target for a fixed time each, and report.
#
# Called by `make fuzz-smoke` (60s each, the per-PR gate) and `make fuzz-long`
# (8640s each = 24 hours across ten targets, the campaign). One script for
# both, because a smoke run that differs from the campaign run is a smoke run
# that does not predict it.
#
# What it does that a bare `for f in bin/fuzz_*` does not:
#
#   - Keeps going after a target crashes. A crash is the POINT; stopping at the
#     first one wastes the rest of the machine's night.
#   - Copies each crash into tests/fuzz/regress/<target>/ minimised, so the
#     finding survives as a test case rather than as a file in artifacts/ that
#     the next run overwrites.
#   - Prints one summary at the end, since nobody reads 24 hours of scrollback.
#
# Usage: tests/fuzz/run.sh [seconds-per-target]

set -u

secs="${1:-60}"
here=$(dirname "$0")
root=$(cd "$here/../.." && pwd)
bin="$root/bin"
corpus="$root/tests/fuzz/corpus"
regress="$root/tests/fuzz/regress"
artifacts="$root/tests/fuzz/artifacts"
dicts="$root/tests/fuzz/dict"

mkdir -p "$artifacts"

# The targets are built with -fprofile-instr-generate (for `make fuzz-cov`), so
# every run drops a counter file wherever it was started - default.profraw in
# the repo root, overwritten by the next target and useful to nobody. Coverage
# is measured deliberately by coverage.sh, which sets this itself; here the
# counters just go somewhere harmless.
export LLVM_PROFILE_FILE="$root/tests/fuzz/cov/run-%p.profraw"
mkdir -p "$root/tests/fuzz/cov"

# Target -> dictionary. Kept in step with the Makefile's FUZZ_DICT_* block.
dict_for() {
    case "$1" in
        fuzz_htttp_request|fuzz_htttp_response) echo "$dicts/htttp.dict" ;;
        fuzz_codec_request|fuzz_codec_response) echo "$dicts/codec.dict" ;;
        fuzz_jwt_verify)                        echo "$dicts/jwt.dict" ;;
        fuzz_rows)                              echo "$dicts/rows.dict" ;;
        fuzz_rc_line|fuzz_rc_bind)              echo "$dicts/rc.dict" ;;
        *)                                      echo "" ;;
    esac
}

# -type f -perm -u+x, not a plain glob: a -g build leaves bin/fuzz_x.dSYM
# DIRECTORIES next to the binaries, and `ls bin/fuzz_*` matches those too -
# which then produces a "target" called fuzz_x.dSYM, a corpus directory for it,
# and a regress directory for it, all empty and all committed.
targets=$(find "$bin" -maxdepth 1 -type f -perm -u+x -name 'fuzz_*' 2>/dev/null | sort)
if [ -z "$targets" ]; then
    echo "run.sh: no fuzz targets built - run 'make fuzz-build' first" >&2
    exit 1
fi

failed=""
total=0

for f in $targets; do
    name=$(basename "$f")
    short=${name#fuzz_}
    c="$corpus/$short"
    mkdir -p "$c" "$regress/$short"

    n_before=$(find "$c" -type f 2>/dev/null | wc -l | tr -d ' ')
    d=$(dict_for "$name")

    # One artifact directory PER TARGET. With a shared prefix, the crash-filing
    # loop below cannot tell which target wrote which crash-<sha1>, and files
    # every artifact on disk under whichever target happened to fail - so one
    # bug arrives as a regression case in three unrelated targets, where it
    # fails for reasons that have nothing to do with them.
    a="$artifacts/$short"
    mkdir -p "$a"

    echo "=============================================================="
    echo "== $name  (${secs}s, corpus $n_before)"
    echo "=============================================================="

    # -print_final_stats gives execs/sec and corpus size for the write-up.
    # -timeout is per input: a single input that takes 25s is a hang, and a
    # hang is a denial-of-service finding on a request parser.
    # shellcheck disable=SC2086  # $d is one optional flag or nothing
    "$f" \
        ${d:+-dict="$d"} \
        -max_total_time="$secs" \
        -max_len=65536 \
        -rss_limit_mb=4096 \
        -timeout=25 \
        -print_final_stats=1 \
        -artifact_prefix="$a/" \
        "$c"
    rc=$?

    n_after=$(find "$c" -type f 2>/dev/null | wc -l | tr -d ' ')
    total=$((total + 1))

    if [ $rc -ne 0 ]; then
        failed="$failed $name"
        echo ""
        echo "!! $name exited $rc - preserving the crashing input"

        # libFuzzer names artifacts crash-<sha1>/leak-<sha1>/timeout-<sha1>.
        # Minimise before filing: a 40 KB crasher and its 12-byte minimisation
        # are the same bug, and only one of them is readable in a diff.
        for art in "$a"/crash-* "$a"/leak-* "$a"/timeout-* "$a"/oom-*; do
            [ -f "$art" ] || continue
            base=$(basename "$art")
            [ -f "$regress/$short/$base" ] && continue
            echo "   minimising $base"
            "$f" -minimize_crash=1 -runs=10000 \
                 -exact_artifact_path="$regress/$short/$base" "$art" \
                 >/dev/null 2>&1 || cp "$art" "$regress/$short/$base"
            echo "   filed  tests/fuzz/regress/$short/$base"
        done
    fi

    echo "-- $name: corpus $n_before -> $n_after"
    echo ""
done

echo "=============================================================="
if [ -n "$failed" ]; then
    echo "FUZZ FINDINGS in:$failed"
    echo "Minimised inputs are in tests/fuzz/regress/ and are now part of"
    echo "'make fuzz-regress'. Fix the bug, then that target must pass."
    exit 1
fi
echo "$total targets, ${secs}s each, no findings."
