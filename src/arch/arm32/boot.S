.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    cpsid i
    ldr sp, =boot_stack_top

    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
1:
    cmp r0, r1
    bhs 2f
    str r2, [r0], #4
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
