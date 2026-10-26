// The test title's entry point and its XAPI thread startup routine. The
// runtime starts a title's threads as the kernel does (r1 its stack, r13
// its KPCR, nothing else); the C code here (built for the 64-bit ELFv2
// ABI) needs its TOC in r2, and a function called through a pointer its
// own address in r12.
  .text
  .macro TOC
  lis 2, .TOC.@highest
  ori 2, 2, .TOC.@higher
  rldicr 2, 2, 32, 31
  oris 2, 2, .TOC.@high
  ori 2, 2, .TOC.@l
  .endm

  .globl _start
  .type _start, @function
_start:
  mflr 0
  std 0, 16(1)
  stdu 1, -32(1)
  TOC
  bl main
  nop
  addi 1, 1, 32
  ld 0, 16(1)
  mtlr 0
  blr
  .size _start, .-_start

// thread_startup(start, context): ExCreateThread's startup routine.
  .globl thread_startup
  .type thread_startup, @function
thread_startup:
  mflr 0
  std 0, 16(1)
  stdu 1, -32(1)
  TOC
  mr 12, 3
  mtctr 3
  mr 3, 4
  bctrl
  addi 1, 1, 32
  ld 0, 16(1)
  mtlr 0
  blr
  .size thread_startup, .-thread_startup

// interrupt_entry(source, user): the graphics interrupt callback, into
// title.c's GraphicsInterrupt.
  .globl interrupt_entry
  .type interrupt_entry, @function
interrupt_entry:
  mflr 0
  std 0, 16(1)
  stdu 1, -32(1)
  TOC
  bl GraphicsInterrupt
  nop
  addi 1, 1, 32
  ld 0, 16(1)
  mtlr 0
  blr
  .size interrupt_entry, .-interrupt_entry

// audio_entry(*argument): the audio render driver's callback, into
// title.c's AudioCallback.
  .globl audio_entry
  .type audio_entry, @function
audio_entry:
  mflr 0
  std 0, 16(1)
  stdu 1, -32(1)
  TOC
  bl AudioCallback
  nop
  addi 1, 1, 32
  ld 0, 16(1)
  mtlr 0
  blr
  .size audio_entry, .-audio_entry

// mftb, for the time base check.
  .globl time_base
  .type time_base, @function
time_base:
  mftb 3
  blr
  .size time_base, .-time_base

// The current KPCR.
  .globl pcr
  .type pcr, @function
pcr:
  mr 3, 13
  blr
  .size pcr, .-pcr

// Calls with nine arguments: the ninth, at r1 + 96 in the caller's ELFv2
// parameter area, goes where the Xenon's ABI has it, 8 bytes at r1 + 0x50.
  .macro XENON9 name, target
  .globl \name
  .type \name, @function
\name:
  mflr 0
  std 0, 16(1)
  ld 11, 96(1)
  stdu 1, -128(1)
  std 11, 0x50(1)
  bl \target
  nop
  addi 1, 1, 128
  ld 0, 16(1)
  mtlr 0
  blr
  .size \name, .-\name
  .endm

  XENON9 sprintf9, sprintf
  XENON9 NtCreateFile9, NtCreateFile
  XENON9 NtQueryDirectoryFile9, NtQueryDirectoryFile
  XENON9 XamUserReadProfileSettings9, XamUserReadProfileSettings
  XENON9 XamContentCreateEx9, XamContentCreateEx

// The same with ten: the ninth and tenth to r1 + 0x50 and r1 + 0x58.
  .macro XENON10 name, target
  .globl \name
  .type \name, @function
\name:
  mflr 0
  std 0, 16(1)
  ld 11, 96(1)
  ld 12, 104(1)
  stdu 1, -128(1)
  std 11, 0x50(1)
  std 12, 0x58(1)
  bl \target
  nop
  addi 1, 1, 128
  ld 0, 16(1)
  mtlr 0
  blr
  .size \name, .-\name
  .endm

  XENON10 VdSwap10, VdSwap
