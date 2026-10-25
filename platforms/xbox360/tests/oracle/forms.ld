/* The form oracle, low in RAM: under the stack and the done area. */
ENTRY(_start)
SECTIONS
{
  . = 0x00100000;
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .got : { *(.got) *(.toc) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
