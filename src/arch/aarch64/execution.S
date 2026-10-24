.section .text

.macro SAVE_SIMD base
    stp q0, q1, [\base, #0]
    stp q2, q3, [\base, #32]
    stp q4, q5, [\base, #64]
    stp q6, q7, [\base, #96]
    stp q8, q9, [\base, #128]
    stp q10, q11, [\base, #160]
    stp q12, q13, [\base, #192]
    stp q14, q15, [\base, #224]
    stp q16, q17, [\base, #256]
    stp q18, q19, [\base, #288]
    stp q20, q21, [\base, #320]
    stp q22, q23, [\base, #352]
    stp q24, q25, [\base, #384]
    stp q26, q27, [\base, #416]
    stp q28, q29, [\base, #448]
    stp q30, q31, [\base, #480]
.endm
.macro RESTORE_SIMD base
    ldp q0, q1, [\base, #0]
    ldp q2, q3, [\base, #32]
    ldp q4, q5, [\base, #64]
    ldp q6, q7, [\base, #96]
    ldp q8, q9, [\base, #128]
    ldp q10, q11, [\base, #160]
    ldp q12, q13, [\base, #192]
    ldp q14, q15, [\base, #224]
    ldp q16, q17, [\base, #256]
    ldp q18, q19, [\base, #288]
    ldp q20, q21, [\base, #320]
    ldp q22, q23, [\base, #352]
    ldp q24, q25, [\base, #384]
    ldp q26, q27, [\base, #416]
    ldp q28, q29, [\base, #448]
    ldp q30, q31, [\base, #480]
.endm

.global ok_arm_execution_enter
.type ok_arm_execution_enter, %function
ok_arm_execution_enter:
    sub sp, sp, #128
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mrs x1, tpidr_el0
    mrs x2, tpidrro_el0
    stp x1, x2, [sp, #96]
    mrs x1, fpcr
    mrs x2, fpsr
    stp x1, x2, [sp, #112]
    ldr x1, =ok_arm_execution_resume_sp
    mov x2, sp
    str x2, [x1]
    ldr x1, =ok_arm_execution_active_frame
    str x0, [x1]
    ldr x1, =ok_arm_execution_kernel_fp
    SAVE_SIMD x1

    mov x9, x0
    add x1, x9, #304
    RESTORE_SIMD x1
    ldr x1, [x9, #248]
    lsr x2, x1, #32
    msr fpcr, x1
    msr fpsr, x2
    ldp x1, x2, [x9, #256]
    msr elr_el1, x1
    msr sp_el0, x2
    ldr x1, [x9, #272]
    msr spsr_el1, x1
    ldp x1, x2, [x9, #280]
    msr tpidr_el0, x1
    msr tpidrro_el0, x2
    ldr x1, =ok_arm_execution_stack_top
    ldr x1, [x1]
    mov sp, x1
    ldp x0, x1, [x9, #0]
    ldp x2, x3, [x9, #16]
    ldp x4, x5, [x9, #32]
    ldp x6, x7, [x9, #48]
    ldr x8, [x9, #64]
    ldp x10, x11, [x9, #80]
    ldp x12, x13, [x9, #96]
    ldp x14, x15, [x9, #112]
    ldp x16, x17, [x9, #128]
    ldp x18, x19, [x9, #144]
    ldp x20, x21, [x9, #160]
    ldp x22, x23, [x9, #176]
    ldp x24, x25, [x9, #192]
    ldp x26, x27, [x9, #208]
    ldp x28, x29, [x9, #224]
    ldr x30, [x9, #240]
    ldr x9, [x9, #72]
    eret
.size ok_arm_execution_enter, . - ok_arm_execution_enter

.balign 2048
.global ok_arm_execution_vectors
.type ok_arm_execution_vectors, %function
ok_arm_execution_vectors:
.macro VECTOR number, user=0
    .balign 128
    .if \user
        stp x0, x1, [sp, #-16]!
        mov x0, #\number
        b ok_arm_user_trap
    .else
        mov x0, #\number
        b ok_arm_execution_kernel_fault
    .endif
.endm
    VECTOR 0
    VECTOR 1
    VECTOR 2
    VECTOR 3
    VECTOR 4
    VECTOR 5
    VECTOR 6
    VECTOR 7
    VECTOR 8, 1
    VECTOR 9, 1
    VECTOR 10, 1
    VECTOR 11, 1
    VECTOR 12
    VECTOR 13
    VECTOR 14
    VECTOR 15
.balign 128
.size ok_arm_execution_vectors, . - ok_arm_execution_vectors

ok_arm_user_trap:
    // All register and SIMD state is captured before entering compiled code.
    // SP_EL1 is kernel-owned; an invalid user SP never becomes a kernel stack.
    ldr x1, =ok_arm_execution_active_frame
    ldr x1, [x1]
    stp x2, x3, [x1, #16]
    stp x4, x5, [x1, #32]
    stp x6, x7, [x1, #48]
    stp x8, x9, [x1, #64]
    stp x10, x11, [x1, #80]
    stp x12, x13, [x1, #96]
    stp x14, x15, [x1, #112]
    stp x16, x17, [x1, #128]
    stp x18, x19, [x1, #144]
    stp x20, x21, [x1, #160]
    stp x22, x23, [x1, #176]
    stp x24, x25, [x1, #192]
    stp x26, x27, [x1, #208]
    stp x28, x29, [x1, #224]
    str x30, [x1, #240]
    mov x10, x0
    ldp x2, x3, [sp]
    stp x2, x3, [x1, #0]
    mrs x2, fpcr
    mrs x3, fpsr
    orr x2, x2, x3, lsl #32
    str x2, [x1, #248]
    mrs x2, elr_el1
    mrs x3, sp_el0
    stp x2, x3, [x1, #256]
    mrs x2, spsr_el1
    str x2, [x1, #272]
    mrs x2, tpidr_el0
    mrs x3, tpidrro_el0
    stp x2, x3, [x1, #280]
    add x2, x1, #304
    SAVE_SIMD x2
    ldr x2, =ok_arm_execution_kernel_fp
    RESTORE_SIMD x2
    ldr x1, =ok_arm_execution_resume_sp
    ldr x1, [x1]
    ldp x2, x3, [x1, #96]
    msr tpidr_el0, x2
    msr tpidrro_el0, x3
    ldp x2, x3, [x1, #112]
    msr fpcr, x2
    msr fpsr, x3
    mov x0, x10
    mov x1, sp
    bl ok_arm_execution_trap
    ldr x0, =ok_arm_execution_resume_sp
    ldr x0, [x0]
    mov sp, x0
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #128
    ret

.section .note.GNU-stack,"",%progbits
