ENTRY(_start)
PHDRS {
    code PT_LOAD FLAGS(5);
    ro PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x400000;
    .text : { *(.text.entry) *(.text*) } :code
    . = ALIGN(4096);
    .rodata : { *(.rodata*) } :ro
    . = ALIGN(4096);
    .data : { *(.data*) } :data
    .bss : { *(.bss*) *(COMMON) } :data
    ASSERT(. < 0x5c0000, "Application exceeds available memory")
    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
