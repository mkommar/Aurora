ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text : { *(.text.entry) *(.text*) }
    . = ALIGN(4096);
    __text_end = .;
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    __ro_end = .;
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    __image_end = .;
    ASSERT(__image_end < 0x5c0000, "User image overlaps boot info and stack")
    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
