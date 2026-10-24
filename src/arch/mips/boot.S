.set noreorder
.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
    # The freestanding O32 ABI permits paired floating-point registers.
    mfc0 $t0, $12
    li $t1, 0x20000000
    or $t0, $t0, $t1
    mtc0 $t0, $12
    ehb
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
    addiu $sp, $sp, -32
    jal ok_boot_capture
    nop
    jal kernel_main
    nop
3:
    b 3b
    nop

.section .bootstrap_bss, "aw", @nobits
.align 12
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:
