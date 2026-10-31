.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
    .option push
    .option norelax
    # QEMU -bios none starts every hart here; this profile runs on hart 0
    # alone and parks the others for good before they touch memory.
    csrr t0, mhartid
    bnez t0, .Lpark
    la sp, boot_stack_top
    la t0, _bss_start
    la t1, _bss_end
1:
    bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:
    call ok_boot_capture
    call kernel_main
3:
    wfi
    j 3b
.Lpark:
    csrw mie, zero
4:
    wfi
    j 4b
    .option pop

.section .bootstrap_bss, "aw", @nobits
.align 12
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:
