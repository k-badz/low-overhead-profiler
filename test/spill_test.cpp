/**
 * Stress harness for the background spill-to-disk feature (LOP_SPILL_TO_DISK).
 *
 * Several threads each emit a large number of begin/end pairs - far more than fit in one event
 * buffer - so buffers rotate repeatedly and the background spiller writes most of them to disk.
 * A flow start/finish pair brackets each thread's work (and straddles rotation boundaries). At
 * flush, the single trace must therefore be reassembled from BOTH the spilled disk segments and
 * the buffers still in RAM, with nothing lost.
 *
 * It is meant to be compiled against a profiler built with a deliberately tiny LOP_BUFFER_SIZE
 * (to force rotation cheaply) or with a tight RAM cap (to exercise the OOM relief valve) - see
 * run_tests.sh, which builds the various configurations. Validate the output with validate.py.
 *
 * Usage: spill_test [pairs_per_thread]   (default 20000)
 */
#define LOP_IMPLEMENTATION   // header-only: this standalone test is the engine's one TU
#include "profiler.h"
#include <thread>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>

using namespace LOP;

// Static (program-lifetime) names, as the API requires - the profiler stores the pointer, not a
// copy, so it must stay valid until profiler_flush(). (Using a stack buffer here would dangle.)
static const char* kNames[4] = { "w0", "w1", "w2", "w3" };

static void worker(int id, int pairs) {
    const char* nb = kNames[id & 3];
    emit_flow_start_event("flow", 0xABCD0000u + id);
    for (int i = 0; i < pairs; ++i) {
        emit_begin_event(nb);
        emit_end_event(nb);
    }
    emit_flow_finish_event("flow", 0xABCD0000u + id);
}

int main(int argc, char** argv) {
    const int threads = 4;
    const int pairs = (argc > 1) ? atoi(argv[1]) : 20000;

    profiler_enable();
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i) ts.emplace_back(worker, i, pairs);
    for (auto& t : ts) t.join();
    profiler_disable();

    // Give the throttled, low-priority spiller time to drain most filled buffers to disk so the
    // flush below has to merge MANY disk segments back with whatever remains in RAM.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    profiler_flush();

    long total_pairs = (long)threads * pairs;
    printf("TEST emitted %ld begin + %ld end pairs across %d threads\n", total_pairs, total_pairs, threads);
    return 0;
}
