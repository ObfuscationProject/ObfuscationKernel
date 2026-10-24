ENTRY(_start)
PHDRS { code PT_LOAD FILEHDR PHDRS FLAGS(5); data PT_LOAD FLAGS(6); stack PT_GNU_STACK FLAGS(6); }
SECTIONS
{
    . = USER_BASE + SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    . = USER_BASE + 0x10000;
    .peer_state : { KEEP(*(.peer_state)) } :data
    .data : { *(.data .data.* .sdata .sdata.*) } :data
    PROVIDE(__global_pointer$ = . + 0x800);
    _gp = . + 0x7ff0;
    .got : { *(.got .got.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.*) *(COMMON) } :data
    /DISCARD/ : { *(.note.GNU-stack) *(.comment) *(.eh_frame .eh_frame_hdr) }
}
