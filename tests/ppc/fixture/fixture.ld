/* The fixture above 2 GiB, where 32-bit mode builds addresses with lis
   and the registers hold their sign-extended forms, from one page past
   0x82000000 (room for an image header below it). */
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
