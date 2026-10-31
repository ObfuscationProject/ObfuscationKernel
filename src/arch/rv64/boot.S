.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
    .option push
    .option norelax
    # QEMU -bios none starts every hart here with a0 = mhartid, a1 = DTB.
    # Only hart 0 boots; the others park before touching memory.
    csrr t0, mhartid
    bnez t0, .Lpark
    la sp, boot_stack_top
    la t0, _bss_start
    la t1, _bss_end
1:
    bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:
    # tp is this hart's per-CPU block, or 0 in probes without the backend.
    la t0, boot_cpu_block
    ld tp, 0(t0)
    call ok_boot_capture
    call kernel_main
3:
    wfi
    j 3b

# A parked hart has no stack and takes no trap (mstatus.MIE is clear); its
# MSIP only ends the wait. A start request in its mailbox entry names the
# entry point, stack, tp and logical id. Harts without an entry stay parked.
.Lpark:
    li t1, 8
    csrw mie, t1
    li t1, 16
    bgeu t0, t1, .Lpark_forever
    la t1, ok_rv64_hart_mailbox
    slli t2, t0, 5
    add t1, t1, t2
    li t2, 0x2000000
    slli t3, t0, 2
    add t2, t2, t3
4:
    # Clear MSIP before the check, so a request raised after it wakes the wfi.
    sw zero, 0(t2)
    fence
    ld t3, 0(t1)
    bnez t3, 5f
    wfi
    j 4b
5:
    fence
    ld sp, 8(t1)
    ld tp, 16(t1)
    ld a0, 24(t1)
    jr t3
.Lpark_forever:
    csrw mie, zero
6:
    wfi
    j 6b
    .option pop

# Not BSS: hart 0 clears BSS while parked harts may already read their entry.
.section .data
.align 6
.global ok_rv64_hart_mailbox
ok_rv64_hart_mailbox:
    .zero 16 * 32
.weak ok_rv64_cpu_blocks
boot_cpu_block:
    .quad ok_rv64_cpu_blocks

.section .bootstrap_bss, "aw", @nobits
.align 12
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:
