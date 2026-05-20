.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
    .option push
    .option norelax
    la sp, boot_stack_top
    la t0, _bss_start
    la t1, _bss_end
1:
    bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:
    call kernel_main
3:
    wfi
    j 3b
    .option pop

.section .bootstrap_bss, "aw", @nobits
.align 12
boot_stack:
    .skip 1048576
boot_stack_top:
