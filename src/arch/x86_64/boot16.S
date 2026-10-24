// Read a fixed 8 MiB payload in 16 KiB BIOS transfers. Each transfer uses a
// low-memory bounce buffer, followed by a protected-mode copy above 1 MiB.
// This avoids overwriting the EBDA, VGA aperture, BIOS ROM or this boot sector.
.code16
.section .text
.global _start
.equ BIOS_CHUNKS, 512
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp
    mov %dl, boot_drive
    in $0x92, %al
    and $0xfe, %al
    or $0x02, %al
    out %al, $0x92

    call e820_collect

read_chunk:
    movw $32, dap_count
    movb boot_drive, %dl
    mov $dap, %si
    mov $0x42, %ah
    int $0x13
    jc disk_error
    cli
    xor %ax, %ax
    mov %ax, %ds
    lgdt gdt_descriptor
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x08, $copy_chunk

real_resume:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp
    addl $32, dap_lba
    decw chunks_left
    jnz read_chunk
    lgdt gdt_descriptor
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x08, $kernel_entry

// Preserve the complete BIOS table below the protected-mode stack/bounce area.
// Header: magic, status (1 complete, 2 failed, 3 full), count, entry size.
// Count preserves validated records even on failure/full; neither is allocatable.
// CF terminates enumeration after at least one valid record per the BIOS API.
e820_collect:
    mov $0x5000, %di
    movl $0x30323845, 0(%di)
    movl $2, 4(%di)
    movl $0, 8(%di)
    movl $24, 12(%di)
    add $16, %di
    xor %ebx, %ebx
    xor %ebp, %ebp
e820_next:
    cmp $128, %bp
    jae e820_full
    movl $1, 20(%di)
    mov $0xe820, %eax
    mov $0x534d4150, %edx
    mov $24, %ecx
    push %ds
    push %es
    pushl %ebp
    pushl %edi
    int $0x15
    popl %edi
    popl %ebp
    pop %es
    pop %ds
    jc e820_end
    cmp $0x534d4150, %eax
    jne e820_return
    cmp $20, %ecx
    je e820_legacy
    cmp $24, %ecx
    jne e820_return
    jmp e820_record
e820_legacy:
    movl $1, 20(%di)
e820_record:
    inc %ebp
    add $24, %di
    test %ebx, %ebx
    jnz e820_next
e820_end:
    test %ebp, %ebp
    jz e820_return
    movl $1, 0x5004
e820_return:
    mov %ebp, 0x5008
    ret
e820_full:
    movl $3, 0x5004
    jmp e820_return

disk_error:
    xor %ax, %ax
    mov %ax, %ds
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
    cli
    hlt
    jmp halt

.align 4
dap:
    .byte 0x10, 0
dap_count:
    .word 32
    .word 0
    .word 0x0800
dap_lba:
    .long 1, 0
next_destination:
    .long 0x00100000
chunks_left:
    .word BIOS_CHUNKS
boot_drive:
    .byte 0
error_message:
    .asciz "okernel disk read failed"

.align 8
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
    .quad 0x00009a000000ffff
gdt_descriptor:
    .word gdt_descriptor - gdt - 1
    .long gdt

.code32
copy_chunk:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7000, %esp
    cld
    mov $0x00008000, %esi
    mov next_destination, %edi
    mov $4096, %ecx
    rep movsl
    mov %edi, next_destination
    ljmp $0x18, $leave_protected

.code16
leave_protected:
    mov %cr0, %eax
    and $0xfffffffe, %eax
    mov %eax, %cr0
    ljmp $0, $real_resume

.code32
kernel_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7000, %esp
    mov $0x30323845, %eax
    mov $0x5000, %ebx
    ljmp $0x08, $0x00101000

// Versioned extent record consumed by the SDK exporter and OS image builder.
// A BIOS image without this record is the earlier fixed 4 MiB format.
.org 0x1f0
    .ascii "OKB2"
    .long BIOS_CHUNKS * 32 * 512
.org 510
    .word 0xaa55
