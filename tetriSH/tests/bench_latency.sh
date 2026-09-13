#!/bin/sh
#
# bench_latency.sh - p50 / p99 / p99.9 request latency, and the throughput at
# which p99 leaves the SLO.
#
# The front door to tests/bench_latency.c: builds it, turns flags into the env
# vars it reads, and runs it against a throwaway tetrisd on a free port. The
# numbers land in <csv-dir>/latency_steps.csv, one row per offered rate.
#
# Flags exist because the knobs that matter here are the ones you change every
# run (how many clients, what SLO, how long), and remembering six env var names
# at 2am is not a workflow.
#
#   ./tests/bench_latency.sh --clients 400 --slo-ms 20 --duration 15
#
# Every flag has a matching BENCH_* env var; the flag wins.

set -eu

usage()
{
    cat <<'USAGE'
usage: tests/bench_latency.sh [options]

  --clients N        concurrent clients            (default 300, max MAX_SESSIONS)
  --room-size N      players per room              (default 8)
  --workers N        load-generator processes      (default 3)
  --duration N       measured seconds per rate     (default 10)
  --warmup N         unmeasured seconds per rate   (default 2)
  --slo-ms N         SLO: p99 must stay under this (default 25)
  --rate-start N     first offered rate, req/s     (default 800)
  --rate-max N       stop climbing past this       (default 60000)
  --growth PCT       rate increase per step        (default 100, i.e. doubling)
  --steps N          most rates to try             (default 12)
  --refine N         bisections around the knee    (default 4)
  --csv-dir PATH     where the CSV goes            (default var/bench)
  --verbose          let the client library narrate its handshakes
  -h, --help         this

The probe is a JOIN from a client already in a room, which the server answers
409. It is a true request/response round trip through the session process and
the admin thread, so unlike test_saturation's command->STATE timing it is not
quantized by the 20 Hz game tick.

The clients are split across several forked load generators driving one
daemon, because a single process doing TLS for all of them becomes the
bottleneck before tetrisd does. Watch the lag99 column: when it is a large
share of p99, the row is measuring this harness and says so.
USAGE
}

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

while [ $# -gt 0 ]; do
    case "$1" in
        --clients)    BENCH_CLIENTS=$2;    shift 2 ;;
        --room-size)  BENCH_ROOM_SIZE=$2;  shift 2 ;;
        --workers)    BENCH_WORKERS=$2;    shift 2 ;;
        --duration)   BENCH_DURATION_S=$2; shift 2 ;;
        --warmup)     BENCH_WARMUP_S=$2;   shift 2 ;;
        --slo-ms)     BENCH_SLO_MS=$2;     shift 2 ;;
        --rate-start) BENCH_RATE_START=$2; shift 2 ;;
        --rate-max)   BENCH_RATE_MAX=$2;   shift 2 ;;
        --growth)     BENCH_RATE_GROWTH_PCT=$2; shift 2 ;;
        --steps)      BENCH_RATE_STEPS=$2; shift 2 ;;
        --refine)     BENCH_REFINE_STEPS=$2; shift 2 ;;
        --csv-dir)    BENCH_CSV_DIR=$2;    shift 2 ;;
        --verbose)    BENCH_VERBOSE=1;     shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Marking an unset name for export is a no-op the child never sees, so every
# knob the caller did not set falls through to the default in bench_latency.c.
export BENCH_CLIENTS BENCH_ROOM_SIZE BENCH_WORKERS BENCH_DURATION_S BENCH_WARMUP_S \
       BENCH_SLO_MS BENCH_RATE_START BENCH_RATE_MAX BENCH_RATE_GROWTH_PCT \
       BENCH_RATE_STEPS BENCH_REFINE_STEPS BENCH_CSV_DIR BENCH_VERBOSE

# One client per session means one socket per session in THIS process too, and
# the default soft limit on macOS is 256 - under which a 300-client run fails
# in connect(), looking exactly like a server that refused. The daemon raises
# its own limit at startup; this raises the harness's.
want=$(( ${BENCH_CLIENTS:-300} * 2 + 64 ))
hard=$(ulimit -Hn)
if [ "$hard" = "unlimited" ] || [ "$hard" -ge "$want" ]; then
    ulimit -Sn "$want" 2>/dev/null || true
fi

make -s dirs bin/bench_latency

# Falls back to a throwaway CA when the shipped certificate has expired, so a
# measurement run is never blocked on TLS material nobody here can re-sign.
TETRISH_AUTH_DIR=$(./tests/ephemeral_auth.sh)
export TETRISH_AUTH_DIR

exec ./bin/bench_latency
