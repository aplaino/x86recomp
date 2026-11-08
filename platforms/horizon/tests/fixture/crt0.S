// Entry and module header of the AArch64 core's test fixture
// (tests/a64/fixture) as a Switch homebrew NRO: bytes 0x10-0x7F are the
// NRO header horizon_elf2nro writes, MOD0 locates .dynamic, .bss and
// .eh_frame_hdr. The loader zeroes .bss and applies the relocations, so
// crt0 only calls the C entry point and exits; the fixture's output goes
// through svcOutputDebugString.

  .section .crt0, "ax", %progbits
  .global _start
  .type _start, %function
_start:
  b __nx_start
  .size _start, 4
  .word __nx_mod0 - _start
  .ascii "HOMEBREW"
  .fill 0x70, 1, 0

  .balign 4
__nx_mod0:
  .ascii "MOD0"
  .word __dynamic_start - __nx_mod0
  .word __bss_start - __nx_mod0
  .word __bss_end - __nx_mod0
  .word __eh_frame_hdr_start - __nx_mod0
  .word __eh_frame_hdr_end - __nx_mod0
  .word 0

  .text
  .global __nx_start
  .type __nx_start, %function
__nx_start:
  .cfi_startproc
  .cfi_undefined x30
  mov x29, #0
  bl fixture_main
  svc #0x7          // svcExitProcess
  brk #0
  .cfi_endproc
  .size __nx_start, . - __nx_start

  // u32 fixture_write(const char* str, u64 size): Result
  // svcOutputDebugString(const char* str, u64 size)
  .global fixture_write
  .type fixture_write, %function
fixture_write:
  .cfi_startproc
  svc #0x27
  ret
  .cfi_endproc
  .size fixture_write, . - fixture_write
