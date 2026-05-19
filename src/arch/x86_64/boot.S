.section .multiboot, "a"
.align 4
multiboot_header:
    .long 0x1badb002
    .long 0x00010003
    .long -(0x1badb002 + 0x00010003)
    .long multiboot_header
    .long 0x00100000
    .long _kernel_file_end
    .long _kernel_end
    .long _start

.section .text.boot, "ax"
.code32
.global _start
.type _start, @function
_start:
    cli
    mov $boot_stack_top, %esp

    mov $boot_pd, %edi
    xor %eax, %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    orl $0x83, (%edi)
    movl $0, 4(%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b

    mov $boot_pdpt, %eax
    or $0x03, %eax
    mov %eax, boot_pml4

    mov $boot_pd, %eax
    or $0x03, %eax
    mov %eax, boot_pdpt

    mov %cr4, %eax
    or $0x620, %eax
    mov %eax, %cr4

    mov %cr0, %eax
    and $0xfffffffb, %eax
    or $0x00000002, %eax
    mov %eax, %cr0

    mov $boot_pml4, %eax
    mov %eax, %cr3

    mov $0xc0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    lgdt boot_gdt_descriptor

    mov %cr0, %eax
    or $0x80000001, %eax
    mov %eax, %cr0

    ljmp $0x08, $long_mode_entry

.code64
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $boot_stack_top, %rsp
    xor %rbp, %rbp

    mov $_bss_start, %rdi
    mov $_bss_end, %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    rep stosb

    call kernel_main
2:
    hlt
    jmp 2b

.section .rodata.boot, "a"
.align 8
boot_gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00af92000000ffff
boot_gdt_descriptor:
    .word boot_gdt_descriptor - boot_gdt - 1
    .quad boot_gdt

.section .bootstrap_bss, "aw", @nobits
.align 4096
boot_pml4:
    .skip 4096
boot_pdpt:
    .skip 4096
boot_pd:
    .skip 4096
.align 16
boot_stack:
    .skip 1048576
boot_stack_top:
