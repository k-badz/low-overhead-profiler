#!/usr/bin/env bash
# Copyright (c) 2025-2026 Krzysztof Badziak
#
# Licensed under the MIT License:
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# Measure per-event tracing overhead of the (header-only) profiler.
#
# Builds test/bench.cpp, runs it, and reports per-event overhead (wall-time delta of tracing on vs
# off, per event) and the smallest observed gap between two consecutive events. Gates on correctness
# (balanced begin/end counts) but NOT on the perf numbers - those are advisory, especially on a VM.
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

echo "Building bench ..."
$CXX $CXXFLAGS "$HERE/bench.cpp" -o "$WORK/bench" || fail "build"

( cd "$WORK" && "$WORK/bench" >"$WORK/run.txt" 2>/dev/null ) || fail "run"
python3 "$HERE/bench_report.py" "$WORK" >>"$WORK/run.txt" || fail "report"

kv()   { grep -oE "$1=[0-9.-]+" "$WORK/run.txt" | head -1 | cut -d= -f2; }  # BENCH line "key=val"
line() { grep -E "^$1 " "$WORK/run.txt" | head -1 | awk '{print $2}'; }      # report line "KEY val"

PE=$(kv per_event_ns)
MIN=$(line MIN_CONSECUTIVE_NS)
MED=$(line MEDIAN_CONSECUTIVE_NS)
EV=$(line EVENTS)
BAL=$(line BALANCED)

[ "$BAL" = yes ] || fail "begin/end not balanced"

printf '\n%18s %16s %18s %8s\n' per_event_ns min_consec_ns median_consec_ns events
printf '%18s %16s %18s %8s\n' "$PE" "$MIN" "$MED" "$EV"
echo
echo "(Per-event = wall delta on/off per event; min_consec = closest two timestamps. Noisy on VMs.)"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Tracing overhead (header-only C++)"
        echo ""
        echo "| per-event ns (Δ on/off) | min consecutive ns | median consecutive ns | events |"
        echo "|---|---|---|---|"
        echo "| $PE | $MIN | $MED | $EV |"
        echo ""
        echo "_Numbers are noisy on shared CI runners; treat as indicative, not a gate._"
    } >> "$GITHUB_STEP_SUMMARY"
fi

echo "BENCH OK"
