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

// You can set this to "true" to enable what is called "safer" mode.
// This mode will attempt to check for buffer exhaustion in the assembly and will try to
// recover from such situation by disabling the profiler, doing necessary operations, and re-enabling it.
// This mode is highly experimental and might not be tested enough.
// Side effects:
// - double memory usage due to double buffering used in recovery process
// - during recovery process some events will be lost
// - tracing overhead might be increased by around 1 nanosecond / event
// - the asynchronous flushing of events to disk might create many threads if you continue to
//   emit events faster than they are flushed for long time
// To enable, set this to true, and also find macros with same name
// in the profiler_asm.cpp (Linux) or profiler_asm.asm (Windows) and also set them to true or 1.
// You will find appropriate comment near them in their respective files.
#define LOP_SAFER false

// This is additional variation of mode explained above. When you enable "safer" mode above, on
// top of that you can also enable lossless mode which will not loose events during the recovery.
// As above, it requires support both in cpp and asm files so change both.
// Side effects:
// - much lower performance (like 16ns/event), because we are not stopping the profiler in that
//   case, we need to do interlocked increments to the event buffers (due to hot swap done).
#define LOP_SAFER_LOSSLESS false

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
