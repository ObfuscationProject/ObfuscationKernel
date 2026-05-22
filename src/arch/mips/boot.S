.set noreorder
.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
    la $sp, boot_stack_top
    la $t0, _bss_start
    la $t1, _bss_end
1:
    sltu $t2, $t0, $t1
    beqz $t2, 2f
    nop
    sw $zero, 0($t0)
    addiu $t0, $t0, 4
    b 1b
    nop
2:
    jal kernel_main
    nop
3:
    b 3b
    nop

.section .bootstrap_bss, "aw", @nobits
.align 12
boot_stack:
    .skip 1048576
boot_stack_top:
