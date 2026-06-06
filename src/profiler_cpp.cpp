/**
 * Copyright (c) 2025 Krzysztof Badziak
 * Copyright (c) 2021-2024 Intel Corporation
 *
 * Licensed under the MIT License (modifications) / Apache License 2.0 (original).
 * See profiler.h for the full notice.
 */

// Pure-C++ implementation of the profiler hot path - a drop-in alternative backend to the
// hand-written assembly in profiler_asm.cpp (Linux) / profiler_asm.asm (Windows). It exports the
// exact same extern "C" symbols (_asm_emit_*, _asm_fast_rdtsc, _asm_get_tid), so a build selects a
// backend simply by compiling THIS file INSTEAD OF the asm one (never both - the symbols collide):
//
//   asm backend (default): g++ ... src/profiler_asm.cpp src/profiler.cpp ...
//   c++ backend:           g++ ... src/profiler_cpp.cpp  src/profiler.cpp ...
//
// The point is to compare per-event tracing overhead (see test/run_bench.sh): if the compiler can
// match the assembly, the asm backends (and their 3-place macro sync) could later be retired.
//
// This is a faithful port of the assembly: same per-thread TLS resolution (raw segment tid ->
// custom_tls[] array index), same single-rdtsc-with-fixed-offsets slot writes, same bounds-check +
// exhaustion handoff. The only platform-specific code is the one-instruction segment read in
// raw_tid(); everything else is shared between Windows and Linux.

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <intrin.h>        // __rdtsc, __readgsqword, _mm_lfence
#else
#  include <x86intrin.h>     // __rdtsc, _mm_lfence
#endif

// Keep these in sync with profiler.h / profiler.cpp and the asm backends. (The eventual
// unification that drops the asm would also remove this duplication.)
#define LOP_DOUBLE_BUFFER 1
#define LOP_BUFFER_SIZE 0x400000U

// Layout MUST match the definitions in profiler.cpp byte-for-byte: the engine creates and flushes
// these objects; this backend only fills slots in them. (Same copies the asm backend keeps.)
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
    Event* next_event; // must be first field (matched by the engine + asm)
    Event* events;
    Event* events_backup;
    uint64_t thread_id;
};

struct CustomTLS {
    EventBuffer event_buffer;
};

struct ProfilerEngine; // opaque here; its first field is custom_tls (CustomTLS**), like the asm assumes

// Trampolines defined in profiler.cpp (C linkage).
extern "C" CustomTLS* allocate_custom_tls();
extern "C" void exhaustion_handler(EventBuffer*);

// --- TLS resolution: faithful port of the asm MacroTLSCheck -----------------------------------

static inline uint64_t raw_tid() {
#if defined(_WIN32) || defined(_WIN64)
    return __readgsqword(0x48);                 // Windows TEB self/thread id slot
#else
    uint64_t t;
    __asm__ __volatile__("movq %%fs:0x10, %0" : "=r"(t)); // glibc TLS thread descriptor
    return t;
#endif
}

static inline uint64_t tls_index(uint64_t tid) {
#if defined(_WIN32) || defined(_WIN64)
    return tid & 0xFFFF;                         // Windows: mask only (mirrors the MASM prologue)
#else
    return (tid >> 12) & 0xFFFF;                 // Linux: shift then mask (mirrors the GCC prologue)
#endif
}

static inline EventBuffer* tls_buffer(ProfilerEngine* engine) {
    // custom_tls is the engine's first field; the asm does the same via "movq (%rdi), %rdi".
    CustomTLS** custom_tls = *reinterpret_cast<CustomTLS***>(engine);
    uint64_t i = tls_index(raw_tid());
    CustomTLS* tls = custom_tls[i];
    if (!tls) {                                  // first event on this thread (or slot collision)
        tls = allocate_custom_tls();            // news + engine-registers an EventBuffer
        custom_tls[i] = tls;                    // the asm stores the slot itself too
    }
    return &tls->event_buffer;
}

// Resolve TLS and (when double buffering) hand off to the exhaustion handler if fewer than the
// largest event's 3 slots remain. After the handler swaps in a fresh buffer, next_event/events are
// updated in place, so the caller re-reads next_event - exactly like the asm retry.
static inline EventBuffer* prep(ProfilerEngine* engine) {
    EventBuffer* b = tls_buffer(engine);
#if LOP_DOUBLE_BUFFER
    if (static_cast<uint64_t>(b->next_event - b->events) >= (LOP_BUFFER_SIZE - 3))
        exhaustion_handler(b);
#endif
    return b;
}

// --- rdtsc / tid helpers (same symbols the engine uses for calibration + tid labelling) --------

extern "C" uint64_t _asm_fast_rdtsc() { return __rdtsc(); }

extern "C" uint64_t _asm_barriered_rdtsc() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

extern "C" uint64_t _asm_get_tid() { return raw_tid(); }

// --- Event emitters: one slot ------------------------------------------------------------------
// Fields written first, timestamp last, so the captured time is as late as possible (as in asm).

extern "C" void _asm_emit_begin_event(ProfilerEngine* engine, const char* name) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 1;
    e->name = name; e->type = CALL_BEGIN; e->timestamp = __rdtsc();
}

extern "C" void _asm_emit_end_event(ProfilerEngine* engine, const char* name) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 1;
    e->name = name; e->type = CALL_END; e->timestamp = __rdtsc();
}

extern "C" void _asm_emit_begin_meta_event(ProfilerEngine* engine, const char* name, uint64_t metadata) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 1;
    e->name = name; e->type = CALL_BEGIN_META; e->metadata = metadata; e->timestamp = __rdtsc();
}

extern "C" void _asm_emit_end_meta_event(ProfilerEngine* engine, const char* name, uint64_t metadata) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 1;
    e->name = name; e->type = CALL_END_META; e->metadata = metadata; e->timestamp = __rdtsc();
}

extern "C" void _asm_emit_counter_event(ProfilerEngine* engine, const char* name, uint64_t count) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 1;
    e->name = name; e->type = COUNTER_INT; e->metadata = count; e->timestamp = __rdtsc();
}

// --- Event emitters: two slots (one rdtsc, second slot timestamp nudged) -----------------------

extern "C" void _asm_emit_endbegin_event(ProfilerEngine* engine, const char* end_name, const char* begin_name) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 2;
    e[0].name = end_name;   e[0].type = CALL_END;
    e[1].name = begin_name; e[1].type = CALL_BEGIN;
    uint64_t t = __rdtsc();
    e[0].timestamp = t;
    e[1].timestamp = t + 1;
}

extern "C" void _asm_emit_immediate_event(ProfilerEngine* engine, const char* name) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 2;
    e[0].name = name; e[0].type = CALL_END;
    e[1].name = name; e[1].type = CALL_BEGIN;
    uint64_t t = __rdtsc();
    e[0].timestamp = t;
    e[1].timestamp = t + 10;
}

extern "C" void _asm_emit_immediate_meta_event(ProfilerEngine* engine, const char* name, uint64_t metadata) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 2;
    e[0].name = name; e[0].type = CALL_END_META;   e[0].metadata = metadata;
    e[1].name = name; e[1].type = CALL_BEGIN_META; e[1].metadata = metadata;
    uint64_t t = __rdtsc();
    e[0].timestamp = t;
    e[1].timestamp = t + 10;
}

// --- Event emitters: three slots (flow) --------------------------------------------------------
// Middle slot intentionally has no name written (flush hardcodes "flow" for FLOW_* events).

extern "C" void _asm_emit_flow_start_event(ProfilerEngine* engine, const char* name, uint64_t flow_id) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 3;
    e[0].name = name; e[0].type = CALL_BEGIN_META; e[0].metadata = flow_id;
    e[1].type = FLOW_START;    e[1].metadata = flow_id;
    e[2].name = name; e[2].type = CALL_END_META;   e[2].metadata = flow_id;
    uint64_t t = __rdtsc();
    e[0].timestamp = t;
    e[1].timestamp = t + 5;
    e[2].timestamp = t + 10;
}

extern "C" void _asm_emit_flow_finish_event(ProfilerEngine* engine, const char* name, uint64_t flow_id) {
    EventBuffer* b = prep(engine);
    Event* e = b->next_event; b->next_event = e + 3;
    e[0].name = name; e[0].type = CALL_BEGIN_META; e[0].metadata = flow_id;
    e[1].type = FLOW_FINISH;   e[1].metadata = flow_id;
    e[2].name = name; e[2].type = CALL_END_META;   e[2].metadata = flow_id;
    uint64_t t = __rdtsc();
    e[0].timestamp = t;
    e[1].timestamp = t + 5;
    e[2].timestamp = t + 10;
}
