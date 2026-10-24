.section .rodata.native_user,"a"
.code64
.global ok_native_user_begin, ok_native_user_end
.global ok_native_user_kernel_read, ok_native_user_readonly_write
.global ok_native_user_nx, ok_native_user_unmapped, ok_native_user_invalid
.global ok_native_user_dense
ok_native_user_begin:
    movabs $0x1122334455667788, %rbx
    movabs $0x8877665544332211, %r12
    mov $0x5a, %eax
    movd %eax, %xmm0
    xor %edi, %edi
    mov %cs, %di
    mov $42, %esi
    mov $0x1234, %eax
    syscall
    movabs $0x40002000, %rdx
    mov %rax, (%rdx)
    movdqu %xmm0, 16(%rdx)
    mov $0x2345, %eax
    syscall
1:
    jmp 1b
ok_native_user_kernel_read:
    movabs $0x00100000, %rax
    mov (%rax), %rbx
    ud2
ok_native_user_readonly_write:
    movabs $0x40000000, %rax
    movb $1, (%rax)
    ud2
ok_native_user_nx:
    movabs $0x40002000, %rax
    jmp *%rax
ok_native_user_unmapped:
    movabs $0x60000000, %rax
    mov (%rax), %rbx
    ud2
ok_native_user_invalid:
    ud2
ok_native_user_dense:
    movabs $0x41000000, %rdi
    mov $192, %ecx
1:
    mov %cl, (%rdi)
    add $4096, %rdi
    dec %ecx
    jnz 1b
    mov $0x3456, %eax
    syscall
2:
    jmp 2b
ok_native_user_end:
.section .note.GNU-stack,"",@progbits
