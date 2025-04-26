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
 
#include <stdint.h>
#include <unistd.h>
#include <stddef.h>

// To enable "safer" mode, set LOP_SAFER to true and make sure
// that LOP_BUFFER_SIZE is equal to the one set up in profiler.cpp
#define LOP_SAFER false
#define LOP_SAFER_LOSSLESS false
#define LOP_BUFFER_SIZE 0x400000U

struct CustomTLS;
struct ProfilerEngine;

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
    Event* next_event;
    Event* events;
    Event* events_backup;
    uint64_t thread_id;
};

extern CustomTLS* allocate_custom_tls();
extern void exhaustion_handler(EventBuffer*);

#define CONCAT(A, B) A##B
#define TOSTRING(s) _TOSTRING(s)
#define _TOSTRING(s) #s

#define INTERLOCKED_ADD "xaddq"

#if LOP_SAFER

#if LOP_SAFER_LOSSLESS
#define INTERLOCKED_ADD "lock xaddq"

#define MacroExhaustionPreHandler                                                   \
    "push %%r11\n\t"                                                                \
    "push %%rdx\n\t"                                                                \
    "push %%rax\n\t"                                                                \
    "push %%rsi\n\t"

#define MacroExhaustionPostHandler(label_prefix)                                    \
    "pop  %%rsi\n\t"                                                                \
    "pop  %%rax\n\t"                                                                \
    "pop  %%rdx\n\t"                                                                \
    "pop  %%r11\n\t"                                                                \
    "jmp " TOSTRING(CONCAT(label_prefix,_fallback_handled)) "\n\t"

#else // LOP_SAFER_LOSSLESS

#define MacroExhaustionPreHandler ""
#define MacroExhaustionPostHandler(label_prefix)                                    \
    "ret\n\t"

#endif // LOP_SAFER_LOSSLESS

#define MacroExhaustionCheck(label_prefix)                                          \
    "movq %c0(%%r11), %%r9\n\t"                                                     \
    "sub  %c1(%%r11), %%r9\n\t"                                                     \
    "cmp  %2, %%r9\n\t"                                                             \
    "jz " TOSTRING(CONCAT(label_prefix,_handle_fallback)) "\n\t"                    \
TOSTRING(CONCAT(label_prefix,_fallback_handled)) ":\n\t"

#define MacroExhaustionFallback(label_prefix)                                       \
TOSTRING(CONCAT(label_prefix,_handle_fallback)) ":\n\t"                             \
    MacroExhaustionPreHandler                                                       \
    "movq %%r11, %%rdi\n\t"                                                         \
    "sub  $40, %%rsp\n\t"                                                           \
    "call exhaustion_handler\n\t"                                                   \
    "add  $40, %%rsp\n\t"                                                           \
    MacroExhaustionPostHandler(label_prefix)

#else // LOP_SAFER

#define MacroExhaustionCheck(label_prefix) ""
#define MacroExhaustionFallback(label_prefix) ""

#endif // LOP_SAFER

#define MacroTLSCheck(label_prefix)                                                 \
    "movq %%fs:0x10, %%r9\n\t"                                                      \
    "movq (%%rdi), %%rdi\n\t"                                                       \
    "shr  $12, %%r9\n\t"                                                            \
    "and  $0xFFFF, %%r9\n\t"                                                        \
    "lea  (%%rdi, %%r9, 8), %%rax\n\t"                                              \
    "movq (%%rax), %%r11\n\t"                                                       \
    "test %%r11, %%r11\n\t"                                                         \
    "jz " TOSTRING(CONCAT(label_prefix,_allocate_custom_tls_and_continue)) "\n\t"   \
TOSTRING(CONCAT(label_prefix,_custom_tls_ready)) ":\n\t"

#define MacroTLSAllocate(label_prefix)    \
TOSTRING(CONCAT(label_prefix,_allocate_custom_tls_and_continue)) ":\n\t"  \
    "push %%rdx\n\t"                                                      \
    "push %%rax\n\t"                                                      \
    "push %%rsi\n\t"                                                      \
    "sub  $32, %%rsp\n\t"                                                 \
    "call allocate_custom_tls\n\t"                                        \
    "add  $32, %%rsp\n\t"                                                 \
    "mov  %%rax, %%r11\n\t"                                               \
    "pop  %%rsi\n\t"                                                      \
    "pop  %%rax\n\t"                                                      \
    "pop  %%rdx\n\t"                                                      \
    "movq %%r11, (%%rax)\n\t"                                             \
    "jmp " TOSTRING(CONCAT(label_prefix,_custom_tls_ready)) "\n\t"

extern "C" __attribute__((naked)) uint64_t _asm_fast_rdtsc() {
    __asm__ __volatile__(
        "rdtsc\n\t"
        "shl $32, %rdx\n\t"
        "or %rdx, %rax\n\t"
        "ret");
}

extern "C" __attribute__((naked)) uint64_t _asm_get_tid() {
    __asm__ __volatile__(
        "mov %fs:0x10, %rax\n\t"
        "ret");
}

extern "C" __attribute__((naked)) void _asm_emit_begin_event(ProfilerEngine*, const char*) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_begin_event)
        MacroExhaustionCheck(_asm_emit_begin_event)
        "movq %3, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c7(%%r9)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_begin_event)
        MacroExhaustionFallback(_asm_emit_begin_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_BEGIN), "i" (offsetof(Event, type)), "i" (offsetof(Event, timestamp)) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_end_event(ProfilerEngine*, const char*) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_end_event)
        MacroExhaustionCheck(_asm_emit_end_event)
        "movq %3, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c7(%%r9)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_end_event)
        MacroExhaustionFallback(_asm_emit_end_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_END), "i" (offsetof(Event, type)), "i" (offsetof(Event, timestamp)) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_endbegin_event(ProfilerEngine*, const char*, const char*) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_endbegin_event)
        MacroExhaustionCheck(_asm_emit_endbegin_event)
        "movq %3*2, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "lea %c3(%%r9), %%r10\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rdx, %c4(%%r10)\n\t"
        "movq %8, %c6(%%r10)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c7(%%r9)\n\t"
        "addq $1, %%rax\n\t"
        "movq %%rax, %c7(%%r10)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_endbegin_event)
        MacroExhaustionFallback(_asm_emit_endbegin_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_END), "i" (offsetof(Event, type)), "i" (offsetof(Event, timestamp)),
            "i" (CALL_BEGIN) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_immediate_event(ProfilerEngine*, const char*) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_immediate_event)
        MacroExhaustionCheck(_asm_emit_immediate_event)
        "movq %3*2, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "lea %c3(%%r9), %%r10\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rsi, %c4(%%r10)\n\t"
        "movq %8, %c6(%%r10)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c7(%%r9)\n\t"
        "addq $10, %%rax\n\t"
        "movq %%rax, %c7(%%r10)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_immediate_event)
        MacroExhaustionFallback(_asm_emit_immediate_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_END), "i" (offsetof(Event, type)), "i" (offsetof(Event, timestamp)),
            "i" (CALL_BEGIN) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_begin_meta_event(ProfilerEngine*, const char*, uint64_t) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_begin_meta_event)
        MacroExhaustionCheck(_asm_emit_begin_meta_event)
        "movq %3, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rdx, %c7(%%r9)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c8(%%r9)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_begin_meta_event)
        MacroExhaustionFallback(_asm_emit_begin_meta_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_BEGIN_META), "i" (offsetof(Event, type)), "i" (offsetof(Event, metadata)),
            "i" (offsetof(Event, timestamp)) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_end_meta_event(ProfilerEngine*, const char*, uint64_t) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_end_meta_event)
        MacroExhaustionCheck(_asm_emit_end_meta_event)
        "movq %3, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rdx, %c7(%%r9)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c8(%%r9)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_end_meta_event)
        MacroExhaustionFallback(_asm_emit_end_meta_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_END_META), "i" (offsetof(Event, type)), "i" (offsetof(Event, metadata)),
            "i" (offsetof(Event, timestamp)) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_counter_event(ProfilerEngine*, const char*, uint64_t) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_counter_event)
        MacroExhaustionCheck(_asm_emit_counter_event)
        "movq %3, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rdx, %c7(%%r9)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c8(%%r9)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_counter_event)
        MacroExhaustionFallback(_asm_emit_counter_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (COUNTER_INT), "i" (offsetof(Event, type)), "i" (offsetof(Event, metadata)),
            "i" (offsetof(Event, timestamp)) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_immediate_meta_event(ProfilerEngine*, const char*, uint64_t) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_immediate_meta_event)
        MacroExhaustionCheck(_asm_emit_immediate_meta_event)
        "movq %3*2, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "lea %c3(%%r9), %%r10\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rdx, %c9(%%r9)\n\t"
        "movq %%rsi, %c4(%%r10)\n\t"
        "movq %8, %c6(%%r10)\n\t"
        "movq %%rdx, %c9(%%r10)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c7(%%r9)\n\t"
        "addq $10, %%rax\n\t"
        "movq %%rax, %c7(%%r10)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_immediate_meta_event)
        MacroExhaustionFallback(_asm_emit_immediate_meta_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_END_META), "i" (offsetof(Event, type)), "i" (offsetof(Event, timestamp)),
            "i" (CALL_BEGIN_META), "i" (offsetof(Event, metadata)) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_flow_start_event(ProfilerEngine*, const char*, uint64_t) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_flow_start_event)
        MacroExhaustionCheck(_asm_emit_flow_start_event)
        "movq %3*3, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "lea %c3(%%r9), %%r10\n\t"
        "lea %c3*2(%%r9), %%r11\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rdx, %c9(%%r9)\n\t"
        "movq %8, %c6(%%r10)\n\t"
        "movq %%rdx, %c9(%%r10)\n\t"
        "movq %%rsi, %c4(%%r11)\n\t"
        "movq %10, %c6(%%r11)\n\t"
        "movq %%rdx, %c9(%%r11)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c7(%%r9)\n\t"
        "addq $5, %%rax\n\t"
        "movq %%rax, %c7(%%r10)\n\t"
        "addq $5, %%rax\n\t"
        "movq %%rax, %c7(%%r11)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_flow_start_event)
        MacroExhaustionFallback(_asm_emit_flow_start_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_BEGIN_META), "i" (offsetof(Event, type)), "i" (offsetof(Event, timestamp)),
            "i" (FLOW_START), "i" (offsetof(Event, metadata)), "i" (CALL_END_META) :
    );
}

extern "C" __attribute__((naked)) void _asm_emit_flow_finish_event(ProfilerEngine*, const char*, uint64_t) {
    __asm__ __volatile__(
        MacroTLSCheck(_asm_emit_flow_finish_event)
        MacroExhaustionCheck(_asm_emit_flow_finish_event)
        "movq %3*3, %%r9\n\t"
        INTERLOCKED_ADD " %%r9, %c0(%%r11)\n\t"
        "lea %c3(%%r9), %%r10\n\t"
        "lea %c3*2(%%r9), %%r11\n\t"
        "movq %%rsi, %c4(%%r9)\n\t"
        "movq %5, %c6(%%r9)\n\t"
        "movq %%rdx, %c9(%%r9)\n\t"
        "movq %8, %c6(%%r10)\n\t"
        "movq %%rdx, %c9(%%r10)\n\t"
        "movq %%rsi, %c4(%%r11)\n\t"
        "movq %10, %c6(%%r11)\n\t"
        "movq %%rdx, %c9(%%r11)\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %%rax\n\t"
        "movq %%rax, %c7(%%r9)\n\t"
        "addq $5, %%rax\n\t"
        "movq %%rax, %c7(%%r10)\n\t"
        "addq $5, %%rax\n\t"
        "movq %%rax, %c7(%%r11)\n\t"
        "ret\n\t"
        MacroTLSAllocate(_asm_emit_flow_finish_event)
        MacroExhaustionFallback(_asm_emit_flow_finish_event)
        : : "i" (offsetof(EventBuffer, next_event)), "i" (offsetof(EventBuffer, events)), "i" (LOP_BUFFER_SIZE * sizeof(Event)),
            "i" (sizeof(Event)), "i" (offsetof(Event, name)), "i" (CALL_BEGIN_META), "i" (offsetof(Event, type)), "i" (offsetof(Event, timestamp)),
            "i" (FLOW_FINISH), "i" (offsetof(Event, metadata)), "i" (CALL_END_META) :
    );
}
