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
    xor %ebp, %ebp

    mov $_bss_start, %edi
    mov $_bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    cld
    rep stosb

    push $0
    push $0
    push %ebx
    push %esi
    call ok_boot_capture
    add $16, %esp
    call kernel_main
1:
    hlt
    jmp 1b

.section .bootstrap_bss, "aw", @nobits
.align 16
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:
