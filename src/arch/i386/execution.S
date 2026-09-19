.section .text
.code32
.set FRAME_PC, 128
.set FRAME_SP, 132
.set FRAME_FLAGS, 136
.set FRAME_FP, 160

.global ok_i386_execution_enter
.type ok_i386_execution_enter, @function
ok_i386_execution_enter:
    pushfl
    cli
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov %esp, ok_i386_execution_resume_esp
    mov 24(%esp), %eax
    mov %eax, ok_i386_execution_active_frame
    fxsave ok_i386_execution_kernel_fp
    fxrstor FRAME_FP(%eax)
    pushl 48(%eax)
    pushl FRAME_SP(%eax)
    pushl FRAME_FLAGS(%eax)
    pushl $0x23
    pushl FRAME_PC(%eax)
    mov 32(%eax), %gs
    mov 36(%eax), %fs
    mov 40(%eax), %es
    mov 44(%eax), %ds
    mov %ss:0(%eax), %edi
    mov %ss:4(%eax), %esi
    mov %ss:8(%eax), %ebp
    mov %ss:16(%eax), %ebx
    mov %ss:20(%eax), %edx
    mov %ss:24(%eax), %ecx
    mov %ss:28(%eax), %eax
    iret
.size ok_i386_execution_enter, . - ok_i386_execution_enter

.macro TRAP_NO_ERROR vector
.global ok_i386_trap_\vector
ok_i386_trap_\vector:
    pushl $0
    pushl $\vector
    jmp ok_i386_trap_common
.endm
.macro TRAP_ERROR vector
.global ok_i386_trap_\vector
ok_i386_trap_\vector:
    pushl $\vector
    jmp ok_i386_trap_common
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
.global ok_i386_syscall_entry
ok_i386_syscall_entry:
    pushl $0
    pushl $128
ok_i386_trap_common:
    cld
    push %ds
    push %es
    push %fs
    push %gs
    pushal
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    testb $3, 60(%esp)
    jz 1f
    mov ok_i386_execution_active_frame, %eax
    fxsave FRAME_FP(%eax)
    fxrstor ok_i386_execution_kernel_fp
    mov ok_i386_execution_kernel_fs, %ax
    mov %ax, %fs
    mov ok_i386_execution_kernel_gs, %ax
    mov %ax, %gs
    mov %esp, %eax
    and $-16, %esp
    sub $12, %esp
    push %eax
    call ok_i386_execution_trap
    mov ok_i386_execution_resume_esp, %esp
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    popfl
    ret
1:
    mov %esp, %ebx
    and $-16, %esp
    sub $512, %esp
    fxsave (%esp)
    sub $12, %esp
    push %ebx
    call ok_i386_execution_kernel_trap
    add $16, %esp
    fxrstor (%esp)
    mov %ebx, %esp
    popal
    pop %gs
    pop %fs
    pop %es
    pop %ds
    add $8, %esp
    iret

.section .rodata
.align 4
.global ok_i386_execution_vectors
ok_i386_execution_vectors:
.irp vector,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .long ok_i386_trap_\vector
.endr
.section .note.GNU-stack,"",@progbits
