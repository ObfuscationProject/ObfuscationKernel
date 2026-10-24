ENTRY(_start)

SECTIONS
{
    . = 0x40010000;
    _kernel_start = .;

    .text ALIGN(4K) :
    {
        KEEP(*(.text.boot))
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
        *(.eh_frame*)
    }

    .ARM.exidx ALIGN(4) : { *(.ARM.exidx*) }
    .ARM.extab ALIGN(4) : { *(.ARM.extab*) }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    /* An emitted terminal byte makes the header extent match objcopy even
       when the final input data section is empty or ends before alignment. */
    .image_tail : { BYTE(0); _kernel_file_end = .; }

    .bss ALIGN(4K) (NOLOAD) :
    {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss*)
        _bss_end = .;
    }

    .bootstrap_bss ALIGN(4K) (NOLOAD) :
    {
        *(.bootstrap_bss)
    }

    _kernel_end = .;
    ASSERT(_start == 0x40010000, "ARM Linux-header entry moved")
    ASSERT(_kernel_file_end > _kernel_start + 48, "ARM image lacks payload")
    ASSERT(_kernel_end <= 0x50000000, "ARM image exceeds supervisor RAM aperture")
}
