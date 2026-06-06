#!/usr/bin/env bash
#
# Compare per-event tracing overhead of the assembly backend vs the pure-C++ backend.
#
# Builds the SAME test/bench.cpp against each backend, runs both, and prints a comparison of
# per-event overhead (wall-time delta of tracing on vs off, per event) and the minimal observed
# gap between two consecutive events. Gates on correctness (both backends must emit the same,
# balanced event count) but NOT on the perf numbers - those are advisory, especially on a VM.
#
# Linux/x64; needs g++ (override with CXX=) and python3. Scale work with LOP_BENCH_PAIRS / LOP_BENCH_REPS.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O2 -pthread -I${ROOT}/include"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAILED: $*" >&2; exit 1; }

echo "Building bench (asm backend) ..."
$CXX $CXXFLAGS "$HERE/bench.cpp" "$ROOT/src/profiler_asm.cpp" "$ROOT/src/profiler.cpp" -o "$WORK/bench_asm" || fail "asm build"
echo "Building bench (c++ backend) ..."
$CXX $CXXFLAGS "$HERE/bench.cpp" "$ROOT/src/profiler_cpp.cpp" "$ROOT/src/profiler.cpp" -o "$WORK/bench_cpp" || fail "cpp build"

run_backend() { # <name> <binary>
    local name="$1"
    local bin="$2"
    local dir="$WORK/$name"
    mkdir -p "$dir"
    ( cd "$dir" && "$bin" >"$dir/run.txt" 2>/dev/null ) || fail "$name run"
    python3 "$HERE/bench_report.py" "$dir" >>"$dir/run.txt" || fail "$name report"
}

run_backend asm "$WORK/bench_asm"
run_backend cpp "$WORK/bench_cpp"

kv()   { grep -oE "$2=[0-9.-]+" "$1" | head -1 | cut -d= -f2; }  # BENCH line "key=val"
line() { grep -E "^$2 " "$1" | head -1 | awk '{print $2}'; }     # report line "KEY val"

A="$WORK/asm/run.txt"; C="$WORK/cpp/run.txt"
A_PE=$(kv "$A" per_event_ns);  C_PE=$(kv "$C" per_event_ns)
A_MIN=$(line "$A" MIN_CONSECUTIVE_NS); C_MIN=$(line "$C" MIN_CONSECUTIVE_NS)
A_MED=$(line "$A" MEDIAN_CONSECUTIVE_NS); C_MED=$(line "$C" MEDIAN_CONSECUTIVE_NS)
A_EV=$(line "$A" EVENTS); C_EV=$(line "$C" EVENTS)
A_BAL=$(line "$A" BALANCED); C_BAL=$(line "$C" BALANCED)

# Correctness gate (perf numbers are not gated).
[ "$A_BAL" = yes ] && [ "$C_BAL" = yes ] || fail "begin/end not balanced (asm=$A_BAL cpp=$C_BAL)"
[ "$A_EV" = "$C_EV" ] || fail "event counts differ (asm=$A_EV cpp=$C_EV)"

printf '\n%-8s %16s %16s %18s\n' backend per_event_ns min_consec_ns median_consec_ns
printf '%-8s %16s %16s %18s\n' asm "$A_PE" "$A_MIN" "$A_MED"
printf '%-8s %16s %16s %18s\n' cpp "$C_PE" "$C_MIN" "$C_MED"
echo
echo "Correctness: both balanced, $A_EV events each."
echo "(Per-event = wall delta on/off per event; min_consec = closest two timestamps. Noisy on VMs.)"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Tracing overhead: asm vs C++ backend"
        echo ""
        echo "| backend | per-event ns (Δ on/off) | min consecutive ns | median consecutive ns |"
        echo "|---|---|---|---|"
        echo "| asm | $A_PE | $A_MIN | $A_MED |"
        echo "| cpp | $C_PE | $C_MIN | $C_MED |"
        echo ""
        echo "_Events: $A_EV each. Numbers are noisy on shared CI runners; treat as indicative, not a gate._"
    } >> "$GITHUB_STEP_SUMMARY"
fi

echo "BENCH OK"
