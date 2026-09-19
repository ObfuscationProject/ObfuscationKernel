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

    /* Keep PCI MMIO in a supervisor-only, cache-inhibited TLB1 slot. */
    lis 6, 0x1002
    mtspr 624, 6
    lis 6, 0xc000
    ori 6, 6, 0x0800
    mtspr 625, 6
    lis 6, 0xc000
    ori 6, 6, 0x000a
    mtspr 626, 6
    lis 6, 0xc000
    ori 6, 6, 0x0005
    mtspr 627, 6
    li 6, 0
    mtspr 944, 6
    tlbwe
    isync

    /* ppce500 PCI ATMU: outbound 256 MiB MMIO, inbound low 256 MiB RAM.
       CCSR and PCI address registers are big-endian; PCI configuration data is little-endian. */
    lis 7, 0xe000
    ori 7, 7, 0x8000
    lis 6, 0x000c
    stw 6, 0x0c20(7)
    stw 6, 0x0c28(7)
    li 6, 0
    stw 6, 0x0c24(7)
    lis 6, 0x8004
    ori 6, 6, 0x401b
    stw 6, 0x0c30(7)
    li 6, 0
    stw 6, 0x0da0(7)
    stw 6, 0x0da8(7)
    stw 6, 0x0dac(7)
    lis 6, 0xa0f5
    ori 6, 6, 0x501b
    stw 6, 0x0db0(7)
    sync

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
