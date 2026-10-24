.syntax unified
.arch armv7-a
.fpu neon
.arm
.section .rodata.arm32_native_probe, "a"
.balign 16
.global ok_arm32_probe_begin, ok_arm32_probe_timer, ok_arm32_probe_write_fault
.global ok_arm32_probe_thumb
.global ok_arm32_probe_fp_arm, ok_arm32_probe_fp_thumb
.global ok_arm32_probe_execute_fault
.global ok_arm32_probe_kernel_fault, ok_arm32_probe_float, ok_arm32_probe_memory, ok_arm32_probe_end
ok_arm32_probe_begin:
    movw r4, #0x2345
    movt r4, #1
    movw r9, #0x7890
    movt r9, #6
    mrc p15, 0, r1, c13, c0, 2
    mrc p15, 0, r2, c13, c0, 3
    mov r3, #0x5678
    mcr p15, 0, r3, c13, c0, 2
    mov r0, #42
    mov r7, #65
    svc #0
    add r0, r0, #1
    mov r7, #66
    svc #0
1:  b 1b
ok_arm32_probe_timer:
    add r4, r4, #1
    b ok_arm32_probe_timer
ok_arm32_probe_write_fault:
    mov r1, #0x10000000
    str r0, [r1]
    svc #0
ok_arm32_probe_kernel_fault:
    movw r1, #0x8000
    movt r1, #0x4000
    ldr r0, [r1]
    svc #0
ok_arm32_probe_execute_fault:
    movw r1, #0x1000
    movt r1, #0x1000
    bx r1
ok_arm32_probe_float:
    mov r1, #123
    mov r2, #456
    vmov d8, r1, r2
    mov r1, #789
    mov r2, #1023
    vmov d31, r1, r2
    mov r7, #67
    svc #0
    vmov r0, r1, d8
    vmov r2, r3, d31
    mov r7, #68
    svc #0
ok_arm32_probe_memory:
    movw r1, #0x1000
    movt r1, #0x1000
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    mov r7, #69
    svc #0
    b ok_arm32_probe_memory
.thumb
.balign 2
    it eq
ok_arm32_probe_thumb:
    moveq r0, #91
    movs r7, #70
    svc #0
    b .
.macro fp_roundtrip
    mov r0, #0x11
    mov r1, #0
    vmov d0, r0, r1
    mov r0, #0x22
    vmov d15, r0, r1
    mov r0, #0x33
    vmov d16, r0, r1
    mov r0, #0x44
    vmov d31, r0, r1
    mov r0, #0x00400000
    vmsr fpscr, r0
    mov r7, #71
    svc #0
    vmov r0, r5, d0
    vmov r1, r5, d15
    vmov r2, r5, d16
    vmov r3, r5, d31
    vmrs r4, fpscr
    mov r7, #72
    svc #0
    b .
.endm
.balign 2
ok_arm32_probe_fp_thumb:
    fp_roundtrip
.arm
.balign 4
ok_arm32_probe_fp_arm:
    fp_roundtrip
.balign 4
ok_arm32_probe_end:
.section .note.GNU-stack,"",%progbits
