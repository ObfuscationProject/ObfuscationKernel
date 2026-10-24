.section .rodata.object_isolation_user,"a"
.global ok_object_user_begin, ok_object_user_end, ok_object_user_load
ok_object_user_begin:
#if defined(OK_ARCH_TARGET_X86_64)
    mov $0x40001000, %edi
    mov (%rdi), %rsi
ok_object_user_load:
    mov (%rsi), %eax
    mov %eax, 16(%rdi)
    movl $1, 20(%rdi)
    mov $65, %eax
    syscall
1:  jmp 1b
#elif defined(OK_ARCH_TARGET_I386)
    mov $0x40001000, %edi
    mov (%edi), %esi
ok_object_user_load:
    mov (%esi), %eax
    mov %eax, 16(%edi)
    movl $1, 20(%edi)
    mov $65, %eax
    int $0x80
1:  jmp 1b
#elif defined(OK_ARCH_TARGET_ARM32)
    .arm
    movw r1, #0x1000
    movt r1, #0x1000
    ldr r2, [r1]
ok_object_user_load:
    ldr r0, [r2]
    str r0, [r1, #16]
    mov r0, #1
    str r0, [r1, #20]
    mov r7, #65
    svc #0
1:  b 1b
#elif defined(OK_ARCH_TARGET_AARCH64)
    mov x1, #0x10000000
    add x1, x1, #0x1000
    ldr x2, [x1]
ok_object_user_load:
    ldr w0, [x2]
    str w0, [x1, #16]
    mov w0, #1
    str w0, [x1, #20]
    mov x8, #65
    svc #0
1:  b 1b
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
    .option push
    .option norvc
    .option norelax
    li t0, 0x40001000
#if defined(OK_ARCH_TARGET_RV64)
    ld t1, 0(t0)
#else
    lw t1, 0(t0)
#endif
ok_object_user_load:
    lw t2, 0(t1)
    sw t2, 16(t0)
    li t2, 1
    sw t2, 20(t0)
    li a7, 65
    ecall
1:  j 1b
    .option pop
#elif defined(OK_ARCH_TARGET_PPC)
    lis 4, 0x4000
    ori 4, 4, 0x1000
    lwz 5, 0(4)
ok_object_user_load:
    lwz 3, 0(5)
    stw 3, 16(4)
    li 3, 1
    stw 3, 20(4)
    li 0, 65
    sc
1:  b 1b
#elif defined(OK_ARCH_TARGET_MIPS) || defined(OK_ARCH_TARGET_MIPS64)
    .set noreorder
    lui $8, 0x4000
    ori $8, $8, 0x1000
#if defined(OK_ARCH_TARGET_MIPS64)
    ld $9, 0($8)
#else
    lw $9, 0($8)
#endif
ok_object_user_load:
    lw $10, 0($9)
    sw $10, 16($8)
    addiu $10, $0, 1
    sw $10, 20($8)
    addiu $2, $0, 65
    syscall
1:  b 1b
    nop
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
    li.w $t0, 0x40001000
    ld.d $t1, $t0, 0
ok_object_user_load:
    ld.w $t2, $t1, 0
    st.w $t2, $t0, 16
    addi.w $t2, $zero, 1
    st.w $t2, $t0, 20
    addi.w $a7, $zero, 65
    syscall 0
1:  b 1b
#else
#error Unsupported architecture
#endif
ok_object_user_end:
