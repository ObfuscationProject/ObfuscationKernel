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
    mov %eax, %esi
    mov $boot_stack_top, %esp

    // These NOLOAD tables can lie beyond the BIOS payload's padded extent.
    // Clear every entry before paging; physical RAM need not start at zero.
    cld
    mov $boot_pml4, %edi
    xor %eax, %eax
    mov $4096, %ecx
    rep stosl

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

    // The 2 MiB holding the xAPIC registers (0xfee00000), uncached (PCD|PWT).
    // User spaces copy these supervisor entries, so traps on a user CR3 reach it.
    movl $0xfee0009b, boot_pd_high + 503 * 8
    mov $boot_pd_high, %eax
    or $0x03, %eax
    mov %eax, boot_pdpt + 3 * 8

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

    // In kernel mode GS_BASE holds the running CPU's block; this is CPU 0's.
    // Stand-alone probes linked without the execution backend have none.
    .weak ok_x86_cpu_locals
    movabs $ok_x86_cpu_locals, %rax
    test %rax, %rax
    jz .Lno_cpu_block
    mov %rax, (%rax)
    mov %rax, %rdx
    shr $32, %rdx
    mov $0xc0000101, %ecx
    wrmsr
.Lno_cpu_block:

    mov %esi, %edi
    mov %ebx, %esi
    xor %edx, %edx
    xor %ecx, %ecx
    call ok_boot_capture
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
boot_pd_high:
    .skip 4096
.align 16
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:
