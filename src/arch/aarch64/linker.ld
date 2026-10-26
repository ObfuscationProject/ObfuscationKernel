ENTRY(_start)

SECTIONS
{
    . = 0x40080000;
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
        /* Keep file-backed runtime data inside the advertised Linux Image extent. */
        *(.got*)
        *(.init_array*)
        *(.fini_array*)
    }

    _kernel_file_end = .;
    _kernel_image_size = _kernel_file_end - _kernel_start;

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
        _kernel_bootstrap_start = .;
        *(.bootstrap_bss)
        _kernel_bootstrap_end = .;
    }

    _kernel_end = .;
}
