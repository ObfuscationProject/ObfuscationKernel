.syntax unified
.arch armv7-a
.fpu neon
.arm
.section .text
.align 2
.global ok_arm32_enter_native
.type ok_arm32_enter_native,%function
ok_arm32_enter_native:
    mov r12, sp
    cmp r3, #0
    movne sp, r3
    sub sp, sp, #224
    str r12, [sp, #208]
    stmia sp, {r4-r11}
    str lr, [sp, #32]
    mrs r3, cpsr
    str r3, [sp, #36]
    cpsid if
    str r0, [sp, #124]
    str r1, [sp, #132]
    str r2, [sp, #128]
    ldr r3, =ok_arm32_kernel_sp
    str sp, [r3]
    mrc p15, 0, r3, c1, c0, 0
    str r3, [sp, #40]
    mrc p15, 0, r3, c2, c0, 0
    str r3, [sp, #44]
    mrc p15, 0, r3, c2, c0, 1
    str r3, [sp, #48]
    mrc p15, 0, r3, c2, c0, 2
    str r3, [sp, #52]
    mrc p15, 0, r3, c3, c0, 0
    str r3, [sp, #56]
    mrc p15, 0, r3, c12, c0, 0
    str r3, [sp, #60]
    mrc p15, 0, r3, c1, c0, 2
    str r3, [sp, #64]
    mrc p15, 0, r3, c13, c0, 2
    str r3, [sp, #76]
    mrc p15, 0, r3, c13, c0, 3
    str r3, [sp, #80]
    mrs r3, spsr
    str r3, [sp, #84]
    mov r4, sp
    cps #18
    str sp, [r4, #88]
    str lr, [r4, #92]
    mrs r3, spsr
    str r3, [r4, #96]
    mov sp, r4
    cps #0x13
    cps #23
    str sp, [r4, #100]
    str lr, [r4, #104]
    mrs r3, spsr
    str r3, [r4, #108]
    mov sp, r4
    cps #0x13
    cps #27
    str sp, [r4, #112]
    str lr, [r4, #116]
    mrs r3, spsr
    str r3, [r4, #120]
    mov sp, r4
    cps #0x13
    ldr r3, [sp, #64]
    orr r3, r3, #0x00f00000
    mcr p15, 0, r3, c1, c0, 2
    isb
    vmrs r3, fpexc
    str r3, [sp, #68]
    orr r3, r3, #0x40000000
    bic r3, r3, #0x80000000
    vmsr fpexc, r3
    vmrs r3, fpscr
    str r3, [sp, #72]
    add r3, sp, #136
    vstmia r3!, {d8-d15}
    ldr r3, =ok_arm32_native_vectors
    mcr p15, 0, r3, c12, c0, 0
    mov r3, #0
    mcr p15, 0, r3, c2, c0, 2
    mcr p15, 0, r3, c2, c0, 1
    ldr r3, [sp, #132]
    mcr p15, 0, r3, c2, c0, 0
    mov r3, #1
    mcr p15, 0, r3, c3, c0, 0
    mov r3, #0
    dsb sy
    mcr p15, 0, r3, c8, c7, 0
    mcr p15, 0, r3, c7, c5, 0
    mcr p15, 0, r3, c7, c5, 6
    ldr r3, [sp, #40]
    ldr r12, =0x32003306
    bic r3, r3, r12
    orr r3, r3, #1
    orr r3, r3, #0x800000
    mcr p15, 0, r3, c1, c0, 0
    isb
    ldr r4, [sp, #124]
    ldr r3, [r4, #140]
    mcr p15, 0, r3, c13, c0, 2
    ldr r3, [r4, #144]
    mcr p15, 0, r3, c13, c0, 3
    ldr r3, [r4, #420] // Checked canonical user FPEXC, independent of kernel state.
    vmsr fpexc, r3
    add r3, r4, #160
    vldmia r3!, {d0-d15}
    vldmia r3!, {d16-d31}
    ldr r3, [r4, #416]
    vmsr fpscr, r3
#ifdef OK_ARM32_FP_CAPTURE_TEST_POISON
    // Standalone capture-sensitivity fixture only. Hardware already holds the
    // admitted value; a real trap must overwrite this poisoned saved slot.
    mvn r3, #0
    str r3, [r4, #420]
#endif
    add r3, r4, #52
    ldmia r3, {sp, lr}^
    nop
    ldr r3, [r4, #136]
    ldr r12, =0xfe0ffc20 // Preserve user condition flags and Thumb IT state.
    and r3, r3, r12
    orr r3, r3, #0x10
    msr spsr_cxsf, r3
    ldr lr, [r4, #128]
    ldmia r4, {r0-r12}
    movs pc, lr
.size ok_arm32_enter_native, .-ok_arm32_enter_native
.balign 32
ok_arm32_native_vectors:
    b ok_arm32_kernel_fault
    b ok_arm32_undefined
    b ok_arm32_svc
    b ok_arm32_prefetch
    b ok_arm32_data_abort
    b ok_arm32_kernel_fault
    b ok_arm32_irq
    b ok_arm32_kernel_fault
ok_arm32_undefined:
    stmdb sp!, {r0-r12, lr}
    mov r0, #3
    mov r1, #1
    mov r2, #4
    mrs r3, spsr
    tst r3, #0x20
    movne r2, #2
    b ok_arm32_capture
ok_arm32_svc:
    stmdb sp!, {r0-r12, lr}
    mov r0, #0
    mov r1, #2
    mov r2, #0
    b ok_arm32_capture
ok_arm32_prefetch:
    stmdb sp!, {r0-r12, lr}
    mov r0, #2
    mov r1, #3
    mov r2, #4
    b ok_arm32_capture
ok_arm32_data_abort:
    stmdb sp!, {r0-r12, lr}
    mov r0, #2
    mov r1, #4
    mov r2, #8
    b ok_arm32_capture
ok_arm32_irq:
    stmdb sp!, {r0-r12, lr}
    mov r0, #1
    mov r1, #6
    mov r2, #4
    b ok_arm32_capture
ok_arm32_capture:
    mrs r3, spsr
    and r4, r3, #31
    cmp r4, #0x10
    bne ok_arm32_kernel_fault
    ldr r12, =ok_arm32_kernel_sp
    ldr r12, [r12]
    ldr r4, [r12, #124]
    str r3, [r4, #136]
    ldr r5, [sp, #0]
    str r5, [r4, #0]
    ldr r5, [sp, #4]
    str r5, [r4, #4]
    ldr r5, [sp, #8]
    str r5, [r4, #8]
    ldr r5, [sp, #12]
    str r5, [r4, #12]
    ldr r5, [sp, #16]
    str r5, [r4, #16]
    ldr r5, [sp, #20]
    str r5, [r4, #20]
    ldr r5, [sp, #24]
    str r5, [r4, #24]
    ldr r5, [sp, #28]
    str r5, [r4, #28]
    ldr r5, [sp, #32]
    str r5, [r4, #32]
    ldr r5, [sp, #36]
    str r5, [r4, #36]
    ldr r5, [sp, #40]
    str r5, [r4, #40]
    ldr r5, [sp, #44]
    str r5, [r4, #44]
    ldr r5, [sp, #48]
    str r5, [r4, #48]
    add r5, r4, #52
    stmia r5, {sp, lr}^
    nop
    ldr r5, [r4, #52]
    str r5, [r4, #132]
    ldr r5, [sp, #52]
    sub r5, r5, r2
    str r5, [r4, #128]
    str r5, [r4, #60]
    mrc p15, 0, r5, c13, c0, 2
    str r5, [r4, #140]
    mrc p15, 0, r5, c13, c0, 3
    str r5, [r4, #144]
    vmrs r5, fpexc
    str r5, [r4, #420] // Capture actual user FPEXC before any kernel normalization.
    mov r5, #0x40000000
    vmsr fpexc, r5
    add r5, r4, #160
    vstmia r5!, {d0-d15}
    vstmia r5!, {d16-d31}
    vmrs r5, fpscr
    str r5, [r4, #416]
    ldr r7, [r12, #128]
    str sp, [r7, #16]
    strb r0, [r7]
    str r1, [r7, #4]
    mov r5, #0
    str r5, [r7, #8]
    str r5, [r7, #12]
    cmp r0, #2
    bne 1f
    cmp r1, #4
    mrceq p15, 0, r5, c5, c0, 0
    mrceq p15, 0, r6, c6, c0, 0
    mrcne p15, 0, r5, c5, c0, 1
    mrcne p15, 0, r6, c6, c0, 2
    str r5, [r7, #8]
    str r6, [r7, #12]
1:
    cmp r0, #1
    bne 2f
    ldr r5, =0x08010000
    ldr r6, [r5, #12]
    ubfx r1, r6, #0, #10
    str r1, [r7, #4]
    // The timer PPI completes here; any other ID is a device line
    // (TrapKind::interrupt) whose top half masks and completes it in C.
    cmp r1, #30
    movne r0, #4
    strb r0, [r7]
    mov r0, #0
    mcr p15, 0, r0, c14, c2, 1
    streq r6, [r5, #16]
    dsb sy
2:
    cpsid if
    cps #0x13
    mov sp, r12
    b ok_arm32_return
ok_arm32_return:
    ldr r3, [sp, #40]
    dsb sy
    mcr p15, 0, r3, c1, c0, 0
    isb
    ldr r3, [sp, #44]
    mcr p15, 0, r3, c2, c0, 0
    ldr r3, [sp, #48]
    mcr p15, 0, r3, c2, c0, 1
    ldr r3, [sp, #52]
    mcr p15, 0, r3, c2, c0, 2
    ldr r3, [sp, #56]
    mcr p15, 0, r3, c3, c0, 0
    ldr r3, [sp, #60]
    mcr p15, 0, r3, c12, c0, 0
    ldr r3, [sp, #76]
    mcr p15, 0, r3, c13, c0, 2
    ldr r3, [sp, #80]
    mcr p15, 0, r3, c13, c0, 3
    mov r3, #0
    mcr p15, 0, r3, c8, c7, 0
    dsb sy
    isb
    mov r4, sp
    cps #18
    ldr sp, [r4, #88]
    ldr lr, [r4, #92]
    ldr r3, [r4, #96]
    msr spsr_cxsf, r3
    cps #0x13
    cps #23
    ldr sp, [r4, #100]
    ldr lr, [r4, #104]
    ldr r3, [r4, #108]
    msr spsr_cxsf, r3
    cps #0x13
    cps #27
    ldr sp, [r4, #112]
    ldr lr, [r4, #116]
    ldr r3, [r4, #120]
    msr spsr_cxsf, r3
    cps #0x13
    ldr r3, [sp, #84]
    msr spsr_cxsf, r3
    add r3, sp, #136
    vldmia r3!, {d8-d15}
    ldr r3, [sp, #72]
    vmsr fpscr, r3
    ldr r3, [sp, #68]
    vmsr fpexc, r3
    ldr r3, [sp, #64]
    mcr p15, 0, r3, c1, c0, 2
    isb
    ldr lr, [sp, #32]
    ldr r3, [sp, #36]
    ldmia sp, {r4-r11}
    msr cpsr_cxsf, r3
    ldr sp, [sp, #208]
    ldr r12, =ok_arm32_kernel_sp
    mov r3, #0
    str r3, [r12]
    bx lr
ok_arm32_kernel_fault:
    cpsid if
    ldr r0, =0x09000000
    ldr r1, =ok_arm32_fault_text
3:
    ldrb r2, [r1], #1
    cmp r2, #0
    beq 4f
    str r2, [r0]
    b 3b
4:
    wfe
    b 4b
.ltorg
.section .rodata
ok_arm32_fault_text:
    .asciz "ARM32_NATIVE_KERNEL_FAULT\n"
.section .bss
.align 3
ok_arm32_kernel_sp:
    .space 4
.align 3
ok_arm32_trap_stack:
    .space 4096
ok_arm32_trap_stack_top:
.section .note.GNU-stack,"",%progbits
