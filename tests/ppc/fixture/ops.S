// One example of every instruction form the lifter decodes, for the form
// test (recomp_lift_ppc --forms runs each one alone against the oracle).
// Never called. r31 is left out: the oracle's stubs keep their state
// pointer there. Base registers of memory forms point into the test's
// buffer; everything else is random.
  .text
  .globl ops
  .type ops, @function
ops:
  // D-form arithmetic
  addi 3, 4, -12345
  addi 3, 0, 77
  addis 5, 6, -2
  addis 5, 0, 0x7FFF
  addic 7, 8, 1000
  addic. 7, 8, -1
  subfic 9, 10, -7
  mulli 11, 12, -32768
  // XO-form arithmetic, with OE and Rc
  add 3, 4, 5
  add. 3, 4, 5
  addo 3, 4, 5
  addo. 3, 4, 5
  addc 6, 7, 8
  addco. 6, 7, 8
  adde 9, 10, 11
  addeo 9, 10, 11
  addme 12, 13
  addmeo. 12, 13
  addze 14, 15
  addzeo 14, 15
  subf 3, 4, 5
  subf. 3, 4, 5
  subfo 3, 4, 5
  subfc 6, 7, 8
  subfco. 6, 7, 8
  subfe 9, 10, 11
  subfeo. 9, 10, 11
  subfme 12, 13
  subfmeo 12, 13
  subfze 14, 15
  subfzeo. 14, 15
  neg 16, 17
  nego. 16, 17
  mullw 3, 4, 5
  mullwo. 3, 4, 5
  mulld 6, 7, 8
  mulldo 6, 7, 8
  mulhw 9, 10, 11
  mulhw. 9, 10, 11
  mulhwu 12, 13, 14
  mulhd 15, 16, 17
  mulhdu 18, 19, 20
  divw 3, 4, 5
  divwo. 3, 4, 5
  divwu 6, 7, 8
  divwuo 6, 7, 8
  divd 9, 10, 11
  divdo. 9, 10, 11
  divdu 12, 13, 14
  divduo 12, 13, 14
  // logical
  andi. 3, 4, 0x8001
  andis. 3, 4, 0x8001
  ori 5, 6, 0xFFFF
  oris 5, 6, 0x8000
  xori 7, 8, 0x1234
  xoris 7, 8, 0xFFFF
  and 3, 4, 5
  and. 3, 4, 5
  andc 6, 7, 8
  or 9, 10, 11
  or. 9, 10, 11
  orc 12, 13, 14
  xor 15, 16, 17
  nand 18, 19, 20
  nor 21, 22, 23
  nor. 21, 22, 23
  eqv 24, 25, 26
  extsb 3, 4
  extsb. 3, 4
  extsh 5, 6
  extsw 7, 8
  extsw. 7, 8
  cntlzw 9, 10
  cntlzw. 9, 10
  cntlzd 11, 12
  // rotates and shifts
  rlwinm 3, 4, 5, 6, 20
  rlwinm 3, 4, 0, 0, 31
  rlwinm 3, 4, 31, 28, 3
  rlwinm. 3, 4, 2, 0, 29
  rlwimi 5, 6, 8, 16, 23
  rlwimi 5, 6, 30, 28, 1
  rlwnm 7, 8, 9, 0, 31
  rlwnm. 7, 8, 9, 30, 2
  rldicl 3, 4, 13, 7
  rldicl. 3, 4, 63, 0
  rldicr 5, 6, 3, 60
  rldic 7, 8, 21, 11
  rldimi 9, 10, 40, 8
  rldimi. 9, 10, 0, 63
  rldcl 11, 12, 13, 5
  rldcr 14, 15, 16, 57
  slw 3, 4, 5
  slw. 3, 4, 5
  srw 6, 7, 8
  sraw 9, 10, 11
  sraw. 9, 10, 11
  srawi 12, 13, 0
  srawi 12, 13, 7
  srawi. 12, 13, 31
  sld 14, 15, 16
  srd 17, 18, 19
  srad 20, 21, 22
  srad. 20, 21, 22
  sradi 23, 24, 0
  sradi 23, 24, 33
  sradi. 23, 24, 63
  // compares
  cmpw 0, 3, 4
  cmpw 7, 3, 4
  cmpd 1, 5, 6
  cmplw 2, 7, 8
  cmpld 3, 9, 10
  cmpwi 4, 11, -5
  cmpdi 5, 12, 32767
  cmplwi 6, 13, 0xFFFF
  cmpldi 0, 14, 1
  // condition register
  crand 0, 5, 30
  cror 1, 2, 3
  crxor 4, 4, 4
  crnand 31, 0, 1
  crnor 7, 8, 9
  creqv 10, 11, 12
  crandc 13, 14, 15
  crorc 16, 17, 18
  mcrf 3, 6
  .long 0x7D000400  // mcrxr 2 (no mnemonic in LLVM: the 64-bit ISA dropped it)
  mfcr 3
  mtcrf 0xFF, 4
  mtcrf 0x21, 5
  mtocrf 0x10, 6
  mfocrf 7, 0x04
  // special purpose registers
  mfxer 3
  mtxer 4
  mflr 5
  mtlr 6
  mfctr 7
  mtctr 8
  // loads
  lbz 3, 17(4)
  lbzu 3, -1(4)
  lbzx 5, 6, 7
  lbzx 5, 0, 7
  lbzux 5, 6, 7
  lhz 8, 2(9)
  lhzu 8, 4(9)
  lhzx 10, 11, 12
  lhzux 10, 11, 12
  lha 13, -6(14)
  lhau 13, 6(14)
  lhax 15, 16, 17
  lhaux 15, 16, 17
  lwz 3, 8(4)
  lwzu 3, -8(4)
  lwzx 5, 6, 7
  lwzux 5, 6, 7
  lwa 8, 12(9)
  lwax 10, 11, 12
  lwaux 10, 11, 12
  ld 13, 16(14)
  ldu 13, -16(14)
  ldx 15, 16, 17
  ldux 15, 16, 17
  lhbrx 18, 19, 20
  lwbrx 21, 22, 23
  // stores
  stb 3, 5(4)
  stbu 3, -5(4)
  stbx 5, 6, 7
  stbux 5, 6, 7
  sth 8, 6(9)
  sthu 8, 2(9)
  sthx 10, 11, 12
  sthux 10, 11, 12
  stw 13, 4(14)
  stwu 13, -4(14)
  stwx 15, 16, 17
  stwux 15, 16, 17
  std 18, 8(19)
  stdu 18, -24(19)
  stdx 20, 21, 22
  stdux 20, 21, 22
  sthbrx 23, 24, 25
  stwbrx 26, 27, 28
  // reservations, ordering and the cache
  lwarx 3, 4, 5
  ldarx 6, 7, 8
  stwcx. 9, 10, 11
  stdcx. 12, 13, 14
  sync
  lwsync
  eieio
  isync
  dcbt 3, 4
  dcbst 5, 6
  dcbf 7, 8
  dcbz 9, 10
  // floating-point loads and stores
  lfs 1, 8(4)
  lfsu 2, -4(5)
  lfsx 3, 6, 7
  lfsux 4, 8, 9
  lfd 5, 16(10)
  lfdu 6, -8(11)
  lfdx 7, 12, 13
  lfdux 8, 14, 15
  stfs 9, 4(16)
  stfsu 10, -8(17)
  stfsx 11, 18, 19
  stfsux 12, 20, 21
  stfd 13, 8(22)
  stfdu 14, -16(23)
  stfdx 15, 24, 25
  stfdux 16, 26, 27
  stfiwx 17, 28, 29
  // floating-point arithmetic, double and single, with record forms
  fadd 1, 2, 3
  fadd. 1, 2, 3
  fsub 4, 5, 6
  fsub. 4, 5, 6
  fmul 7, 8, 9
  fmul. 7, 8, 9
  fdiv 10, 11, 12
  fdiv. 10, 11, 12
  fsqrt 13, 14
  fmadd 15, 16, 17, 18
  fmadd. 15, 16, 17, 18
  fmsub 19, 20, 21, 22
  fnmadd 23, 24, 25, 26
  fnmadd. 23, 24, 25, 26
  fnmsub 27, 28, 29, 30
  fadds 1, 2, 3
  fadds. 1, 2, 3
  fsubs 4, 5, 6
  fmuls 7, 8, 9
  fmuls. 7, 8, 9
  fdivs 10, 11, 12
  fsqrts 13, 14
  fmadds 15, 16, 17, 18
  fmsubs 19, 20, 21, 22
  fmsubs. 19, 20, 21, 22
  fnmadds 23, 24, 25, 26
  fnmsubs 27, 28, 29, 30
  fnmsubs. 27, 28, 29, 30
  fres 1, 2
  frsqrte 3, 4
  fsel 5, 6, 7, 8
  fsel. 5, 6, 7, 8
  // moves, rounding and conversions
  fmr 9, 10
  fmr. 9, 10
  fneg 11, 12
  fabs 13, 14
  fnabs 15, 16
  frsp 17, 18
  frsp. 17, 18
  fctiw 19, 20
  fctiwz 21, 22
  fctiwz. 21, 22
  fctid 23, 24
  fctidz 25, 26
  fcfid 27, 28
  fcfid. 27, 28
  // compares and the FPSCR
  fcmpu 0, 1, 2
  fcmpu 7, 3, 4
  fcmpo 1, 5, 6
  fcmpo 6, 7, 8
  mffs 9
  mffs. 9
  mtfsf 0xFF, 10
  mtfsf 0x81, 11
  mtfsf. 0x3C, 12
  mtfsfi 7, 3
  mtfsfi 0, 9
  mtfsfi. 5, 15
  mtfsb0 31
  mtfsb0 3
  mtfsb1 6
  mtfsb1 24
  .long 0xFFC0004D  // mtfsb1. 30 (LLVM has no record form of it)
  mcrfs 2, 0
  mcrfs 5, 3
  blr
  .size ops, .-ops
