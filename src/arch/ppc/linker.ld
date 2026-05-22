ENTRY(_start)

SECTIONS
{
    . = 0x01000000;
    _kernel_start = .;

    .text ALIGN(4K) :
    {
        *(.text.boot)
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
        *(.eh_frame*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    _kernel_file_end = .;

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
}
