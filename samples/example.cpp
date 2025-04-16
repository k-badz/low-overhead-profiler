#include "profiler.h"

#include <stdint.h>
#include <thread>
#include <chrono>

int main() {
    LOP::profiler_enable();

    LOP::emit_begin_event("test part A");

    LOP::emit_counter_event("some_resource", 1);

    LOP::emit_begin_event("main thread is starting thread 1");
    std::thread t1([]() {
        LOP::emit_begin_event("thread1 sleeping");
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
        LOP::emit_end_event("thread1 sleeping");
        LOP::emit_counter_event("some_resource", 2);
        });
    LOP::emit_end_event("main thread is starting thread 1");

    LOP::emit_counter_event("some_resource", 3);
    LOP::emit_begin_event("main thread sleeping");
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
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

    LOP::emit_end_event("test part C");
    LOP::profiler_disable();
    LOP::profiler_flush();
    return 0;
}