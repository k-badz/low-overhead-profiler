/*
 * Copyright (c) 2025-2026 Krzysztof Badziak
 *
 * Licensed under the MIT License:
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
// Header-only: this standalone program is the one TU that compiles the profiler engine.
#define LOP_IMPLEMENTATION
#include "profiler.h"

#include <stdint.h>
#include <thread>
#include <chrono>
#include <map>
#include <string>
#include <stdexcept>

uint64_t pack_context_to_meta(std::string context_name, uint32_t flow_id = 0)
{
    const static std::map<std::string, uint16_t> context_to_id_map = {
        {"client", 0},
        {"server", 1},
        {"io", 2},
        {"network", 3}
    };

    auto context_id = context_to_id_map.find(context_name);
    if (context_id == context_to_id_map.end()) {
        throw std::runtime_error("Context name not found");
    }

    return ((uint64_t)77 << 48) | ((uint64_t)context_id->second << 32) | (uint64_t)flow_id;
}

int main()
{
    LOP::profiler_enable();

    LOP::emit_begin_meta_event("start request", pack_context_to_meta("client"));
    LOP::emit_flow_start_event("start thread flow", pack_context_to_meta("client", 123));

    std::thread t1([]() {
        LOP::emit_flow_finish_event("start thread flow", pack_context_to_meta("server", 123));
        LOP::emit_begin_meta_event("processing request", pack_context_to_meta("server"));
        LOP::emit_begin_meta_event("checking db", pack_context_to_meta("io"));
        volatile uint32_t a = 0;
        for (uint32_t i = 0; i < 10000; ++i) {
            ++a;
        }
        LOP::emit_end_meta_event("checking db", pack_context_to_meta("io"));
        LOP::emit_begin_meta_event("sending response", pack_context_to_meta("network"));
        volatile uint32_t b = 0;
        for (uint32_t i = 0; i < 10000; ++i) {
            ++b;
        }
        LOP::emit_end_meta_event("sending response", pack_context_to_meta("network"));
        LOP::emit_end_meta_event("processing request", pack_context_to_meta("server"));
        LOP::emit_flow_start_event("join thread flow", pack_context_to_meta("server", 456));
    });

    LOP::emit_end_meta_event("start request", pack_context_to_meta("client"));
    LOP::emit_begin_meta_event("wait for server", pack_context_to_meta("client"));
    t1.join();
    LOP::emit_flow_finish_event("join thread flow", pack_context_to_meta("client", 456));
    LOP::emit_end_meta_event("wait for server", pack_context_to_meta("client"));
    LOP::emit_begin_meta_event("process response", pack_context_to_meta("client"));
    LOP::emit_begin_meta_event("dumping", pack_context_to_meta("io"));
    LOP::emit_end_meta_event("dumping", pack_context_to_meta("io"));
    LOP::emit_end_meta_event("process response", pack_context_to_meta("client"));

    LOP::profiler_disable();
    LOP::profiler_flush();
    return 0;
}