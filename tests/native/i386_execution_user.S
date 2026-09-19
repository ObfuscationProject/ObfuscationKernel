.section .rodata.native_user,"a"
.code32
.global ok_native_user_begin, ok_native_user_end
.global ok_native_user_kernel_read, ok_native_user_readonly_write
.global ok_native_user_nx, ok_native_user_unmapped, ok_native_user_invalid
ok_native_user_begin:
    mov $0x11223344, %esi
    mov $0x88776655, %ebp
    mov $0x5a, %eax
    movd %eax, %xmm0
    fld1
    movl $0x76543210, %fs:0
    movl $0x13579bdf, %gs:0
    xor %ebx, %ebx
    mov %cs, %bx
    mov $42, %ecx
    mov $0x1234, %eax
    int $0x80
    mov $0x40002000, %edx
    mov %eax, (%edx)
    movdqu %xmm0, 16(%edx)
    fistpl 32(%edx)
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %es
    std
    mov $0x2345, %eax
    int $0x80
1:
    jmp 1b
ok_native_user_kernel_read:
    mov $0x00100000, %eax
    mov (%eax), %ebx
    ud2
ok_native_user_readonly_write:
    mov $0x40000000, %eax
    movb $1, (%eax)
    ud2
ok_native_user_nx:
    mov $0x40002000, %eax
    jmp *%eax
ok_native_user_unmapped:
    mov $0x60000000, %eax
    mov (%eax), %ebx
    ud2
ok_native_user_invalid:
    ud2
.global ok_native_user_segment_stack
ok_native_user_segment_stack:
    mov $0x33, %ax
    mov %ax, %ss
    mov $4032, %esp
    mov $0x4567, %eax
    int $0x80
    pushl $0x76543210
    pop %edx
    mov $0x5678, %eax
    int $0x80
    ud2
ok_native_user_end:
.section .note.GNU-stack,"",@progbits
