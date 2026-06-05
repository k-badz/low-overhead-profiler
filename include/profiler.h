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

#pragma once

#include <stdint.h>

// Per-thread double buffering. With this enabled (the default), the profiler will never
// crash when a thread fills up its event buffer. Instead, the thread instantly swaps to a
// pre-allocated backup buffer and keeps tracing, while a background thread allocates the
// next backup. The filled buffer is kept in a linked list and all of them are merged into
// a single trace file at flush time, so no events are lost.
// How it works / things to keep in mind:
// - the hot path gains a single (well-predicted) bounds check per event, which is
//   essentially free compared to the rest of the emission.
// - the swap happens on the very thread that depleted the buffer, so it needs no atomics,
//   no global disable, and no draining - it is naturally lossless and cheap.
// - filled buffers are retained in RAM until flush. Each buffer is large
//   (LOP_BUFFER_SIZE * sizeof(Event), ~128 MB with the defaults), so memory grows with the
//   amount of tracing. For long-running sessions, call profiler_flush() periodically.
// Set this to 0 to get the original, truly zero-overhead unsafe mode: no bounds check at
// all, but the buffer can overflow and crash if you trace for too long.
// IMPORTANT: this macro must be kept in sync with the same macro in profiler_asm.cpp
// (Linux) and profiler_asm.asm (Windows).
#define LOP_DOUBLE_BUFFER 1

// Background spill-to-disk. With this enabled (the default), a dedicated low-priority
// background thread continuously dumps filled buffers (the ones double buffering keeps in a
// linked list) to a single temporary file as RAW BINARY (no parsing - just the Event bytes),
// then frees that ~128 MB of RAM. At flush time the trace is assembled from BOTH the spilled
// disk segments and whatever is still in RAM, so nothing is lost. This bounds memory growth on
// long runs without touching the hot path at all.
// Things to keep in mind:
// - the asm hot path is NOT involved, so - unlike LOP_DOUBLE_BUFFER - this macro lives only
//   here and in profiler.cpp; the asm files need no changes and no sync.
// - the spiller stays out of the way: it runs at the lowest OS priority and throttles itself.
//   If it ever can't keep up, filled buffers simply stay in RAM (same as with spilling off) -
//   it is a best-effort memory optimization, never a correctness requirement.
// - if a buffer allocation ever fails (RAM exhausted), the profiler does NOT crash: it boosts
//   the spiller to drain buffers to disk and retries the allocation once RAM is reclaimed.
// - the spill file holds raw Event structs, including the (still-valid) name pointers, so it is
//   only meaningful within the SAME process run - it is not a portable/persistent trace.
// LOP_SPILL_RAM_THRESHOLD is how many filled buffers are kept in RAM before the spiller starts
// writing the excess to disk (so short runs never touch the disk at all).
// Requires LOP_DOUBLE_BUFFER. Set to 0 to keep all filled buffers in RAM until flush.
#define LOP_SPILL_TO_DISK 1
#define LOP_SPILL_RAM_THRESHOLD 2
#if LOP_SPILL_TO_DISK && !LOP_DOUBLE_BUFFER
#  error "LOP_SPILL_TO_DISK requires LOP_DOUBLE_BUFFER (it spills the filled_buffers list)."
#endif

namespace LOP {

// Self-explanatory, I guess.
void profiler_enable();
void profiler_disable();

// You can use suffix to create multiple files in one process session.
void profiler_flush(const char* suffix = nullptr);

// All events require a string that will be used as a name of the event and this is what
// you will see on the trace. The pointer that you supply to the emit functions must be alive
// at the point of profiler_flush() call. The profiler will not copy the string, it will just
// store the pointer, because copying it around would kill the performance. So it is safest
// to just use some static strings as in example.

// Simple events.
void emit_begin_event(const char* name);
void emit_end_event(const char* name);
void emit_immediate_event(const char* name);

// Double event that can be used as fast separator between two profiled regions
// while roughly having overhead of just single event (only one RDTSC call).
void emit_endbegin_event(const char* end_name, const char* begin_name);

// Events allowing putting some additional metadata which will be present in the trace.
// Check context_example.cpp/contextize.py for example usage.
void emit_begin_meta_event(const char* name, uint64_t metadata);
void emit_end_meta_event(const char* name, uint64_t metadata);
void emit_immediate_meta_event(const char* name, uint64_t metadata);
void emit_counter_event(const char* name, uint64_t count);

// Flow events. Good to connect between events managed by different threads
// like monitoring of buffer liveness, async launch latencies, etc etc.
// Important notice - Perfetto UI support only 32bit flow IDs but I'm
// leaving whole 64bits here if you would like to put additional metadata here.
// Check context_example.cpp/contextize.py for example usage.
void emit_flow_start_event(const char* name, uint64_t flow_id);
void emit_flow_finish_event(const char* name, uint64_t flow_id);

// Scoped profiles. Automatically emit begin/end events when entering/leaving scope.
class SimpleScopedProfile {
    const char* name;

public:
    SimpleScopedProfile(const char* name) {
        this->name = name;
        emit_begin_event(this->name);
    }

    ~SimpleScopedProfile() {
        emit_end_event(this->name);
    }
};

class MetaScopedProfile {
    const char* name;

public:
    MetaScopedProfile(const char* name, uint64_t meta) {
        this->name = name;
        emit_begin_meta_event(this->name, meta);
    }

    ~MetaScopedProfile() {
        emit_end_event(this->name);
    }
};

// This macro will create a scoped profile with the name of the function.
#if defined(_WIN32) || defined(_WIN64)
#   define LOP_PROFILE_FUNC LOP::SimpleScopedProfile func_scope_profiler(__FUNCSIG__);
#else
#   define LOP_PROFILE_FUNC LOP::SimpleScopedProfile func_scope_profiler(__PRETTY_FUNCTION__);
#endif

}
