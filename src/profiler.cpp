/**
 * Copyright (c) 2025 Krzysztof Badziak
 * Copyright (c) 2021-2024 Intel Corporation
 *
 * This file contains code that was originally licensed under Apache License 2.0
 * and has been modified. The original code is Copyright (c) 2021-2024 Intel Corporation.
 *
 * The original code is licensed under the Apache License, Version 2.0:
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * My modifications to this file are licensed under the MIT License:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 
#include <thread>
#include <list>
#include <mutex>
#include <vector>
#include <cstring>
#include <map>
#include <climits>
#include <stdint.h>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <queue>
#include <string>
#include <condition_variable>
#include <cstdio>
#include <new>
#include <inttypes.h>

#include "profiler.h"

#define CUSTOM_TLS_SIZE 0x10000
#define LOP_BUFFER_SIZE 0x400000U
#define LOP_SPILL_THROTTLE_MS 2 // gap the spiller leaves between segments to stay gentle on IO

#if defined(_WIN32) || defined(_WIN64)
# define NOMINMAX
# include <windows.h>
# include <process.h> // _getpid
# define compiler_barrier() _ReadWriteBarrier()
# define get_process_id() _getpid()
#else
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
# define compiler_barrier() __asm__ __volatile__("" ::: "memory")
# define get_process_id() getpid()
#endif

#pragma warning(disable: 4996)

namespace LOP {

enum event_type : uint32_t {
    CALL_BEGIN,
    CALL_END,
    CALL_BEGIN_META,
    CALL_END_META,
    COUNTER_INT,
    FLOW_START,
    FLOW_FINISH,
};

struct Event {
    uint64_t timestamp;
    const char* name;
    uint64_t metadata;
    event_type type;
};

struct EventBuffer {
    Event* next_event; // Must be first field!!! For simplicty, because its accessed
                       // in critical part of asm and I don't want extra offsets there.
    Event* events;
    // Pre-allocated buffer the thread swaps to when "events" fills up. The owning thread
    // consumes it (exchange to nullptr) and the scheduler thread refills it, so it is atomic.
    // Never touched by the asm hot path, so this costs nothing there.
    std::atomic<Event*> events_backup{nullptr};
    uint64_t thread_id = 0;

    EventBuffer();
    ~EventBuffer();
};

struct CustomTLS {
    EventBuffer event_buffer;
};

struct ProfilerEngine {

    struct BufferState {
        Event* next_event;
        Event* events;
        uint64_t thread_id = 0;
    };

#if LOP_SPILL_TO_DISK
    // One of these prefixes every segment in the spill file, and a copy is also kept in the
    // in-memory "spill_segments" index so flush can compute tsc_base without touching disk.
    struct SpillSegmentHeader {
        uint64_t thread_id;
        uint64_t event_count;
        uint64_t min_timestamp; // earliest timestamp in the segment (its tsc_base contribution)
    };
#endif

    ProfilerEngine();
    ~ProfilerEngine();

    void add_event_buffer(EventBuffer* event_buffer);
    void remove_event_buffer(EventBuffer* event_buffer);
    void handle_depleted_buffer(EventBuffer* depleted_event_buffer);

    // Allocates a fresh event buffer. With spilling on, never returns null: on OOM it boosts
    // the spiller to free RAM (by dumping filled buffers to disk) and retries.
    Event* allocate_event_buffer();

    static void scheduler_loop();

    void enable();
    void disable();
    void flush(const char* suffix = nullptr);
    void flush_buffers(const char* suffix, const std::vector<BufferState>& buffers);
    void emit_one_buffer(FILE* file, const BufferState& buffer, unsigned pid, uint64_t tsc_base,
                         bool& first_event, std::map<uint64_t, Event>& counter_events);

#if LOP_SPILL_TO_DISK
    static void writer_loop();                       // background spiller thread body
    static void set_writer_priority(bool high);      // low while idle, boosted under memory pressure
    bool write_segment_to_disk(const BufferState& seg); // returns false on IO error (seg restored to RAM)
    bool ensure_spill_file_open();                   // lazily creates the temp file
    void reset_spill_state();                        // close + delete temp file, clear index
    void emit_spilled_segments(FILE* file, unsigned pid, uint64_t tsc_base,
                               bool& first_event, std::map<uint64_t, Event>& counter_events);
    void park_writer();                              // pause the spiller (flush quiescence)
    void unpark_writer();                            // resume the spiller
#endif

    CustomTLS** custom_tls; // Must be first field!!! For simplicty, because its accessed
                            // in critical part of asm and I don't want extra offsets there.
    bool enabled;
    bool flushed;
    bool running;

    uint64_t tsc_enable;

    double ticks_per_ns_ratio;

    std::mutex buffers_mutex;
    std::mutex control_mutex;

    bool scheduler_run;
    std::thread scheduler_thread;
    std::queue<EventBuffer*> scheduler_queue; // buffers that need a fresh backup allocated
    std::mutex scheduler_queue_mutex;

    std::list<EventBuffer*> event_buffers;     // live per-thread buffers
    std::list<BufferState> filled_buffers;     // buffers swapped out after filling up, kept until flush
    std::mutex filled_mutex;

#if LOP_SPILL_TO_DISK
    // Background spiller. Drains filled_buffers (oldest first) to a temp file and frees the RAM.
    bool writer_run;
    std::atomic<bool> memory_pressure{false};   // set on OOM: spiller drains everything ASAP
    std::mutex writer_mutex;                     // pairs with writer_cv (wake / throttle / park)
    std::condition_variable writer_cv;
    bool writer_parked;                          // flush asks the spiller to hold off the file/list
    bool writer_park_ack;                        // spiller confirms it has parked
    std::mutex spill_file_mutex;                 // guards spill_file + spill_segments (NOT filled_mutex)
    FILE* spill_file;                            // lazily created temp file
    std::string spill_path;
    std::vector<SpillSegmentHeader> spill_segments; // in-memory index, FIFO append order
    std::thread writer_thread;                   // MUST be last: starts as soon as it is constructed
#endif

    std::chrono::system_clock::time_point time_enable;
};

inline ProfilerEngine g_lop_inst;

extern "C" {
    // For windows, these are implemented in profiler_asm.asm (via MASM/ml64.exe).
    // For linux, these are implemented in profiler_asm.cpp (via inline assembly).
    uint64_t _asm_fast_rdtsc();
    uint64_t _asm_get_tid();

    // Default event emitters.
    void _asm_emit_begin_event(ProfilerEngine*, const char*);
    void _asm_emit_end_event(ProfilerEngine*, const char*);
    void _asm_emit_endbegin_event(ProfilerEngine*, const char*, const char*);
    void _asm_emit_immediate_event(ProfilerEngine*, const char*);

    void _asm_emit_begin_meta_event(ProfilerEngine*, const char*, uint64_t);
    void _asm_emit_end_meta_event(ProfilerEngine*, const char*, uint64_t);
    void _asm_emit_counter_event(ProfilerEngine*, const char*, uint64_t);
    void _asm_emit_immediate_meta_event(ProfilerEngine*, const char*, uint64_t);

    void _asm_emit_flow_start_event(ProfilerEngine*, const char*, uint64_t);
    void _asm_emit_flow_finish_event(ProfilerEngine*, const char*, uint64_t);

    CustomTLS* allocate_custom_tls() {
        return new CustomTLS;
    }

    void exhaustion_handler(EventBuffer* depleted_event_buffer) {
        g_lop_inst.handle_depleted_buffer(depleted_event_buffer);
    }
};

void profiler_enable() {
    g_lop_inst.enable();
}
void profiler_disable() {
    g_lop_inst.disable();
}
void profiler_flush(const char* suffix) {
    g_lop_inst.flush(suffix);
}

ProfilerEngine::ProfilerEngine()
:   custom_tls(new CustomTLS* [CUSTOM_TLS_SIZE]),
    enabled(false),
    flushed(true),
    running(false),
    tsc_enable(0),
    ticks_per_ns_ratio(0.0),
    buffers_mutex(),
    control_mutex(),
#if LOP_DOUBLE_BUFFER
    scheduler_run(true),
#else
    scheduler_run(false),
#endif
    scheduler_thread(scheduler_loop),
    scheduler_queue(),
    scheduler_queue_mutex(),
    event_buffers(),
    filled_buffers(),
    filled_mutex(),
#if LOP_SPILL_TO_DISK
    writer_run(true),
    writer_mutex(),
    writer_cv(),
    writer_parked(false),
    writer_park_ack(false),
    spill_file_mutex(),
    spill_file(nullptr),
    spill_path(),
    spill_segments(),
    writer_thread(writer_loop),
#endif
    time_enable()
{
    char* disable_string = std::getenv("LOP_DISABLE");
    if (!disable_string || !static_cast<uint32_t>(std::stoi(disable_string))) {
        // Kinda hacky way of estimating frequency. Result is overriden later at flush if
        // run is longer than a 1 second because we gather similar statistics for whole run
        // as well and the longer time we average over, the better accuracy we have.
        auto chrono_start = std::chrono::system_clock::now();
        uint64_t start_tsc = _asm_fast_rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        uint64_t stop_tsc = _asm_fast_rdtsc();
        auto chrono_end = std::chrono::system_clock::now();

        double unix_time_diff_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(chrono_end.time_since_epoch()).count() -
            std::chrono::duration_cast<std::chrono::nanoseconds>(chrono_start.time_since_epoch()).count()
        );

        double tsc_ticks = static_cast<double>(stop_tsc - start_tsc);
        ticks_per_ns_ratio = tsc_ticks / unix_time_diff_ns;
        printf("Estimated TSC freq: %f GHz\n", ticks_per_ns_ratio);
        printf("                    %f ticks per nanosecond\n", ticks_per_ns_ratio);

        memset(custom_tls, 0, sizeof(CustomTLS*)*CUSTOM_TLS_SIZE);

        running = true;
    }
}

void ProfilerEngine::enable() {
    const std::lock_guard<std::mutex> lock(control_mutex);
    if (running && !enabled) {
        flushed = false;
        enabled = true;
        
        // Generate special event so that we can track on the trace at what point of UNIX time it was enabled.
        emit_begin_event("lop_engine_enable");
        time_enable = std::chrono::system_clock::now();
        tsc_enable = _asm_fast_rdtsc();
        emit_end_meta_event("lop_engine_enable", std::chrono::duration_cast<std::chrono::nanoseconds>(time_enable.time_since_epoch()).count());
    }
}

void ProfilerEngine::disable() {
    const std::lock_guard<std::mutex> lock(control_mutex);
    if (running && enabled) {
        // Generate special event so that we can track on the trace at what point of UNIX time it was disabled.
        emit_begin_event("lop_engine_disable");
        auto time_disable = std::chrono::system_clock::now();
        emit_end_meta_event("lop_engine_disable", std::chrono::duration_cast<std::chrono::nanoseconds>(time_disable.time_since_epoch()).count());
        
        enabled = false;
    } 
}

void ProfilerEngine::scheduler_loop()
{
    while (g_lop_inst.scheduler_run)
    {
        // Pull one buffer that recently swapped to its backup and now needs a new one.
        EventBuffer* event_buffer = nullptr;
        {
            const std::lock_guard<std::mutex> lock(g_lop_inst.scheduler_queue_mutex);
            if (!g_lop_inst.scheduler_queue.empty()) {
                event_buffer = g_lop_inst.scheduler_queue.front();
                g_lop_inst.scheduler_queue.pop();
            }
        }

        if (!event_buffer)
        {
            // Nothing to do, stay negligible while idle. A thread cannot deplete a fresh
            // buffer faster than LOP_BUFFER_SIZE * ~8ns (~33ms for the default size), so a
            // 5ms poll is plenty to prepare the next backup in time. If a burst ever beats
            // us to it, handle_depleted_buffer() falls back to a synchronous allocation.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Prepare the next backup ahead of the next depletion. Only the scheduler ever stores
        // a non-null backup (the owning thread only ever consumes it), so a plain check-then-
        // store is race free, and we never allocate one we already have.
        if (event_buffer->events_backup.load() == nullptr) {
            event_buffer->events_backup.store(g_lop_inst.allocate_event_buffer());
        }
    }
}

Event* ProfilerEngine::allocate_event_buffer() {
#if LOP_SPILL_TO_DISK
    Event* p = new (std::nothrow) Event[LOP_BUFFER_SIZE];
    if (p) return p;

    // Out of memory. Instead of crashing, turn the spiller into a relief valve: tell it to
    // drain filled buffers to disk at boosted priority (each freed buffer is ~128 MB) and keep
    // retrying. As soon as the spiller frees enough, the allocation succeeds and we move on.
    printf("LOP: buffer allocation failed - draining buffers to disk to reclaim RAM...\n");
    fflush(stdout);
    memory_pressure.store(true);
    {
        const std::lock_guard<std::mutex> lock(writer_mutex);
        writer_cv.notify_all();
    }
    while (p == nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // yield the CPU to the spiller
        p = new (std::nothrow) Event[LOP_BUFFER_SIZE];
    }
    return p;
#else
    return new Event[LOP_BUFFER_SIZE];
#endif
}

#if LOP_SPILL_TO_DISK
void ProfilerEngine::set_writer_priority(bool high) {
    // Best-effort: failures (e.g. missing privileges) are fine - priority is only an optimization.
#if defined(_WIN32) || defined(_WIN64)
    SetThreadPriority(GetCurrentThread(), high ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
#else
    struct sched_param sp;
    sp.sched_priority = 0;
    // SCHED_IDLE <-> SCHED_OTHER avoids the "nice ratchet" (a thread can't lower its nice and
    // then raise it back without privileges, but switching scheduling policy back is allowed).
    if (pthread_setschedparam(pthread_self(), high ? SCHED_OTHER : SCHED_IDLE, &sp) != 0) {
        setpriority(PRIO_PROCESS, 0, high ? 0 : 19);
    }
#endif
}

bool ProfilerEngine::ensure_spill_file_open() {
    if (spill_file) return true;

    const char* dir = std::getenv("TMPDIR");
#if defined(_WIN32) || defined(_WIN64)
    if (!dir) dir = std::getenv("TEMP");
    if (!dir) dir = ".";
#else
    if (!dir) dir = "/tmp";
#endif
    char path[256];
    snprintf(path, sizeof(path), "%s/lop_spill_pid%u.tmp", dir, static_cast<unsigned>(get_process_id()));
    spill_path = path;
    spill_file = fopen(spill_path.c_str(), "wb+");
    if (!spill_file) {
        printf("LOP: couldn't open spill file '%s'; keeping buffers in RAM.\n", spill_path.c_str());
        spill_path.clear();
    }
    return spill_file != nullptr;
}

void ProfilerEngine::reset_spill_state() {
    const std::lock_guard<std::mutex> lock(spill_file_mutex);
    if (spill_file) {
        fclose(spill_file);
        spill_file = nullptr;
    }
    if (!spill_path.empty()) {
        remove(spill_path.c_str());
        spill_path.clear();
    }
    spill_segments.clear();
}

bool ProfilerEngine::write_segment_to_disk(const BufferState& seg) {
    const uint64_t count = static_cast<uint64_t>(seg.next_event - seg.events);

    // The earliest timestamp in the segment is its contribution to the global tsc_base. We scan
    // for it here (on the low-priority thread, while the data is cache-warm) so that flush can
    // compute tsc_base from headers alone, without re-reading event payloads from disk.
    uint64_t min_ts = count ? seg.events[0].timestamp : 0;
    for (uint64_t i = 1; i < count; ++i) {
        if (seg.events[i].timestamp < min_ts) min_ts = seg.events[i].timestamp;
    }
    SpillSegmentHeader hdr{ seg.thread_id, count, min_ts };

    bool ok = false;
    {
        const std::lock_guard<std::mutex> lock(spill_file_mutex);
        if (ensure_spill_file_open()) {
            ok = fwrite(&hdr, sizeof(hdr), 1, spill_file) == 1
                 && (count == 0 || fwrite(seg.events, sizeof(Event), count, spill_file) == count);
            if (ok) spill_segments.push_back(hdr);
        }
    }

    if (ok) {
        delete[] seg.events; // ownership was transferred to us when we popped it; ~128 MB freed
        return true;
    }

    // IO failure: put the segment back at the front so flush still emits it from RAM. We never
    // lose events; we just don't get the memory saving for this one.
    {
        const std::lock_guard<std::mutex> lock(filled_mutex);
        filled_buffers.push_front(seg);
    }
    return false;
}

void ProfilerEngine::writer_loop() {
    ProfilerEngine& self = g_lop_inst;
    set_writer_priority(false);
    bool boosted = false;

    while (self.writer_run) {
        // Park handshake: flush asks us to stop touching the file/list while it reads them.
        {
            std::unique_lock<std::mutex> lock(self.writer_mutex);
            if (self.writer_parked) {
                self.writer_park_ack = true;
                self.writer_cv.notify_all();
                self.writer_cv.wait(lock, [&] { return !self.writer_parked || !self.writer_run; });
                continue;
            }
        }

        const bool pressure = self.memory_pressure.load();
        if (pressure && !boosted) { set_writer_priority(true); boosted = true; }

        // Critical section 1: pop the oldest filled buffer (FIFO preserves per-thread fill order).
        // Under memory pressure we drain everything; otherwise we leave a small RAM cushion.
        BufferState seg{};
        bool have = false;
        {
            const std::lock_guard<std::mutex> lock(self.filled_mutex);
            const size_t keep = pressure ? 0u : static_cast<size_t>(LOP_SPILL_RAM_THRESHOLD);
            if (self.filled_buffers.size() > keep) {
                seg = self.filled_buffers.front();
                self.filled_buffers.pop_front();
                have = true;
            }
        }
        // filled_mutex released here: the slow fwrite below never blocks handle_depleted_buffer.

        if (!have) {
            if (boosted) { // nothing left to spill: pressure is relieved, go back to idle priority
                self.memory_pressure.store(false);
                set_writer_priority(false);
                boosted = false;
            }
            std::unique_lock<std::mutex> lock(self.writer_mutex);
            self.writer_cv.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }

        if (!self.write_segment_to_disk(seg)) {
            std::unique_lock<std::mutex> lock(self.writer_mutex);
            self.writer_cv.wait_for(lock, std::chrono::milliseconds(50)); // IO error: back off
            continue;
        }

        if (!pressure) { // gentle throttle between segments; skipped while relieving pressure
            std::unique_lock<std::mutex> lock(self.writer_mutex);
            self.writer_cv.wait_for(lock, std::chrono::milliseconds(LOP_SPILL_THROTTLE_MS));
        }
    }
}

void ProfilerEngine::park_writer() {
    std::unique_lock<std::mutex> lock(writer_mutex);
    writer_parked = true;
    writer_cv.notify_all();
    writer_cv.wait(lock, [this] { return writer_park_ack || !writer_run; });
}

void ProfilerEngine::unpark_writer() {
    std::unique_lock<std::mutex> lock(writer_mutex);
    writer_parked = false;
    writer_park_ack = false;
    writer_cv.notify_all();
}

void ProfilerEngine::emit_spilled_segments(FILE* file, unsigned pid, uint64_t tsc_base,
                                           bool& first_event, std::map<uint64_t, Event>& counter_events) {
    if (spill_segments.empty() || !spill_file) return;

    // The spiller is parked, so we have exclusive access to the file. Replay it front-to-back -
    // the same order the segments were filled - into one reused scratch buffer, one at a time.
    fflush(spill_file);
    fseek(spill_file, 0, SEEK_SET);
    Event* scratch = new Event[LOP_BUFFER_SIZE];
    for (const SpillSegmentHeader& indexed : spill_segments) {
        SpillSegmentHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, spill_file) != 1) break;
        const uint64_t count = hdr.event_count;
        if (count) {
            if (fread(scratch, sizeof(Event), count, spill_file) != count) break;
        }
        BufferState bs;
        bs.events = scratch;
        bs.next_event = scratch + count;
        bs.thread_id = hdr.thread_id;
        emit_one_buffer(file, bs, pid, tsc_base, first_event, counter_events);
    }
    delete[] scratch;
}
#endif // LOP_SPILL_TO_DISK

void ProfilerEngine::emit_one_buffer(FILE* file, const BufferState& buffer, unsigned pid,
                                     uint64_t tsc_base, bool& first_event,
                                     std::map<uint64_t, Event>& COUNTER_events) {
    Event* event = buffer.events;
    uint64_t event_thread_id = buffer.thread_id;
    for (; event < buffer.next_event; ++event) {
        auto tsc_diff = event->timestamp - tsc_base;
        auto time_ns = static_cast<uint64_t>(static_cast<double>(tsc_diff) / ticks_per_ns_ratio);

        if (event->type == COUNTER_INT) {
            // Sort COUNTER_INT events in the meantime using ordered map and process them later.
            // Chrome tracing requires that they are sorted by timestamps, otherwise it glitches.
            // And no, that "feature" is not documented anywhere.
            // Stored BY VALUE (not by pointer): when this buffer came from disk, "event" points
            // into a scratch buffer that is freed before the deferred counter pass runs.
            COUNTER_events.insert({ event->timestamp, *event });
        }
        else if (event->type == CALL_BEGIN || event->type == CALL_END) {
            const char* eventPh = (event->type == CALL_BEGIN) ? "B" : "E";
            fprintf(file,
                "%c{"
                "\"tid\":\"%" PRIx64 "\","
                "\"pid\":%u,"
                "\"ts\":%" PRIu64 ".%03" PRIu64 ","
                "\"name\":\"%s\","
                "\"ph\":\"%s\""
                "}\n",
                first_event ? ' ' : ',', event_thread_id, pid, time_ns / 1000, time_ns % 1000, event->name, eventPh);
        }
        else if (event->type == CALL_BEGIN_META || event->type == CALL_END_META) {
            const char* eventPh = (event->type == CALL_BEGIN_META) ? "B" : "E";
            const char* metaName = (event->type == CALL_BEGIN_META) ? "b_meta" : "e_meta";
            fprintf(file,
                "%c{"
                "\"tid\":\"%" PRIx64 "\","
                "\"pid\":%u,"
                "\"ts\":%" PRIu64 ".%03" PRIu64 ","
                "\"name\":\"%s\","
                "\"ph\":\"%s\","
                "\"args\":{"
                "\"%s\":\"%" PRIx64 "\""
                "}"
                "}\n",
                first_event ? ' ' : ',', event_thread_id, pid, time_ns / 1000, time_ns % 1000, event->name, eventPh, metaName, event->metadata);
        }
        else if (event->type == FLOW_START || event->type == FLOW_FINISH) {
            const char* eventPh = (event->type == FLOW_START) ? "s" : "f";
            uint32_t truncated_flow_id = (uint32_t)event->metadata; // perfetto supports only 32bit flow IDs.
            fprintf(file,
                "%c{"
                "\"tid\":\"%" PRIx64 "\","
                "\"pid\":%u,"
                "\"ts\":%" PRIu64 ".%03" PRIu64 ","
                "\"name\":\"flow\","
                "\"ph\":\"%s\","
                "\"bp\":\"e\","
                "\"id\":%" PRIu32 ","
                "\"args\":{"
                "\"flow_id\":\"%" PRIx64 "\""
                "}"
                "}\n",
                first_event ? ' ' : ',', event_thread_id, pid, time_ns / 1000, time_ns % 1000, eventPh, truncated_flow_id, event->metadata);
        }
        else {
            printf("Unknown event type. Bailing out.\n");
            return;
        }

        first_event = false;
    }
}

void ProfilerEngine::flush_buffers(const char* suffix, const std::vector<BufferState>& buffers) {
    // We REALLY want these two to happen together.
    compiler_barrier();
    auto tsc_disable = _asm_fast_rdtsc();
    auto time_disable = std::chrono::system_clock::now();
    compiler_barrier();

    uint64_t events_counter = 0;
#if LOP_SPILL_TO_DISK
    // Buffers already spilled to disk are emitted first (they are the oldest), so count them first.
    for (const SpillSegmentHeader& hdr : spill_segments) {
        printf("Got %" PRIu64 "/%" PRIu32 " (%" PRIu64 "%%) spilled events of thread: %" PRIx64 "\n",
            hdr.event_count, LOP_BUFFER_SIZE, hdr.event_count * 100 / LOP_BUFFER_SIZE, hdr.thread_id);
        events_counter += hdr.event_count;
    }
#endif
    for (const BufferState& buffer : buffers) {
        uint64_t events_in_buffer = buffer.next_event - buffer.events;
        printf("Got %" PRIu64 "/%" PRIu32 " (%" PRIu64 "%%) events in buffer of thread: %" PRIx64 "\n",
            events_in_buffer,
            LOP_BUFFER_SIZE,
            events_in_buffer * 100 / LOP_BUFFER_SIZE,
            buffer.thread_id);
        events_counter += events_in_buffer;
    }

    printf("TOTAL EVENTS: %" PRIu64 "\n", events_counter);

    auto pid = get_process_id();
    double unix_time_diff_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time_disable.time_since_epoch()).count() -
        std::chrono::duration_cast<std::chrono::nanoseconds>(time_enable.time_since_epoch()).count()
        );

    char name[200];
    if (suffix) snprintf(name, 200, "events_pid%u_ts%" PRIu64 "_%s.json", pid, static_cast<uint64_t>(unix_time_diff_ns / 1000), suffix);
    else        snprintf(name, 200, "events_pid%u_ts%" PRIu64 ".json", pid, static_cast<uint64_t>(unix_time_diff_ns / 1000));

    std::string cleaned_name(name);
    std::replace(cleaned_name.begin(), cleaned_name.end(), '/', '_');
    std::replace(cleaned_name.begin(), cleaned_name.end(), '\\', '_');

    printf("Creating file: %s\n", cleaned_name.c_str()); fflush(stdout);
    auto file = fopen(name, "w");
    fprintf(file,"{\"displayTimeUnit\": \"ns\", \"traceEvents\": [\n");

    // Find first event, timewise - across BOTH the spilled segments and what is still in RAM.
    uint64_t tsc_base = std::numeric_limits<uint64_t>::max();
#if LOP_SPILL_TO_DISK
    for (const SpillSegmentHeader& hdr : spill_segments) // headers carry each segment's min (no disk read)
        if (hdr.event_count && hdr.min_timestamp < tsc_base) tsc_base = hdr.min_timestamp;
#endif
    for (const BufferState& buffer : buffers) {
        Event* event = buffer.events;
        for (; event < buffer.next_event; ++event)
            if (event->timestamp < tsc_base) tsc_base = event->timestamp;
    }

    if (unix_time_diff_ns > 1000000000.0) {
        // For long (>1s) profiling sessions, overhead from start/end timestamp measurements is small enough that
        // if we base our frequency on those measurements, it will bring more accurate results than hacky estimation
        // code in the constructor.
        double tsc_ticks = static_cast<double>(tsc_disable - tsc_enable);
        ticks_per_ns_ratio = tsc_ticks / unix_time_diff_ns;
        printf("Long run detected. Will use frequency measured over time.\n");
        printf("Measured %f ticks per nanosecond\n", ticks_per_ns_ratio);
    }
    bool first_event = true;
    std::map<uint64_t, Event> COUNTER_events;
    // Emit in fill order: spilled-to-disk segments (oldest), then in-RAM filled buffers, then live.
#if LOP_SPILL_TO_DISK
    emit_spilled_segments(file, static_cast<unsigned>(pid), tsc_base, first_event, COUNTER_events);
#endif
    for (const BufferState& buffer : buffers)
        emit_one_buffer(file, buffer, static_cast<unsigned>(pid), tsc_base, first_event, COUNTER_events);

    for (const auto& [timestamp, event] : COUNTER_events) {
        auto tsc_diff = timestamp - tsc_base;
        auto time_ns = static_cast<uint64_t>(static_cast<double>(tsc_diff) / ticks_per_ns_ratio);
        fprintf(file,
            "%c{"
            "\"pid\": %u,"
            "\"ts\":%" PRIu64 ".%03" PRIu64 ","
            "\"name\":\"%s\","
            "\"ph\":\"C\","
            "\"args\":{"
            "\"val\":%" PRIu64 ""
            "}"
            "}\n",
            first_event ? ' ' : ',', pid, time_ns / 1000, time_ns % 1000, event.name, event.metadata);

        first_event = false;
    }

    fprintf(file,"]}");
    fclose(file);
}

void ProfilerEngine::flush(const char* suffix) {
    const std::lock_guard<std::mutex> control_lock(control_mutex);
    printf("ProfilerEngine::flush at PID:%u\n", get_process_id());
    if (suffix) printf("Flushing for suffix: \"%s\"\n", suffix);
    fflush(stdout);

    if (enabled) {
        printf("Tried to flush enabled LOP. Doing nothing.");
        return;
    }

    if (flushed) {
        printf("Tried to flush already flushed LOP. Doing nothing.");
        return;
    }

#if LOP_SPILL_TO_DISK
    // Park the spiller before we touch the spill file or the filled list, so it can't race us.
    // This MUST happen before we take filled_mutex: the spiller briefly holds filled_mutex to
    // pop a segment, and parking waits for it to reach a safe point.
    park_writer();
#endif
    const std::lock_guard<std::mutex> buffer_lock(buffers_mutex);
    const std::lock_guard<std::mutex> filled_lock(filled_mutex);

    // Gather everything for a single combined trace: the buffers that filled up and were
    // swapped out during the run, plus what is left in the current live buffers.
    std::vector<BufferState> buffers;
    buffers.reserve(filled_buffers.size() + event_buffers.size());
    for (const BufferState& filled : filled_buffers) {
        buffers.push_back(filled);
    }
    for (auto& event_buffer : event_buffers)
    {
        BufferState buffer;
        buffer.events = event_buffer->events;
        buffer.next_event = event_buffer->next_event;
        buffer.thread_id = event_buffer->thread_id;

        buffers.push_back(buffer);
    }

    flush_buffers(suffix, buffers);

    // Free the retained filled buffers and reset the live ones for reuse.
    for (const BufferState& filled : filled_buffers) {
        delete[] filled.events;
    }
    filled_buffers.clear();
    for (EventBuffer* buffer : event_buffers) {
        buffer->next_event = buffer->events; // re-initialize current buffers
    }

#if LOP_SPILL_TO_DISK
    // The spilled segments have been merged into the trace; delete the temp file so a subsequent
    // re-enabled session starts with a clean slate, then let the spiller run again.
    reset_spill_state();
    memory_pressure.store(false);
    unpark_writer();
#endif

    flushed = true;

    printf("ProfilerEngine::flush finished\n"); fflush(stdout);
}

ProfilerEngine::~ProfilerEngine() {
    printf("ProfilerEngine::~ProfilerEngine at PID:%u\n", get_process_id()); fflush(stdout);

    scheduler_run = false;
    scheduler_thread.join();

    if (running) {
        disable();

        if (!flushed) {
            flush(); // flush() parks/unparks the still-live spiller and emits the spilled segments
        }
    }

#if LOP_SPILL_TO_DISK
    // Now stop the spiller for good and make sure no temp file is left behind.
    writer_run = false;
    {
        std::unique_lock<std::mutex> lock(writer_mutex);
        writer_parked = false;
        writer_cv.notify_all();
    }
    writer_thread.join();
    reset_spill_state();
#endif

    printf("ProfilerEngine::~ProfilerEngine finished\n"); fflush(stdout);
}

void ProfilerEngine::add_event_buffer(EventBuffer* event_buffer) {
    const std::lock_guard<std::mutex> lock(buffers_mutex);
    event_buffers.push_back(event_buffer);
}

void ProfilerEngine::remove_event_buffer(EventBuffer* event_buffer) {
    const std::lock_guard<std::mutex> lock(buffers_mutex);
    event_buffers.remove(event_buffer);
}

void ProfilerEngine::handle_depleted_buffer(EventBuffer* depleted_event_buffer) {
    // This runs on the very thread that filled its own buffer, called from the asm fallback.
    // Because only this thread writes this buffer's "events"/"next_event", the swap needs no
    // global disable, no draining and no atomics on those pointers - it is naturally lossless.

    // Take the pre-allocated backup. If the scheduler hasn't managed to prepare one yet (a
    // burst, or many threads depleting at once and serializing the scheduler), allocate one
    // synchronously so we never crash - only this one unlucky event pays for it.
    Event* fresh_buffer = depleted_event_buffer->events_backup.exchange(nullptr);
    if (fresh_buffer == nullptr) {
        fresh_buffer = allocate_event_buffer(); // relief-valve aware: never crashes on OOM
    }

    // Retain the just-filled buffer so it gets parsed (or spilled) at flush. Ownership of the old
    // "events" allocation moves into the filled list (freed in flush(), or by the spiller).
    {
        const std::lock_guard<std::mutex> lock(filled_mutex);
        filled_buffers.push_back({ depleted_event_buffer->next_event,
                                   depleted_event_buffer->events,
                                   depleted_event_buffer->thread_id });
    }
#if LOP_SPILL_TO_DISK
    // Nudge the spiller so it considers the newly filled buffer promptly (it would otherwise
    // pick it up on its next 5 ms poll). Cheap and off the asm hot path.
    writer_cv.notify_one();
#endif

    // Swap the fresh buffer in. The asm caller is paused mid-fallback and will reload
    // next_event from memory when it retries, so the barrier is enough - no atomics needed.
    depleted_event_buffer->events = fresh_buffer;
    depleted_event_buffer->next_event = fresh_buffer;
    compiler_barrier();

    // Ask the scheduler to prepare the next backup ahead of the next depletion.
    {
        const std::lock_guard<std::mutex> lock(scheduler_queue_mutex);
        scheduler_queue.push(depleted_event_buffer);
    }
}

EventBuffer::EventBuffer() {
    thread_id = _asm_get_tid();
    events = g_lop_inst.allocate_event_buffer();

#if LOP_DOUBLE_BUFFER
    events_backup = g_lop_inst.allocate_event_buffer();
#endif

    next_event = events;
    if (events) {
        g_lop_inst.add_event_buffer(this);
    }
    else {
        printf("Couldn't allocate ring buffer.\n");
    }
}

EventBuffer::~EventBuffer() {
    printf("EventBuffer::~EventBuffer at TID:%" PRIu64 "\n", thread_id); fflush(stdout);
    g_lop_inst.remove_event_buffer(this);
    if (events) {
        delete[] events;
        events = nullptr;
    }

    Event* backup = events_backup.exchange(nullptr);
    if (backup) {
        delete[] backup;
    }

    thread_id = -1;
    printf("EventBuffer::~EventBuffer finished\n"); fflush(stdout);
}

void emit_begin_event(const char* name) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_begin_event(&g_lop_inst, name);
    compiler_barrier();
}

void emit_end_event(const char* name) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_end_event(&g_lop_inst, name);
    compiler_barrier();
}

void emit_endbegin_event(const char* end_name, const char* begin_name) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_endbegin_event(&g_lop_inst, end_name, begin_name);
    compiler_barrier();
}

void emit_immediate_event(const char* name) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_immediate_event(&g_lop_inst, name);
    compiler_barrier();
}

void emit_begin_meta_event(const char* name, uint64_t metadata) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_begin_meta_event(&g_lop_inst, name, metadata);
    compiler_barrier();
}

void emit_end_meta_event(const char* name, uint64_t metadata) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_end_meta_event(&g_lop_inst, name, metadata);
    compiler_barrier();
}

void emit_immediate_meta_event(const char* name, uint64_t metadata) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_immediate_meta_event(&g_lop_inst, name, metadata);
    compiler_barrier();
}

void emit_counter_event(const char* name, uint64_t count) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_counter_event(&g_lop_inst, name, count);
    compiler_barrier();
}

void emit_flow_start_event(const char* name, uint64_t flow_id) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_flow_start_event(&g_lop_inst, name, flow_id);
    compiler_barrier();
}

void emit_flow_finish_event(const char* name, uint64_t flow_id) {
    compiler_barrier();
    if (g_lop_inst.enabled) _asm_emit_flow_finish_event(&g_lop_inst, name, flow_id);
    compiler_barrier();
}
 
}; // namespace LOP
