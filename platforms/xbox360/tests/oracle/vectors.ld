/* The vectors at their architected offsets, the shared report code after. */
ENTRY(report)
SECTIONS
{
  .v100 0x100 : { *(.v100) }
  .v200 0x200 : { *(.v200) }
  .v300 0x300 : { *(.v300) }
  .v380 0x380 : { *(.v380) }
  .v400 0x400 : { *(.v400) }
  .v480 0x480 : { *(.v480) }
  .v500 0x500 : { *(.v500) }
  .v600 0x600 : { *(.v600) }
  .v700 0x700 : { *(.v700) }
  .v800 0x800 : { *(.v800) }
  .v900 0x900 : { *(.v900) }
  .vc00 0xC00 : { *(.vc00) }
  .vd00 0xD00 : { *(.vd00) }
  .vf00 0xF00 : { *(.vf00) }
  .vf20 0xF20 : { *(.vf20) }
  .report 0x1000 : { *(.report) }
  /DISCARD/ : { *(.comment) *(.note*) }
}
