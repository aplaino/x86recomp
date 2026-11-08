// Entry of the test fixture, and its two supervisor calls (the test
// platform's, tests/a64/fixture_platform.cpp and oracle.py): the loader
// zeroes .bss and applies the relocations, so crt0 only calls the C entry
// point and exits. A platform that runs the fixture in its own format
// links main.c and ops.S with a crt0 of its own.

  .text
  .global _start
  .type _start, %function
_start:
  .cfi_startproc
  .cfi_undefined x30
  mov x29, #0
  bl fixture_main
  svc #0            // exit
  brk #0
  .cfi_endproc
  .size _start, . - _start

  // u32 fixture_write(const char* str, u64 size)
  .global fixture_write
  .type fixture_write, %function
fixture_write:
  .cfi_startproc
  svc #1
  ret
  .cfi_endproc
  .size fixture_write, . - fixture_write
