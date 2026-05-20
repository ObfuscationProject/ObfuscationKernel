.code16
.section .text
.global _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp
    mov %dl, boot_drive

    movw $0x0800, next_segment
    movl $1, next_lba
    mov $16, %di
read_chunk:
    mov next_segment, %bx
    mov %bx, dap_segment
    mov next_lba, %eax
    mov %eax, dap_lba
    movb boot_drive, %dl
    mov $dap, %si
    mov $0x42, %ah
    int $0x13
    jc disk_error
    mov next_segment, %bx
    add $0x0400, %bx
    mov %bx, next_segment
    mov next_lba, %eax
    add $32, %eax
    mov %eax, next_lba
    dec %di
    jnz read_chunk

    in $0x92, %al
    or $0x02, %al
    out %al, $0x92

    lgdt gdt_descriptor
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x08, $protected_entry

disk_error:
    mov $error_message, %si
print_error:
    lodsb
    test %al, %al
    jz halt
    mov $0x0e, %ah
    mov $0x0007, %bx
    int $0x10
    jmp print_error

halt:
    hlt
    jmp halt

.align 4
dap:
    .byte 0x10
    .byte 0
    .word 32
    .word 0
dap_segment:
    .word 0x0800
dap_lba:
    .long 1
    .long 0

boot_drive:
    .byte 0
next_segment:
    .word 0
next_lba:
    .long 0
error_message:
    .asciz "okernel disk read failed"

.align 8
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
gdt_descriptor:
    .word gdt_descriptor - gdt - 1
    .long gdt

.code32
protected_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %esp

    cld
    mov $0x00008000, %esi
    mov $0x00100000, %edi
    mov $65536, %ecx
    rep movsl
    ljmp $0x08, $0x00101000

.org 510
    .word 0xaa55
