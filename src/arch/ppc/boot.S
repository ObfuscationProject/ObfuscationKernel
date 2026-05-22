.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
    /* Map ppce500 CCSR MMIO: EA 0xe0000000 -> PA 0xfe0000000. */
    lis 6, 0x1001
    mtspr 624, 6
    lis 6, 0xc000
    ori 6, 6, 0x0500
    mtspr 625, 6
    lis 6, 0xe000
    ori 6, 6, 0x000a
    mtspr 626, 6
    lis 6, 0xe000
    ori 6, 6, 0x0015
    mtspr 627, 6
    li 6, 0x000f
    mtspr 944, 6
    tlbwe
    isync

    lis 1, boot_stack_top@ha
    addi 1, 1, boot_stack_top@l

    lis 3, _bss_start@ha
    addi 3, 3, _bss_start@l
    lis 4, _bss_end@ha
    addi 4, 4, _bss_end@l
    li 5, 0
1:
    cmplw 3, 4
    bge 2f
    stw 5, 0(3)
    addi 3, 3, 4
    b 1b
2:
    bl kernel_main
3:
    b 3b

.section .bootstrap_bss, "aw", @nobits
.align 12
boot_stack:
    .skip 1048576
boot_stack_top:
