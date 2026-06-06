/**
 * Per-event tracing-overhead benchmark, backend-agnostic.
 *
 * Link it against either backend (the difference is the only thing that changes):
 *   g++ -O2 -std=c++17 -pthread -Iinclude test/bench.cpp src/profiler_asm.cpp src/profiler.cpp
 *   g++ -O2 -std=c++17 -pthread -Iinclude test/bench.cpp src/profiler_cpp.cpp src/profiler.cpp
 *
 * Method (matches the owner's manual approach): run the SAME loop with tracing off then on; the
 * wall-time delta divided by events emitted is the per-event overhead. Because the public emit
 * wrappers are just `if (enabled) backend(...)`, the off run pays only a predicted-not-taken
 * branch, so the delta isolates the backend body -> a clean asm-vs-C++ comparison. After the runs
 * it flushes a trace so test/bench_report.py can also report the smallest gap between two
 * consecutive events (the optimistic minimal overhead).
 *
 * Defaults keep the total enabled events below LOP_BUFFER_SIZE so no buffer rotation pollutes the
 * hot-path number. Override with argv or LOP_BENCH_PAIRS / LOP_BENCH_REPS.
 */
#include "profiler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

using namespace LOP;
using clk = std::chrono::steady_clock;

static volatile uint64_t g_acc = 0; // volatile "work" so the loop is never optimized away

static double run_loop(long pairs) {
    auto t0 = clk::now();
    for (long i = 0; i < pairs; ++i) {
        emit_begin_event("bench");
        g_acc++;
        emit_end_event("bench");
    }
    auto t1 = clk::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

static long env_long(const char* name, long fallback) {
    const char* v = getenv(name);
    return v ? atol(v) : fallback;
}

int main(int argc, char** argv) {
    long pairs = (argc > 1) ? atol(argv[1]) : env_long("LOP_BENCH_PAIRS", 200000);
    int  reps  = (argc > 2) ? atoi(argv[2]) : (int)env_long("LOP_BENCH_REPS", 5);
    if (pairs < 1) pairs = 1;
    if (reps  < 1) reps  = 1;

    // Warm up: allocate this thread's TLS buffer and prime caches / branch predictors.
    profiler_enable();
    long warm = pairs / 10 + 1;
    run_loop(warm);
    profiler_disable();

    std::vector<double> per_event, on_ms, off_ms;
    for (int r = 0; r < reps; ++r) {
        double off = run_loop(pairs);          // tracing OFF (wrappers no-op)
        profiler_enable();
        double on = run_loop(pairs);           // tracing ON (full emit path)
        profiler_disable();
        per_event.push_back((on - off) / (double)(2 * pairs));
        on_ms.push_back(on / 1e6);
        off_ms.push_back(off / 1e6);
    }

    // Emit the accumulated enabled-run events to a trace for bench_report.py.
    profiler_flush();

    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    long enabled_events = 2 * (warm + (long)reps * pairs);
    printf("BENCH pairs_per_rep=%ld reps=%d enabled_events=%ld off_ms=%.3f on_ms=%.3f per_event_ns=%.3f\n",
           pairs, reps, enabled_events, median(off_ms), median(on_ms), median(per_event));
    return 0;
}
