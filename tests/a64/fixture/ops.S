// Instructions the C part of the fixture does not get from the compiler:
// flag register access, condition-compare chains, division corner cases,
// bitfield and reverse forms, exclusive and acquire/release accesses, the
// thread pointer, non-temporal pairs and literal loads. Each function folds
// its results into x0 so the fixture's output depends on every one.

  .text

  // u64 ops_flags(u64 a, u64 b)
  .global ops_flags
  .type ops_flags, %function
ops_flags:
  .cfi_startproc
  adds x2, x0, x1
  adcs x3, x0, x1
  sbcs x4, x0, x1
  mrs x5, nzcv
  ngcs x6, x1
  adcs w7, w0, w1
  sbcs w8, w1, w0
  ccmp x0, x1, #4, ne
  ccmn x0, #3, #2, ge
  cset x9, hi
  ccmp w0, #17, #9, lo
  csetm w10, lt
  msr nzcv, x0
  csinv x11, x0, x1, vs
  csneg x12, x1, x0, mi
  cinc w13, w1, gt
  mrs x14, nzcv
  eor x0, x2, x3
  eor x0, x0, x4, ror #7
  add x0, x0, x5
  eon x0, x0, x6
  add x0, x0, x7, lsl #3
  bic x0, x0, x8, asr #2
  orn x0, x0, x9, lsr #1
  add x0, x0, x10
  eor x0, x0, x11
  add x0, x0, x12
  add x0, x0, x13, lsl #32
  eor x0, x0, x14
  ret
  .cfi_endproc
  .size ops_flags, . - ops_flags

  // u64 ops_div(u64 a, u64 b)
  .global ops_div
  .type ops_div, %function
ops_div:
  .cfi_startproc
  udiv x2, x0, x1
  sdiv x3, x0, x1
  udiv w4, w0, w1
  sdiv w5, w0, w1
  udiv x6, x0, xzr
  sdiv w7, w0, wzr
  mov x8, #0x8000000000000000
  mov x9, #-1
  sdiv x10, x8, x9
  mov w11, #0x80000000
  sdiv w12, w11, w9
  msub x13, x2, x1, x0
  eor x0, x2, x3, lsl #1
  add x0, x0, x4
  eor x0, x0, x5, lsl #5
  add x0, x0, x6
  add x0, x0, x7
  eor x0, x0, x10
  add x0, x0, x12
  eor x0, x0, x13, ror #11
  ret
  .cfi_endproc
  .size ops_div, . - ops_div

  // u64 ops_bits(u64 a, u64 b)
  .global ops_bits
  .type ops_bits, %function
ops_bits:
  .cfi_startproc
  extr x2, x0, x1, #13
  extr w3, w0, w1, #29
  rbit x4, x0
  rbit w5, w1
  cls x6, x1
  cls w7, w0
  clz x8, x1
  clz w9, w0
  rev16 x10, x0
  rev32 x11, x1
  rev x12, x0
  rev16 w13, w1
  rev w14, w0
  mov x15, x1
  bfi x15, x0, #9, #17
  mov w16, w0
  bfxil w16, w1, #3, #11
  sbfx x17, x0, #5, #23
  ubfiz w2, w2, #4, #9      // overwrites x2 from its own value
  sbfiz x3, x3, #7, #31
  rorv x4, x4, x1
  asrv w5, w5, w0
  lslv w6, w6, w1
  lsrv x7, x7, x0
  smulh x8, x8, x0
  umulh x9, x9, x1
  smaddl x10, w10, w0, x1
  umsubl x11, w11, w1, x0
  smsubl x12, w12, w1, x12
  umaddl x13, w13, w0, x13
  madd w14, w14, w0, w1
  eor x0, x2, x3
  add x0, x0, x4
  eor x0, x0, x5
  add x0, x0, x6, lsl #2
  eor x0, x0, x7
  add x0, x0, x8
  eor x0, x0, x9
  add x0, x0, x10
  eor x0, x0, x11
  add x0, x0, x12
  eor x0, x0, x13
  add x0, x0, x14
  eor x0, x0, x15
  add x0, x0, x16
  eor x0, x0, x17
  ret
  .cfi_endproc
  .size ops_bits, . - ops_bits

  // u64 ops_tpidr(u64 v): sets, reads and restores the thread pointer.
  .global ops_tpidr
  .type ops_tpidr, %function
ops_tpidr:
  .cfi_startproc
  mrs x2, tpidr_el0
  msr tpidr_el0, x0
  mrs x1, tpidr_el0
  msr tpidr_el0, x2
  mrs x3, tpidr_el0
  eor x0, x1, x3, ror #3
  ret
  .cfi_endproc
  .size ops_tpidr, . - ops_tpidr

  // u64 ops_atomics(void* p, u64 v): p is 16-byte aligned, 32 bytes.
  .global ops_atomics
  .type ops_atomics, %function
ops_atomics:
  .cfi_startproc
1:ldxr x2, [x0]
  add x3, x2, x1
  stxr w4, x3, [x0]
  cbnz w4, 1b
  add x16, x0, #8
2:ldaxr w5, [x16]
  eor w6, w5, w1
  stlxr w4, w6, [x16]
  cbnz w4, 2b
  add x7, x0, #12
4:ldxrb w8, [x7]
  add w8, w8, #1
  stxrb w4, w8, [x7]
  cbnz w4, 4b
  add x9, x0, #14
5:ldaxrh w10, [x9]
  sub w10, w10, w1
  stlxrh w4, w10, [x9]
  cbnz w4, 5b
  ldxr x11, [x0]
  clrex
  stxr w12, x11, [x0]       // fails: the monitor was cleared
  dmb ish
  ldar x13, [x0]
  ldar w14, [x7]
  ldarb w15, [x7]
  ldarh w16, [x9]
  add x17, x0, #16
  stlr x13, [x17]
  stlrb w15, [x7]
  stlrh w16, [x9]
  dmb ishld
  dsb sy
  isb
  ldr x2, [x0]
  ldr x3, [x0, #8]
  ldr x6, [x0, #16]
  eor x0, x2, x3, ror #9
  add x0, x0, x6
  add x0, x0, x12, lsl #40
  eor x0, x0, x14, lsl #20
  ret
  .cfi_endproc
  .size ops_atomics, . - ops_atomics

  // u64 ops_mem(void* p): p is 16-byte aligned, 256 bytes, read-write.
  .global ops_mem
  .type ops_mem, %function
ops_mem:
  .cfi_startproc
  mov x1, x0
  ldnp x2, x3, [x0, #16]
  stnp x3, x2, [x0, #48]
  ldpsw x4, x5, [x0, #8]
  add x6, x0, #128
  stp x2, x4, [x6, #-16]!
  ldp w7, w8, [x6], #24
  ldur x9, [x6, #-3]
  stur w9, [x6, #-37]
  ldrsb w10, [x0, #5]
  ldrsb x11, [x0, #6]
  ldrsh w12, [x0, #10]
  ldrsh x13, [x0, #12]
  mov x14, #-8
  ldr x15, [x6, x14]
  mov w14, #3
  ldr w16, [x0, w14, uxtw #2]
  mov w14, #-2
  ldrsh x17, [x6, w14, sxtw #1]
  strb w10, [x0, #200]!
  strh w12, [x0], #-190
  prfm pldl1keep, [x0, #64]
  prfm pstl2strm, [x0, x14]
  prfm plil1keep, 6f
  sub x0, x0, x1            // the offset, not the address, reaches the result
  ldr x2, 6f
  ldrsw x3, 7f
  ldr w4, 7f
  adr x5, 6f
  ldr x5, [x5, #8]
  eor x0, x0, x2
  add x0, x0, x3
  eor x0, x0, x4
  add x0, x0, x5
  eor x0, x0, x7, lsl #32
  add x0, x0, x8
  eor x0, x0, x9
  add x0, x0, x10
  eor x0, x0, x11
  add x0, x0, x12
  eor x0, x0, x13
  add x0, x0, x15
  eor x0, x0, x16
  add x0, x0, x17
  ret
  .balign 8
6:.quad 0x0123456789abcdef
  .quad 0xfedcba9876543210
7:.word 0x89abcdef
  .word 0
  .cfi_endproc
  .size ops_mem, . - ops_mem
