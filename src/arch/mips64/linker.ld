OUTPUT_FORMAT("elf64-bigmips")
ENTRY(_start)

SECTIONS
{
    . = 0xffffffff80010000;
    _kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - 0xffffffff00000000)
    {
        *(.text.boot)
        *(.text*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - 0xffffffff00000000)
    {
        *(.rodata*)
        *(.eh_frame*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - 0xffffffff00000000)
    {
        *(.data*)
    }

    _kernel_file_end = .;

    .bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bss) - 0xffffffff00000000)
    {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss*)
        _bss_end = .;
    }

    .bootstrap_bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bootstrap_bss) - 0xffffffff00000000)
    {
        *(.bootstrap_bss)
    }

    _kernel_end = .;
}
