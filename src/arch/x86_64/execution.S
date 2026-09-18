.section .text
.code64

// UserFrame offsets are checked by execution.cpp. The first 15 register slots
// match the push order below. The remaining slots are reserved for other ISAs.
.set FRAME_PC, 256
.set FRAME_SP, 264
.set FRAME_FLAGS, 272
.set FRAME_FP, 304

.macro PUSH_REGISTERS
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro POP_REGISTERS
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
.endm

.global ok_x86_execution_enter
.type ok_x86_execution_enter, @function
ok_x86_execution_enter:
    pushfq
    cli
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, ok_x86_execution_resume_rsp(%rip)
    mov %rdi, ok_x86_execution_active_frame(%rip)
    fxsave64 ok_x86_execution_kernel_fp(%rip)
    fxrstor64 FRAME_FP(%rdi)

    mov $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es
    pushq $0x1b
    pushq FRAME_SP(%rdi)
    pushq FRAME_FLAGS(%rdi)
    pushq $0x23
    pushq FRAME_PC(%rdi)
    mov %rdi, %rax
    mov 0(%rax), %r15
    mov 8(%rax), %r14
    mov 16(%rax), %r13
    mov 24(%rax), %r12
    mov 32(%rax), %r11
    mov 40(%rax), %r10
    mov 48(%rax), %r9
    mov 56(%rax), %r8
    mov 64(%rax), %rbp
    mov 72(%rax), %rdi
    mov 80(%rax), %rsi
    mov 88(%rax), %rdx
    mov 96(%rax), %rcx
    mov 104(%rax), %rbx
    mov 112(%rax), %rax
    iretq
.size ok_x86_execution_enter, . - ok_x86_execution_enter

// SYSCALL does not load TSS.rsp0. Interrupts and DF are masked by IA32_FMASK
// before this entry; no instruction touches the untrusted user stack.
.global ok_x86_syscall_entry
.type ok_x86_syscall_entry, @function
ok_x86_syscall_entry:
    mov %rsp, ok_x86_execution_user_rsp(%rip)
    mov ok_x86_execution_stack_top(%rip), %rsp
    pushq $0x1b
    pushq ok_x86_execution_user_rsp(%rip)
    push %r11
    pushq $0x23
    push %rcx
    pushq $0
    pushq $256
    jmp ok_x86_trap_common
.size ok_x86_syscall_entry, . - ok_x86_syscall_entry

.macro TRAP_NO_ERROR vector
.global ok_x86_trap_\vector
ok_x86_trap_\vector:
    pushq $0
    pushq $\vector
    jmp ok_x86_trap_common
.endm
.macro TRAP_ERROR vector
.global ok_x86_trap_\vector
ok_x86_trap_\vector:
    pushq $\vector
    jmp ok_x86_trap_common
.endm

TRAP_NO_ERROR 0
TRAP_NO_ERROR 1
TRAP_NO_ERROR 2
TRAP_NO_ERROR 3
TRAP_NO_ERROR 4
TRAP_NO_ERROR 5
TRAP_NO_ERROR 6
TRAP_NO_ERROR 7
TRAP_ERROR 8
TRAP_NO_ERROR 9
TRAP_ERROR 10
TRAP_ERROR 11
TRAP_ERROR 12
TRAP_ERROR 13
TRAP_ERROR 14
TRAP_NO_ERROR 15
TRAP_NO_ERROR 16
TRAP_ERROR 17
TRAP_NO_ERROR 18
TRAP_NO_ERROR 19
TRAP_NO_ERROR 20
TRAP_ERROR 21
TRAP_NO_ERROR 22
TRAP_NO_ERROR 23
TRAP_NO_ERROR 24
TRAP_NO_ERROR 25
TRAP_NO_ERROR 26
TRAP_NO_ERROR 27
TRAP_NO_ERROR 28
TRAP_ERROR 29
TRAP_ERROR 30
TRAP_NO_ERROR 31
TRAP_NO_ERROR 32
TRAP_NO_ERROR 33
TRAP_NO_ERROR 34
TRAP_NO_ERROR 35
TRAP_NO_ERROR 36
TRAP_NO_ERROR 37
TRAP_NO_ERROR 38
TRAP_NO_ERROR 39
TRAP_NO_ERROR 40
TRAP_NO_ERROR 41
TRAP_NO_ERROR 42
TRAP_NO_ERROR 43
TRAP_NO_ERROR 44
TRAP_NO_ERROR 45
TRAP_NO_ERROR 46
TRAP_NO_ERROR 47
TRAP_NO_ERROR 255

ok_x86_trap_common:
    cld
    PUSH_REGISTERS
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    testb $3, 144(%rsp)
    jz 1f
    mov ok_x86_execution_active_frame(%rip), %rax
    fxsave64 FRAME_FP(%rax)
    fxrstor64 ok_x86_execution_kernel_fp(%rip)
    mov $0xc0000100, %ecx
    mov ok_x86_execution_kernel_fs(%rip), %rax
    mov %rax, %rdx
    shr $32, %rdx
    wrmsr
    mov $0xc0000101, %ecx
    mov ok_x86_execution_kernel_gs(%rip), %rax
    mov %rax, %rdx
    shr $32, %rdx
    wrmsr
    mov %rsp, %rdi
    and $-16, %rsp
    call ok_x86_execution_trap
    mov ok_x86_execution_resume_rsp(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    popfq
    ret
1:
    // Kernel IRQs can occur outside native execution; acknowledge them without
    // turning a kernel fault into a user-process fault or changing its stack.
    mov %rsp, %rbx
    mov %rsp, %rdi
    and $-16, %rsp
    sub $512, %rsp
    fxsave64 (%rsp)
    call ok_x86_execution_kernel_trap
    fxrstor64 (%rsp)
    mov %rbx, %rsp
    POP_REGISTERS
    add $16, %rsp
    iretq

.section .rodata
.align 8
.global ok_x86_execution_vectors
ok_x86_execution_vectors:
.irp vector,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad ok_x86_trap_\vector
.endr
.section .note.GNU-stack,"",@progbits
