COMMENT @
 Copyright (c) 2025 Krzysztof Badziak
 Copyright (c) 2021-2024 Intel Corporation

 This file contains code that was originally licensed under Apache License 2.0
 and has been modified. The original code is Copyright (c) 2021-2024 Intel Corporation.

 The original code is licensed under the Apache License, Version 2.0:
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.

 My modifications to this file are licensed under the MIT License:
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
@

PUBLIC _asm_fast_rdtsc
PUBLIC _asm_get_tid

PUBLIC _asm_emit_begin_event
PUBLIC _asm_emit_end_event
PUBLIC _asm_emit_endbegin_event
PUBLIC _asm_emit_immediate_event

PUBLIC _asm_emit_begin_meta_event
PUBLIC _asm_emit_end_meta_event
PUBLIC _asm_emit_counter_event
PUBLIC _asm_emit_immediate_meta_event

PUBLIC _asm_emit_flow_start_event
PUBLIC _asm_emit_flow_finish_event

EXTERN allocate_custom_tls : PROC 
EXTERN exhaustion_handler : PROC 

COMMENT @ To enable "safer" mode, set LOP_SAFER to 1 and make sure
 that LOP_BUFFER_SIZE is equal to the one set up in profiler.cpp
@
LOP_SAFER equ 0
LOP_SAFER_LOSSLESS equ 0
LOP_BUFFER_SIZE equ 0400000h

CALL_BEGIN       equ 0
CALL_END         equ 1
CALL_BEGIN_META  equ 2
CALL_END_META    equ 3
COUNTER_INT      equ 4
FLOW_START       equ 5
FLOW_FINISH      equ 6

Event STRUCT
    timestamp      dq ?
    event_name     dq ?
    metadata       dq ?
    event_type     dd ?
    padding        dd ? ; Instead of this you could just set /Zp16 in MASM settings to match C++ struct packing
Event ENDS

EventBuffer STRUCT
    next_event    dq ?
    events        dq ?
    events_backup dq ?
    thread_id     dq ?
EventBuffer ENDS

MacroTLSCheck MACRO
    mov   r9, gs:[48h] ; tid
    mov  rcx, [rcx]
    and   r9, 0FFFFh  
    lea  rax, [rcx+r9*8]  
    mov  r11, qword ptr [rax]  
    test r11, r11
    jz   _allocate_custom_tls_and_continue
_custom_tls_ready:
ENDM

MacroTLSAllocate MACRO
_allocate_custom_tls_and_continue:
    push r8
    push rax
    push rdx
    sub rsp, 32
    call allocate_custom_tls
    add rsp, 32
    mov r11, rax
    pop rdx
    pop rax
    pop r8
    mov qword ptr [rax], r11
    jmp _custom_tls_ready
ENDM

INTERLOCKED_ADD equ xadd

IF LOP_SAFER

IF LOP_SAFER_LOSSLESS
    INTERLOCKED_ADD equ lock xadd
ENDIF

    MacroExhaustionCheck MACRO
        mov   r9, [r11].EventBuffer.next_event
        sub   r9, [r11].EventBuffer.events
        cmp   r9, LOP_BUFFER_SIZE * SIZEOF Event
        jnb   _handle_fallback
    _fallback_handled:
    ENDM

    MacroExhaustionFallback MACRO
    _handle_fallback:
IF LOP_SAFER_LOSSLESS
        push r11
        push r8
        push rax
        push rdx
ENDIF
        mov rcx, r11
        sub rsp, 40
        call exhaustion_handler
        add rsp, 40
IF LOP_SAFER_LOSSLESS
        pop rdx
        pop rax
        pop r8
        pop r11
        jmp _fallback_handled
ELSE
        ret
ENDIF
    ENDM

ELSE

    MacroExhaustionCheck MACRO
    ENDM

    MacroExhaustionFallback MACRO
    ENDM

ENDIF

.code

OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

ALIGN 16
_asm_barriered_rdtsc PROC
    rdtscp
    shl rdx, 32
    or rax, rdx
    lfence
	ret
_asm_barriered_rdtsc ENDP

ALIGN 16
_asm_fast_rdtsc PROC
    rdtsc
    shl rdx, 32
    or rax, rdx
	ret
_asm_fast_rdtsc ENDP

ALIGN 16
_asm_get_tid PROC
    mov rax, gs:[48h]
	ret
_asm_get_tid ENDP

ALIGN 16
_asm_emit_begin_event PROC ; profiler_instance: QWORD, event_name: QWORD
    MacroTLSCheck
    MacroExhaustionCheck
    
    mov   r9, SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_BEGIN
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_begin_event ENDP

ALIGN 16
_asm_emit_end_event PROC ; profiler_instance: QWORD, event_name: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_END
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_end_event ENDP

ALIGN 16
_asm_emit_endbegin_event PROC ; profiler_instance: QWORD, end_name: QWORD, begin_name: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, 2 * SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    lea  r10, [r9 + SIZEOF Event]
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_END
    mov  [r10].Event.event_name, r8
    mov  [r10].Event.event_type, CALL_BEGIN
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    add  rax, 1
    mov  [r10].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_endbegin_event ENDP

ALIGN 16
_asm_emit_immediate_event PROC ; profiler_instance: QWORD, event_name: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, 2 * SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    lea  r10, [r9 + SIZEOF Event]
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_END
    mov  [r10].Event.event_name, rdx
    mov  [r10].Event.event_type, CALL_BEGIN
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    add  rax, 10
    mov  [r10].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_immediate_event ENDP

ALIGN 16
_asm_emit_begin_meta_event PROC ; profiler_instance: QWORD, event_name: QWORD, metadata: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_BEGIN_META
    mov  [r9].Event.metadata, r8
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_begin_meta_event ENDP

ALIGN 16
_asm_emit_end_meta_event PROC ; profiler_instance: QWORD, event_name: QWORD, metadata: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_END_META
    mov  [r9].Event.metadata, r8
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_end_meta_event ENDP

ALIGN 16
_asm_emit_counter_event PROC ; profiler_instance: QWORD, event_name: QWORD, count: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, COUNTER_INT
    mov  [r9].Event.metadata, r8
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_counter_event ENDP

ALIGN 16
_asm_emit_immediate_meta_event PROC ; profiler_instance: QWORD, event_name: QWORD, metadata: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, 2 * SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    lea  r10, [r9 + SIZEOF Event]
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_END_META
    mov  [r9].Event.metadata, r8
    mov  [r10].Event.event_name, rdx
    mov  [r10].Event.event_type, CALL_BEGIN_META
    mov  [r10].Event.metadata, r8
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    add  rax, 10
    mov  [r10].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_immediate_meta_event ENDP

ALIGN 16
_asm_emit_flow_start_event PROC ; profiler_instance: QWORD, event_name: QWORD, flow_id: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, 3 * SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    lea  r10, [r9 + SIZEOF Event]
    lea  r11, [r9 + 2 * SIZEOF Event]
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_BEGIN_META
    mov  [r9].Event.metadata,   r8
    mov  [r10].Event.event_type, FLOW_START
    mov  [r10].Event.metadata,   r8
    mov  [r11].Event.event_name, rdx
    mov  [r11].Event.event_type, CALL_END_META
    mov  [r11].Event.metadata,   r8
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    add  rax, 5
    mov  [r10].Event.timestamp, rax
    add  rax, 5
    mov  [r11].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_flow_start_event ENDP

ALIGN 16
_asm_emit_flow_finish_event PROC ; profiler_instance: QWORD, event_name: QWORD, flow_id: QWORD
    MacroTLSCheck
    MacroExhaustionCheck

    mov   r9, 3 * SIZEOF Event
    INTERLOCKED_ADD  [r11].EventBuffer.next_event, r9
    lea  r10, [r9 + SIZEOF Event]
    lea  r11, [r9 + 2 * SIZEOF Event]
    mov  [r9].Event.event_name, rdx
    mov  [r9].Event.event_type, CALL_BEGIN_META
    mov  [r9].Event.metadata,   r8
    mov  [r10].Event.event_type, FLOW_FINISH
    mov  [r10].Event.metadata,   r8
    mov  [r11].Event.event_name, rdx
    mov  [r11].Event.event_type, CALL_END_META
    mov  [r11].Event.metadata,   r8
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  [r9].Event.timestamp, rax
    add  rax, 5
    mov  [r10].Event.timestamp, rax
    add  rax, 5
    mov  [r11].Event.timestamp, rax
    ret

    MacroTLSAllocate
    MacroExhaustionFallback
_asm_emit_flow_finish_event ENDP

OPTION PROLOGUE:PrologueDef
OPTION EPILOGUE:EpilogueDef

END