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