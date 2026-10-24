.section .rodata.runtime_frame_user,"a"
.global ok_frame_user_begin, ok_frame_user_end
ok_frame_user_begin:
#if defined(OK_ARCH_TARGET_X86_64)
    mov $0x40001000, %edi
    movl $0x12345678, (%rdi)
    mov $65, %eax
    syscall
1:  jmp 1b
#elif defined(OK_ARCH_TARGET_I386)
    mov $0x40001000, %edi
    movl $0x12345678, (%edi)
    mov $65, %eax
    int $0x80
1:  jmp 1b
#elif defined(OK_ARCH_TARGET_ARM32)
    .arm
    movw r1, #0x1000
    movt r1, #0x1000
    movw r0, #0x5678
    movt r0, #0x1234
    str r0, [r1]
    mov r7, #65
    svc #0
1:  b 1b
#elif defined(OK_ARCH_TARGET_AARCH64)
    mov x1, #0x10000000
    add x1, x1, #0x1000
    mov w0, #0x5678
    movk w0, #0x1234, lsl #16
    str w0, [x1]
    mov x8, #65
    svc #0
1:  b 1b
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
    .option push
    .option norvc
    .option norelax
    li t0, 0x40001000
    li t1, 0x12345678
    sw t1, 0(t0)
    li a7, 65
    ecall
1:  j 1b
    .option pop
#elif defined(OK_ARCH_TARGET_PPC)
    lis 4, 0x4000
    ori 4, 4, 0x1000
    lis 3, 0x1234
    ori 3, 3, 0x5678
    stw 3, 0(4)
    li 0, 65
    sc
1:  b 1b
#endif
ok_frame_user_end:
