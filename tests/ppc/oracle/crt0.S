// Entry point of the bare-metal test programs (the fixture and the form
// oracle): 32-bit mode with FP and VMX on and interrupt vectors at 0, a
// stack, the TOC, main(), then `sc` with main's result in r3, which ends
// the run (oracle/vectors.S under QEMU, the test's Sc() when lifted).
  .text
  .globl _start
  .type _start, @function
_start:
  mfmsr 3
  clrldi 3, 3, 1      // SF = 0: 32-bit mode
  ori 3, 3, 0x2000    // FP
  oris 3, 3, 0x0200   // VEC
  li 4, 0x40
  andc 3, 3, 4        // IP = 0: vectors at 0
  mtmsrd 3
  isync
  lis 1, 0x00D0       // ORACLE_STACK_TOP
  li 0, 0
  stdu 0, -64(1)
  lis 2, .TOC.@highest
  ori 2, 2, .TOC.@higher
  rldicr 2, 2, 32, 31
  oris 2, 2, .TOC.@high
  ori 2, 2, .TOC.@l
  bl main
  nop
  sc
  .size _start, .-_start
