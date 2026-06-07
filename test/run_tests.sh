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
# End-to-end tests for the background spill-to-disk feature (LOP_SPILL_TO_DISK).
#
# The profiler is header-only, so each scenario just compiles test/spill_test.cpp with the relevant
# config overridden via -D (tiny LOP_BUFFER_SIZE to force buffer rotation cheaply, spilling on/off),
# runs it, and validates the merged trace with validate.py.
#
# Scenarios:
#   1. Forced spill, multi-threaded, built with ASan/UBSan  -> many disk segments merged, no UB.
#   2. Equivalence: spilling ON vs OFF produce identical event counts.
#   3. Flag-off build: no spiller, no temp file, unchanged behavior.
#   4. OOM relief valve: under a tight `ulimit -v`, allocation failures are recovered (the spiller
#      is boosted to free RAM) instead of crashing, and no events are lost.
#
# Linux/x64 only. Requires g++ and python3.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O2 -pthread -I${ROOT}/include"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; rm -f /tmp/lop_spill_pid*.tmp' EXIT

fail() { echo "FAILED: $*" >&2; exit 1; }

# build <out> <bufsize> <spill_on> <threshold> <extra_flags>: header-only, config via -D.
build() {
    local out="$1" bufsize="$2" spill="$3" thresh="$4" extra="$5"
    # shellcheck disable=SC2086
    $CXX $CXXFLAGS $extra \
        -DLOP_BUFFER_SIZE=$bufsize -DLOP_SPILL_TO_DISK=$spill -DLOP_SPILL_RAM_THRESHOLD=$thresh \
        "$HERE/spill_test.cpp" -o "$out"
}

echo "== Scenario 1: forced spill, multi-thread, ASan/UBSan =="
build "$WORK/s1bin" 0x100U 1 1 "-g -fsanitize=address,undefined -fno-omit-frame-pointer"
mkdir -p "$WORK/s1"
( cd "$WORK/s1" && TMPDIR="$WORK/s1" ASAN_OPTIONS=halt_on_error=1 "$WORK/s1bin" 20000 > log.txt 2>&1 ) \
    || fail "scenario 1 crashed (see ASan output)"
segs=$(grep -c "spilled events" "$WORK/s1/log.txt" || true)
echo "  merged $segs disk segments"
[ "$segs" -gt 0 ] || fail "scenario 1 spilled nothing - test did not exercise the disk path"
python3 "$HERE/validate.py" "$WORK/s1" 80000 4 || fail "scenario 1 lost events"
[ -z "$(ls /tmp/lop_spill_pid*.tmp 2>/dev/null)" ] || fail "scenario 1 leaked a spill temp file"

echo "== Scenario 2 & 3: spilling ON vs OFF equivalence + flag-off sanity =="
build "$WORK/s2onbin"  0x100U 1 1 ""
build "$WORK/s2offbin" 0x100U 0 1 ""
mkdir -p "$WORK/s2on" "$WORK/s2off"
( cd "$WORK/s2on"  && TMPDIR="$WORK/s2on"  "$WORK/s2onbin"  20000 > log.txt 2>&1 )
( cd "$WORK/s2off" && TMPDIR="$WORK/s2off" "$WORK/s2offbin" 20000 > log.txt 2>&1 )
total_on=$(python3 "$HERE/validate.py"  "$WORK/s2on"  80000 4  | awk '/^TOTAL/{print $2}') \
    || fail "ON build lost events"
total_off=$(python3 "$HERE/validate.py" "$WORK/s2off" 80000 4 | awk '/^TOTAL/{print $2}') \
    || fail "OFF build lost events"
[ "$total_on" = "$total_off" ] || fail "ON/OFF event totals differ: $total_on vs $total_off"
echo "  ON total=$total_on  OFF total=$total_off  (equivalent)"
grep -qi spill "$WORK/s2off/log.txt" && fail "flag-off build mentioned spilling" || true
[ -z "$(ls "$WORK"/s2off/lop_spill_pid*.tmp 2>/dev/null)" ] || fail "flag-off build created a spill file"

if [ "${LOP_SKIP_OOM_TEST:-0}" = "1" ]; then
    echo "== Scenario 4: OOM relief valve -- SKIPPED (LOP_SKIP_OOM_TEST=1) =="
    echo; echo "ALL TESTS PASSED (scenario 4 skipped)"; exit 0
fi

echo "== Scenario 4: OOM relief valve under ulimit -v =="
build "$WORK/s4bin" 0x40000U 1 2 ""   # 8 MB buffers, default threshold
mkdir -p "$WORK/s4"
# Cap virtual memory so the (deliberately faster-than-the-spiller) producers exhaust RAM and the
# relief valve must kick in. The cap leaves enough headroom that the small (throwing) std:: control
# allocations don't hit the ceiling first - only the big 8 MB buffer allocations do, which is what
# the nothrow relief valve handles. The buffer backlog (~46 x 8 MB) still far exceeds the cap.
if ( cd "$WORK/s4" && ulimit -v 393216 && TMPDIR="$WORK/s4" "$WORK/s4bin" 1500000 > log.txt 2>&1 ); then
    activations=$(grep -c "draining buffers to disk to reclaim RAM" "$WORK/s4/log.txt" || true)
    echo "  relief-valve activations: $activations"
    [ "$activations" -gt 0 ] || fail "scenario 4 never hit the relief valve (raise pairs / lower ulimit)"
    python3 "$HERE/validate.py" "$WORK/s4" 6000000 4 || fail "scenario 4 lost events under OOM"
    [ -z "$(ls /tmp/lop_spill_pid*.tmp 2>/dev/null)" ] || fail "scenario 4 leaked a spill temp file"
else
    fail "scenario 4 crashed under memory pressure (relief valve did not save it)"
fi

echo
echo "ALL TESTS PASSED"
