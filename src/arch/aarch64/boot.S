.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    b boot_entry
    .long 0
    .quad 0x00080000
    .quad _kernel_image_size
    .quad 0x2
    .quad 0
    .quad 0
    .quad 0
    .long 0x644d5241
    .long 0

boot_entry:
    ldr x0, =boot_stack_top
    mov sp, x0

    ldr x0, =_bss_start
    ldr x1, =_bss_end
    mov x2, xzr
1:
    cmp x0, x1
    b.hs 2f
    str x2, [x0], #8
    b 1b
2:
    bl kernel_main
3:
    wfe
    b 3b

.section .bootstrap_bss, "aw", %nobits
.align 12
boot_stack:
    .skip 1048576
boot_stack_top:
