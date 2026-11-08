/* Static-PIE layout: text, read-only and data segments, each page-aligned
   from 0, as a64/image.h's LoadElf expects (and a platform's converter
   to its own format). The section boundary symbols are for a platform's
   crt0 whose module header points at them. */
ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  ro PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
  dyn PT_DYNAMIC;
  eh PT_GNU_EH_FRAME;
}
SECTIONS {
  . = 0;
  .text : { KEEP(*(.crt0)) *(.text .text.*) } :text
  . = ALIGN(0x1000);
  .rodata : { *(.rodata .rodata.*) } :ro
  .dynsym : { *(.dynsym) } :ro
  .dynstr : { *(.dynstr) } :ro
  .hash : { *(.hash) } :ro
  .rela.dyn : { *(.rela.dyn) } :ro
  .eh_frame_hdr : {
    __eh_frame_hdr_start = .;
    *(.eh_frame_hdr)
    __eh_frame_hdr_end = .;
  } :ro :eh
  .eh_frame : { KEEP(*(.eh_frame)) } :ro
  . = ALIGN(0x1000);
  .data : { *(.data .data.*) } :data
  .got : { *(.got) *(.got.plt) } :data
  .dynamic : { __dynamic_start = .; *(.dynamic) } :data :dyn
  .bss : {
    __bss_start = .;
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(8);
    __bss_end = .;
  } :data
  /DISCARD/ : { *(.comment) *(.note*) *(.interp) }
}
