#!/bin/sh
#
# bench_report.sh - the whole measurement set, in one run.
#
# bench_latency.sh measures ONE configuration. This drives it through every
# configuration the numbers came from and prints the tables at the end, so a
# rerun on a quiet machine reproduces the whole set instead of five commands
# typed from memory in the wrong order.
#
# Two things it does that matter:
#
#   The knee is repeated (--reps, default 3). Measured spread across runs was
#   19.4k to 25.3k req/s on the same build, because 300 session processes plus
#   the load generators do not fit on 8 cores without the scheduler getting a
#   vote. One sweep gives you a number; three give you the number AND its error
#   bar, and the error bar is the honest part.
#
#   The tail study holds the offered rate fixed and varies ONLY the client
#   count, which separates "the server is slow at the tail" from "the machine
#   was busy".
#
#   Worth knowing what it caught. On a loaded machine it read 0.4 / 12.9 / 18.1
#   ms at 32 / 128 / 254 clients, which looks exactly like process scheduling
#   and was written up as such. On an idle machine the same experiment reads
#   0.56 / 0.55 / 0.53 ms - flat. The 40x tail was contention from benchmarks
#   run back to back, not the client count. Run this on a quiet machine or the
#   table will invent a story for you.
#
# Run it on an idle machine. It says so if it thinks you have not.
#
#   ./tests/bench_report.sh                 # full set, ~15 min
#   ./tests/bench_report.sh --reps 5        # tighter error bar, ~25 min
#   ./tests/bench_report.sh --quick         # shape check, ~4 min

set -eu

REPS=3
QUICK=0
OUT=""

usage()
{
    cat <<'USAGE'
usage: tests/bench_report.sh [options]

  --reps N      how many times to repeat the knee sweep (default 3)
  --quick       short windows and one rep, for checking the rig not the server
  --out DIR     where CSVs and the log go (default var/bench/report-<date>)
  -h, --help    this

Produces three tables:
  1. steady state at 254 clients x 10 req/s, the number to quote
  2. tail study, p99.9 against client count at a fixed offered rate
  3. knee sweep, repeated, with the spread across repeats
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --reps)    REPS=$2; shift 2 ;;
        --quick)   QUICK=1; shift ;;
        --out)     OUT=$2;  shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

cd "$(cd "$(dirname "$0")/.." && pwd)"

[ -n "$OUT" ] || OUT="var/bench/report-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
LOG="$OUT/run.log"

# Window lengths. The long ones are not arbitrary: p99.9 off a 10 s window at
# 2540 req/s is the 25 worst samples, which is a coin toss. 30 s makes it 76.
if [ "$QUICK" = "1" ]; then
    STEADY_S=5; SWEEP_S=4; REPS=1
else
    STEADY_S=30; SWEEP_S=10
fi

# A busy machine does not just add noise, it moves the knee - which is the
# whole reason this file exists. Say so rather than silently producing a low
# number the reader will take at face value.
#
# Sampled idle CPU, NOT load average. Load average is a decaying window, so
# after a run like this one it still reads 4+ for several minutes and blames
# the user for CPU the previous benchmark spent. This measures the second of
# wall clock about to be used, which is the only second that matters.
#
# 70% is deliberately generous. The editor you launched this from, WindowServer
# and a chat app idling are worth a few percent between them and cannot be
# closed anyway - you are typing into them.
idle=$(top -l 2 -n 0 -s 1 2>/dev/null | awk '/CPU usage/ { i = $7 } END {
    gsub(/%/, "", i); print (i == "") ? 100 : i }')
cores=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)
quiet=$(awk -v i="$idle" 'BEGIN { print (i + 0 >= 70) ? 1 : 0 }')
if [ "$quiet" = "1" ]; then
    echo "machine looks quiet: ${idle}% idle across $cores cores"
else
    echo "WARNING: only ${idle}% CPU idle. The knee will read low and the tail"
    echo "         high. Check what is running, or wait a minute if you just"
    echo "         finished another run."
fi
echo ""

echo "tetriSH benchmark report"
echo "  output : $OUT"
echo "  reps   : $REPS   windows: steady ${STEADY_S}s, sweep ${SWEEP_S}s"
echo ""

run()
{
    dir=$1; shift
    echo "--- $dir" | tee -a "$LOG"
    ./tests/bench_latency.sh --csv-dir "$OUT/$dir" "$@" 2>&1 | tee -a "$LOG" \
        | grep -E "sweep |refine |max throughput|p99 breaks|sessions dropped" \
        || true
    echo "" | tee -a "$LOG"
}

# 1. Steady state. 254 clients at 10 req/s each, which is the load the brief
#    describes, at about a twelfth of the knee. This is the quotable row.
run steady --clients 254 --workers 3 --duration "$STEADY_S" --warmup 3 \
    --rate-start 2540 --steps 1 --refine 0

# 2. Tail study. Same offered rate every time, only the process count changes.
for n in 32 128 254; do
    run "tail$n" --clients "$n" --workers 3 --duration "$STEADY_S" --warmup 3 \
        --rate-start 2540 --steps 1 --refine 0
done

# 3. Knee, repeated. 20% steps rather than doubling: the interesting region is
#    narrow enough that a doubling jumps clean over it.
rep=1
while [ "$rep" -le "$REPS" ]; do
    run "knee$rep" --clients 300 --workers 3 --slo-ms 25 \
        --duration "$SWEEP_S" --warmup 2 \
        --rate-start 16000 --growth 20 --steps 6 --refine 4
    rep=$((rep + 1))
done

# ---- tables -------------------------------------------------------------- #
# Read back from the CSVs rather than scraping the log: the CSV is the record,
# and a table built from stdout would break the first time a column moves.

# Sample count, not max.
#
# max is one observation out of tens of thousands, so it moves by 10x between
# runs of the identical configuration and cannot be defended when someone asks
# what caused it. n is what makes the percentile beside it mean something: at
# 76,000 samples p99.9 is the worst 76, which is a number. At 12,000 it is the
# worst 12, which is a rumour. max stays in the CSV for anyone who wants it.
row()
{
    awk -F, -v label="$2" 'NR == 2 {
        printf "  %-14s %8.2f %8.2f %8.2f %8.2f %10d\n",
               label, $11/1000, $12/1000, $13/1000, $14/1000, $8
    }' "$1"
}

echo ""
echo "=========================================================="
if [ "$QUICK" = "1" ]; then
    # A 5 s window at 2540 req/s is 12,700 samples, so p99.9 is the 13 worst
    # of them. Quick mode proves the rig runs. It does not measure a tail, and
    # saying so here is cheaper than someone quoting one of these numbers.
    echo "QUICK MODE - windows are too short for p99/p99.9 to mean anything."
    echo "Use it to check the rig works, then rerun without --quick."
    echo ""
fi
echo "1. steady state - 254 clients x 10 req/s = 2540 req/s"
echo "                      p50      p90      p99    p99.9    samples"
echo "                      ------------ ms -----------"
[ -f "$OUT/steady/latency_steps.csv" ] && row "$OUT/steady/latency_steps.csv" "254 clients"

echo ""
echo "2. tail study - same 2540 req/s, only the client count changes"
echo "                      p50      p90      p99    p99.9    samples"
echo "                      ------------ ms -----------"
for n in 32 128 254; do
    f="$OUT/tail$n/latency_steps.csv"
    [ -f "$f" ] && row "$f" "$n clients"
done
echo ""
echo "  If p99.9 is flat across the three rows, the tail does NOT come from the"
echo "  process count and the server is behaving. If it climbs with clients,"
echo "  suspect the machine before the code - that is what this table is for."

echo ""
echo "3. knee - 300 clients, p99 <= 25 ms"
echo "  rep   highest passing   first failing"
rep=1
while [ "$rep" -le "$REPS" ]; do
    f="$OUT/knee$rep/latency_steps.csv"
    if [ -f "$f" ]; then
        awk -F, -v r="$rep" '
            NR > 1 && $18 == 1 && $5 + 0 > ok { ok = $5 + 0 }
            NR > 1 && $18 == 0 && (bad == 0 || $5 + 0 < bad) { bad = $5 + 0 }
            END { printf "  %-5s %15.0f %15.0f\n", r, ok, bad }' "$f"
    fi
    rep=$((rep + 1))
done

# The spread across repeats IS the result. A single knee reported to four
# significant figures would be a lie about precision this rig cannot deliver.
awk -F, 'FNR > 1 && $18 == 1 && $5 + 0 > ok { ok = $5 + 0 } END {
    if (ok > 0) printf "\n  best single pass across all reps: %.0f req/s\n", ok
}' "$OUT"/knee*/latency_steps.csv 2>/dev/null || true

echo ""
echo "  full rows: $OUT/*/latency_steps.csv"
echo "  log      : $LOG"
echo "=========================================================="
