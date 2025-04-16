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

#include "profiler.h"

#define CUSTOM_TLS_SIZE 0x10000
#define LOP_BUFFER_SIZE 0x1000000LLU

#if defined(_WIN32) || defined(_WIN64)
# define compiler_barrier() _ReadWriteBarrier()
# define get_process_id() _getpid()
#else
#include <unistd.h>
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
    uint64_t thread_id;
    uint64_t metadata;
    event_type type;
};

struct EventBuffer {
    Event* next_event; // Must be first field!!! For simplicty, because its accessed
                       // in critical part of asm and I don't want extra offsets there.
    Event* events;
    uint64_t thread_id = 0;

    EventBuffer();
    ~EventBuffer();
};

struct CustomTLS {
    EventBuffer event_buffer;
};

struct ProfilerEngine {

    ProfilerEngine();
    ~ProfilerEngine();

    void add_event_buffer(EventBuffer* event_buffer);
    void remove_event_buffer(EventBuffer* event_buffer);

    void enable();
    void disable();
    void flush(const char* suffix = nullptr);

    CustomTLS** custom_tls; // Must be first field!!! For simplicty, because its accessed
                            // in critical part of asm and I don't want extra offsets there.
    volatile bool enabled;
    volatile bool flushed;
    volatile bool running;

    uint64_t tsc_enable;
    uint64_t tsc_disable;

    double ticks_per_ns_ratio;

    std::mutex buffers_mutex;
    std::list<EventBuffer*> event_buffers;
    std::chrono::system_clock::time_point time_enable;
    std::chrono::system_clock::time_point time_disable;
};

extern "C" {
    // For windows, these are implemented in profiler_asm.asm (via MASM/ml64.exe).
    // For linux, these are implemented in profiler_asm.cpp (via inline assembly).
    uint64_t _asm_fast_rdtsc();
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

    uint64_t _asm_get_tid();

    CustomTLS* allocate_custom_tls() {
        return new CustomTLS;
    }
};

inline ProfilerEngine g_lop_inst;

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
    tsc_disable(0),
    ticks_per_ns_ratio(0.0)
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
    if (running && enabled) {
        // Generate special event so that we can track on the trace at what point of UNIX time it was disabled.
        emit_begin_event("lop_engine_disable");
        tsc_disable = _asm_fast_rdtsc();
        time_disable = std::chrono::system_clock::now();
        emit_end_meta_event("lop_engine_disable", std::chrono::duration_cast<std::chrono::nanoseconds>(time_disable.time_since_epoch()).count());
        
        enabled = false;
    } 
}

void ProfilerEngine::flush(const char* suffix) {
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

    std::vector<Event> events_table;

    uint64_t events_counter = 0;
    {
        const std::lock_guard<std::mutex> lock(buffers_mutex);

        for (EventBuffer* buffer : event_buffers) {
            uint64_t events_in_buffer = buffer->next_event - buffer->events;
            printf("Got %llu/%llu (%llu%%) events in buffer of thread: %llx\n",
                events_in_buffer,
                LOP_BUFFER_SIZE,
                events_in_buffer * 100 / LOP_BUFFER_SIZE,
                buffer->thread_id);
            events_counter += events_in_buffer;
        }

        events_table.resize(events_counter);

        uint64_t event_offset = 0;
        for (EventBuffer* buffer : event_buffers) {
            uint64_t events_in_buffer = buffer->next_event - buffer->events;
            memcpy(events_table.data() + event_offset, buffer->events, events_in_buffer * sizeof(Event));
            
            // Patch thread ids.
            for (uint64_t i = 0; i < events_in_buffer; i++) {
                events_table.data()[i + event_offset].thread_id = buffer->thread_id;
            }

            event_offset += events_in_buffer;
            buffer->next_event = buffer->events; // re-initialize this buffer
        }
    }

    printf("TOTAL EVENTS: %llu\n", events_counter);

    if (events_counter > 0) {
        auto pid = get_process_id();

        double unix_time_diff_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(time_disable.time_since_epoch()).count() -
            std::chrono::duration_cast<std::chrono::nanoseconds>(time_enable.time_since_epoch()).count()
            );

        char name[200];
        if (suffix) snprintf(name, 200, "events_pid%u_ts%llu_%s.json", pid, static_cast<uint64_t>(unix_time_diff_ns / 1000), suffix);
        else        snprintf(name, 200, "events_pid%u_ts%llu.json", pid, static_cast<uint64_t>(unix_time_diff_ns / 1000));

        std::string cleaned_name(name);
        std::replace(cleaned_name.begin(), cleaned_name.end(), '/', '_');
        std::replace(cleaned_name.begin(), cleaned_name.end(), '\\', '_');

        printf("Creating file: %s\n", cleaned_name.c_str()); fflush(stdout);
        auto file = fopen(name, "w");
        fprintf(file,"{\"displayTimeUnit\": \"ns\", \"traceEvents\": [\n");

        // Find first event, timewise.
        uint64_t tsc_base = std::numeric_limits<uint64_t>::max();
        for (uint64_t i = 0; i < events_counter; ++i) {
            const auto& event = events_table[i];
            if (event.timestamp < tsc_base) tsc_base = event.timestamp;
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
        std::map<uint64_t, Event*> COUNTER_events;
        for (uint64_t i = 0; i < events_counter; ++i) {
            const auto& event = events_table[i];
            auto tsc_diff = event.timestamp - tsc_base;
            auto time_ns = static_cast<uint64_t>(static_cast<double>(tsc_diff) / ticks_per_ns_ratio);

            if (event.type == COUNTER_INT) {
                // Sort COUNTER_INT events in the meantime using ordered map and process them later.
                // Chrome tracing requires that they are sorted by timestamps, otherwise it glitches.
                // And no, that "feature" is not documented anywhere.
                COUNTER_events.insert({ event.timestamp, &events_table[i] });
            } else if (event.type == CALL_BEGIN || event.type == CALL_END) {
                const char* eventPh = (event.type == CALL_BEGIN) ? "B" : "E";
                fprintf(file,
                    "{"
                        "\"tid\":\"%llx\","
                        "\"pid\":%u,"
                        "\"ts\":%llu.%03llu,"
                        "\"name\":\"%s\","
                        "\"ph\":\"%s\""
                    "},\n",
                    event.thread_id, pid, time_ns/1000, time_ns%1000, event.name, eventPh);
            } else if (event.type == CALL_BEGIN_META || event.type == CALL_END_META) {
                const char* eventPh = (event.type == CALL_BEGIN_META) ? "B" : "E";
                const char* metaName = (event.type == CALL_BEGIN_META) ? "b_meta" : "e_meta";
                fprintf(file,
                    "{"
                        "\"tid\":\"%llx\","
                        "\"pid\":%u,"
                        "\"ts\":%llu.%03llu,"
                        "\"name\":\"%s\","
                        "\"ph\":\"%s\","
                        "\"args\":{"
                            "\"%s\":\"%llx\""
                        "}"
                    "},\n",
                    event.thread_id, pid, time_ns / 1000, time_ns % 1000, event.name, eventPh, metaName, event.metadata);
            } else if (event.type == FLOW_START || event.type == FLOW_FINISH) {
                const char* eventPh = (event.type == FLOW_START) ? "s" : "f";
                fprintf(file,
                    "{"
                        "\"tid\":\"%llx\","
                        "\"pid\":%u,"
                        "\"ts\":%llu.%03llu,"
                        "\"name\":\"flow\","
                        "\"ph\":\"%s\","
                        "\"bp\":\"e\","
                        "\"id\":%llu,"
                        "\"args\":{"
                            "\"flow_id\":\"%llx\""
                        "}"
                    "},\n",
                    event.thread_id, pid, time_ns / 1000, time_ns % 1000, eventPh, event.metadata, event.metadata);
            } else {
                printf("Unknown event type. Bailing out.\n");
                return;
            }
        }

        for (const auto& [timestamp, event] : COUNTER_events) {
            auto tsc_diff = timestamp - tsc_base;
            auto time_ns = static_cast<uint64_t>(static_cast<double>(tsc_diff) / ticks_per_ns_ratio);
            fprintf(file,
                "{"
                    "\"pid\": %u,"
                    "\"ts\":%llu.%03llu,"
                    "\"name\":\"%s\","
                    "\"ph\":\"C\","
                    "\"args\":{"
                        "\"val\":%llu"
                    "}"
                "},\n",
                pid, time_ns / 1000, time_ns % 1000, event->name, event->metadata);
        }

        fprintf(file,"{}]}");
        fclose(file);
    }

    flushed = true;

    printf("ProfilerEngine::flush finished\n"); fflush(stdout);
}

ProfilerEngine::~ProfilerEngine() {
    printf("ProfilerEngine::~ProfilerEngine at PID:%u\n", get_process_id()); fflush(stdout);
    
    if (running) {
        disable();

        if (!flushed) {
            flush();
        }
    }

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

EventBuffer::EventBuffer() {
    thread_id = _asm_get_tid();
    events = new Event[LOP_BUFFER_SIZE];
    next_event = events;
    if (events) {
        g_lop_inst.add_event_buffer(this);
    }
    else {
        printf("Couldn't allocate ring buffer.\n");
    }
}

EventBuffer::~EventBuffer() {
    printf("EventBuffer::~EventBuffer at TID:%llu\n", thread_id); fflush(stdout);
    g_lop_inst.remove_event_buffer(this);
    if (events) {
        delete[] events;
        events = nullptr;
    }

    thread_id = -1;
    printf("EventBuffer::~EventBuffer finished\n"); fflush(stdout);
}

void emit_begin_event(const char* name) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_begin_event(&g_lop_inst, name);
    }
    compiler_barrier();
}

void emit_end_event(const char* name) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_end_event(&g_lop_inst, name);
    }
    compiler_barrier();
}

void emit_endbegin_event(const char* end_name, const char* begin_name) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_endbegin_event(&g_lop_inst, end_name, begin_name);
    }
    compiler_barrier();
}

void emit_immediate_event(const char* name) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_immediate_event(&g_lop_inst, name);
    }
    compiler_barrier();
}

void emit_begin_meta_event(const char* name, uint64_t metadata) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_begin_meta_event(&g_lop_inst, name, metadata);
    }
    compiler_barrier();
}

void emit_end_meta_event(const char* name, uint64_t metadata) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_end_meta_event(&g_lop_inst, name, metadata);
    }
    compiler_barrier();
}

void emit_immediate_meta_event(const char* name, uint64_t metadata) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_immediate_meta_event(&g_lop_inst, name, metadata);
    }
    compiler_barrier();
}

void emit_counter_event(const char* name, uint64_t count) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_counter_event(&g_lop_inst, name, count);
    }
    compiler_barrier();
}

void emit_flow_start_event(const char* name, uint64_t flow_id) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_flow_start_event(&g_lop_inst, name, flow_id);
    }
    compiler_barrier();
}

void emit_flow_finish_event(const char* name, uint64_t flow_id) {
    compiler_barrier();
    if (g_lop_inst.enabled) {
        _asm_emit_flow_finish_event(&g_lop_inst, name, flow_id);
    }
    compiler_barrier();
}
 
}; // namespace LOP
