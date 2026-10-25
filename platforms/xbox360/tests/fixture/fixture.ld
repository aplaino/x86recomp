/* The fixture at a title's usual load address: the PE header page at
   0x82000000 (xbox360_elf2xex writes it), code from the next page. */
ENTRY(_start)
SECTIONS
{
  . = 0x82001000;
  .text : { *(.text .text.*) }
  . = ALIGN(0x1000);
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(0x1000);
  .data : { *(.data .data.*) }
  .got : { *(.got) *(.toc) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
