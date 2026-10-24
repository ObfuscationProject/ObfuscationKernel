.section .text
.global _start, ok_peer_syscall4
#if defined(OK_ARCH_TARGET_X86_64)
_start:
    mov (%rsp), %rdi
    and $-16, %rsp
    call ok_peer_main
ok_peer_syscall4:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    syscall
    ret
#elif defined(OK_ARCH_TARGET_I386)
_start:
    mov (%esp), %eax
    and $-16, %esp
    sub $12, %esp
    push %eax
    call ok_peer_main
ok_peer_syscall4:
    push %ebx
    push %esi
    mov 12(%esp), %eax
    mov 16(%esp), %ebx
    mov 20(%esp), %ecx
    mov 24(%esp), %edx
    mov 28(%esp), %esi
    int $0x80
    pop %esi
    pop %ebx
    ret
#elif defined(OK_ARCH_TARGET_AARCH64)
_start:
    ldr x0, [sp]
    bl ok_peer_main
ok_peer_syscall4:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    svc #0
    ret
#elif defined(OK_ARCH_TARGET_ARM32)
.arm
_start:
    ldr r0, [sp]
    bl ok_peer_main
ok_peer_syscall4:
    push {r7, lr}
    mov r7, r0
    mov r0, r1
    mov r1, r2
    mov r2, r3
    ldr r3, [sp, #8]
    svc #0
    pop {r7, pc}
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
.option norelax
_start:
    la gp, __global_pointer$
#if defined(OK_ARCH_TARGET_RV32)
    lw a0, 0(sp)
#else
    ld a0, 0(sp)
#endif
    call ok_peer_main
ok_peer_syscall4:
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    mv a3, a4
    ecall
    ret
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
_start:
    ld.d $a0, $sp, 0
    bl ok_peer_main
ok_peer_syscall4:
    move $a7, $a0
    move $a0, $a1
    move $a1, $a2
    move $a2, $a3
    move $a3, $a4
    syscall 0
    jr $ra
#elif defined(OK_ARCH_TARGET_MIPS) || defined(OK_ARCH_TARGET_MIPS64)
.set noreorder
_start:
#if defined(OK_ARCH_TARGET_MIPS64)
    ld $4, 0($sp)
#else
    lw $4, 0($sp)
#endif
    jal ok_peer_main
    nop
ok_peer_syscall4:
    move $2, $4
    move $4, $5
    move $5, $6
    move $6, $7
#if defined(OK_ARCH_TARGET_MIPS64)
    move $7, $8
#else
    lw $7, 16($sp)
#endif
    syscall
    beqz $7, 1f
    nop
#if defined(OK_ARCH_TARGET_MIPS64)
    dsubu $2, $0, $2
#else
    subu $2, $0, $2
#endif
1:  jr $ra
    nop
#elif defined(OK_ARCH_TARGET_PPC)
_start:
    lwz 3, 0(1)
    stwu 1, -16(1)
    bl ok_peer_main
ok_peer_syscall4:
    mr 0, 3
    mr 3, 4
    mr 4, 5
    mr 5, 6
    mr 6, 7
    sc
    bns 1f
    neg 3, 3
1:  blr
#endif
#if defined(OK_ARCH_TARGET_ARM32)
.section .note.GNU-stack,"",%progbits
#else
.section .note.GNU-stack,"",@progbits
#endif
