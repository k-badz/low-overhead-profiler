#!/usr/bin/env bash
#
# End-to-end tests for the background spill-to-disk feature (LOP_SPILL_TO_DISK).
#
# Because the buffer size and feature flags are compile-time constants, each scenario compiles the
# profiler from a patched copy of the sources (tiny LOP_BUFFER_SIZE to force rotation cheaply, or
# spilling toggled on/off), runs spill_test.cpp, and validates the merged trace with validate.py.
#
# Scenarios:
#   1. Forced spill, multi-threaded, built with ASan/UBSan  -> many disk segments merged, no UB.
#   2. Equivalence: spilling ON vs OFF produce identical event counts.
#   3. Flag-off build: no spiller, no temp file, unchanged behavior.
#   4. OOM relief valve: under a tight `ulimit -v`, allocation failures are recovered (the spiller
#      is boosted to free RAM) instead of crashing, and no events are lost.
#
# Linux/x64 only (uses the GCC inline-asm backend src/profiler_asm.cpp). Requires g++ and python3.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O2 -pthread"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; rm -f /tmp/lop_spill_pid*.tmp' EXIT

fail() { echo "FAILED: $*" >&2; exit 1; }

# prepare <dir> <bufsize> <spill_on> <threshold>: patched copy of the profiler sources.
prepare() {
    local dir="$1" bufsize="$2" spill="$3" thresh="$4"
    mkdir -p "$dir"
    sed "s/#define LOP_BUFFER_SIZE 0x400000U/#define LOP_BUFFER_SIZE ${bufsize}/" \
        "$ROOT/src/profiler.cpp" > "$dir/profiler.cpp"
    sed "s/#define LOP_BUFFER_SIZE 0x400000U/#define LOP_BUFFER_SIZE ${bufsize}/" \
        "$ROOT/src/profiler_asm.cpp" > "$dir/profiler_asm.cpp"
    sed -e "s/#define LOP_SPILL_TO_DISK 1/#define LOP_SPILL_TO_DISK ${spill}/" \
        -e "s/#define LOP_SPILL_RAM_THRESHOLD 2/#define LOP_SPILL_RAM_THRESHOLD ${thresh}/" \
        "$ROOT/include/profiler.h" > "$dir/profiler.h"
}

# build <dir> <extra_flags> <out>
build() {
    local dir="$1" extra="$2" out="$3"
    # shellcheck disable=SC2086
    $CXX $CXXFLAGS $extra "$HERE/spill_test.cpp" "$dir/profiler_asm.cpp" "$dir/profiler.cpp" \
        -I"$dir" -o "$out"
}

echo "== Scenario 1: forced spill, multi-thread, ASan/UBSan =="
prepare "$WORK/s1" 0x100U 1 1
build "$WORK/s1" "-g -fsanitize=address,undefined -fno-omit-frame-pointer" "$WORK/s1/test"
mkdir -p "$WORK/s1/run"
( cd "$WORK/s1/run" && TMPDIR="$WORK/s1/run" ASAN_OPTIONS=halt_on_error=1 "$WORK/s1/test" 20000 > log.txt 2>&1 ) \
    || fail "scenario 1 crashed (see ASan output)"
segs=$(grep -c "spilled events" "$WORK/s1/run/log.txt" || true)
echo "  merged $segs disk segments"
[ "$segs" -gt 0 ] || fail "scenario 1 spilled nothing - test did not exercise the disk path"
python3 "$HERE/validate.py" "$WORK/s1/run" 80000 4 || fail "scenario 1 lost events"
[ -z "$(ls /tmp/lop_spill_pid*.tmp 2>/dev/null)" ] || fail "scenario 1 leaked a spill temp file"

echo "== Scenario 2 & 3: spilling ON vs OFF equivalence + flag-off sanity =="
prepare "$WORK/s2on"  0x100U 1 1
prepare "$WORK/s2off" 0x100U 0 1
build "$WORK/s2on"  "" "$WORK/s2on/test"
build "$WORK/s2off" "" "$WORK/s2off/test"
mkdir -p "$WORK/s2on/run" "$WORK/s2off/run"
( cd "$WORK/s2on/run"  && TMPDIR="$WORK/s2on/run"  "$WORK/s2on/test"  20000 > log.txt 2>&1 )
( cd "$WORK/s2off/run" && TMPDIR="$WORK/s2off/run" "$WORK/s2off/test" 20000 > log.txt 2>&1 )
total_on=$(python3 "$HERE/validate.py"  "$WORK/s2on/run"  80000 4  | awk '/^TOTAL/{print $2}') \
    || fail "ON build lost events"
total_off=$(python3 "$HERE/validate.py" "$WORK/s2off/run" 80000 4 | awk '/^TOTAL/{print $2}') \
    || fail "OFF build lost events"
[ "$total_on" = "$total_off" ] || fail "ON/OFF event totals differ: $total_on vs $total_off"
echo "  ON total=$total_on  OFF total=$total_off  (equivalent)"
grep -qi spill "$WORK/s2off/run/log.txt" && fail "flag-off build mentioned spilling" || true
[ -z "$(ls "$WORK"/s2off/run/lop_spill_pid*.tmp 2>/dev/null)" ] || fail "flag-off build created a spill file"

if [ "${LOP_SKIP_OOM_TEST:-0}" = "1" ]; then
    echo "== Scenario 4: OOM relief valve -- SKIPPED (LOP_SKIP_OOM_TEST=1) =="
    echo; echo "ALL TESTS PASSED (scenario 4 skipped)"; exit 0
fi

echo "== Scenario 4: OOM relief valve under ulimit -v =="
prepare "$WORK/s4" 0x40000U 1 2   # 8 MB buffers, default threshold
build "$WORK/s4" "" "$WORK/s4/test"
mkdir -p "$WORK/s4/run"
# Cap virtual memory so the (deliberately faster-than-the-spiller) producers exhaust RAM and the
# relief valve must kick in. 350 MB comfortably allows startup but not an unbounded backlog.
if ( cd "$WORK/s4/run" && ulimit -v 358400 && TMPDIR="$WORK/s4/run" "$WORK/s4/test" 1500000 > log.txt 2>&1 ); then
    activations=$(grep -c "draining buffers to disk to reclaim RAM" "$WORK/s4/run/log.txt" || true)
    echo "  relief-valve activations: $activations"
    [ "$activations" -gt 0 ] || fail "scenario 4 never hit the relief valve (raise pairs / lower ulimit)"
    python3 "$HERE/validate.py" "$WORK/s4/run" 6000000 4 || fail "scenario 4 lost events under OOM"
    [ -z "$(ls /tmp/lop_spill_pid*.tmp 2>/dev/null)" ] || fail "scenario 4 leaked a spill temp file"
else
    fail "scenario 4 crashed under memory pressure (relief valve did not save it)"
fi

echo
echo "ALL TESTS PASSED"
