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

void some_sleeping_function()
{   LOP_PROFILE_FUNC
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

int main()
{
    LOP::profiler_enable();

    LOP::emit_counter_event("some_resource", 0);

    LOP::emit_begin_event("test part A");

    LOP::emit_counter_event("some_resource", 1);

    LOP::emit_begin_event("main thread is starting thread 1");
    std::thread t1([]() {
        LOP::emit_begin_event("thread1 sleeping");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        LOP::emit_end_event("thread1 sleeping");
        LOP::emit_counter_event("some_resource", 2);
        });
    LOP::emit_end_event("main thread is starting thread 1");

    LOP::emit_counter_event("some_resource", 3);
    LOP::emit_begin_event("main thread sleeping");
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    LOP::emit_end_event("main thread sleeping");
    LOP::emit_endbegin_event("test part A", "test part B");

    LOP::emit_flow_start_event("flow ... from thread2 create", 123);
    std::thread t2([]() {
        LOP::emit_flow_finish_event("flow ... to actual thread2 start", 123);
        LOP::emit_counter_event("some_resource", 4);
        });

    LOP::emit_counter_event("some_resource", 5);
    LOP::emit_begin_event("main thread waiting for threads 1 and 2");
    t1.join(); t2.join();
    LOP::emit_end_event("main thread waiting for threads 1 and 2");
    LOP::emit_counter_event("some_resource", 6);

    LOP::emit_endbegin_event("test part B", "test part C");

    volatile uint64_t a = 0;
    for (uint64_t i = 0; i < 1000; i++) {
        LOP::emit_begin_event("loop iteration");
        a++;
        LOP::emit_end_event("loop iteration");
    }

    some_sleeping_function();

    LOP::emit_end_event("test part C");

    LOP::emit_counter_event("some_resource", 0);
    LOP::profiler_disable();
    LOP::profiler_flush();
    return 0;
}