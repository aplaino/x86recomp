// One example of every instruction form the lifter decodes, for the form
// test (recomp_lift_ppc --forms runs each one alone against the oracle).
// Never called. r31 is left out: the oracle's stubs keep their state
// pointer there. Base registers of memory forms point into the test's
// buffer; everything else is random. VMX128 forms run on the oracle as
// their VMX twins (recomp_lift_ppc, TwinOf), on registers below 30.
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
  // VMX: loads and stores
  lvx 1, 3, 4
  lvxl 2, 5, 6
  stvx 3, 7, 8
  stvxl 4, 9, 10
  lvebx 5, 11, 12
  lvehx 6, 13, 14
  lvewx 7, 15, 16
  stvebx 8, 17, 18
  stvehx 9, 19, 20
  stvewx 10, 21, 22
  lvsl 11, 23, 24
  lvsr 12, 25, 26
  lvx 13, 0, 27
  dst 3, 4, 0
  dss 0
  // VMX: integer arithmetic
  vaddubm 1, 2, 3
  vadduhm 4, 5, 6
  vadduwm 7, 8, 9
  vaddubs 10, 11, 12
  vadduhs 13, 14, 15
  vadduws 16, 17, 18
  vaddsbs 19, 20, 21
  vaddshs 22, 23, 24
  vaddsws 25, 26, 27
  vaddcuw 28, 29, 30
  vsububm 1, 2, 3
  vsubuhm 4, 5, 6
  vsubuwm 7, 8, 9
  vsububs 10, 11, 12
  vsubuhs 13, 14, 15
  vsubuws 16, 17, 18
  vsubsbs 19, 20, 21
  vsubshs 22, 23, 24
  vsubsws 25, 26, 27
  vsubcuw 28, 29, 30
  vavgub 1, 2, 3
  vavguh 4, 5, 6
  vavguw 7, 8, 9
  vavgsb 10, 11, 12
  vavgsh 13, 14, 15
  vavgsw 16, 17, 18
  vmaxub 1, 2, 3
  vmaxuh 4, 5, 6
  vmaxuw 7, 8, 9
  vmaxsb 10, 11, 12
  vmaxsh 13, 14, 15
  vmaxsw 16, 17, 18
  vminub 19, 20, 21
  vminuh 22, 23, 24
  vminuw 25, 26, 27
  vminsb 28, 29, 30
  vminsh 31, 0, 1
  vminsw 2, 3, 4
  vmuleub 1, 2, 3
  vmuleuh 4, 5, 6
  vmulesb 7, 8, 9
  vmulesh 10, 11, 12
  vmuloub 13, 14, 15
  vmulouh 16, 17, 18
  vmulosb 19, 20, 21
  vmulosh 22, 23, 24
  vmhaddshs 1, 2, 3, 4
  vmhraddshs 5, 6, 7, 8
  vmladduhm 9, 10, 11, 12
  vmsumubm 13, 14, 15, 16
  vmsummbm 17, 18, 19, 20
  vmsumuhm 21, 22, 23, 24
  vmsumuhs 25, 26, 27, 28
  vmsumshm 29, 30, 31, 0
  vmsumshs 1, 2, 3, 4
  vsum4ubs 5, 6, 7
  vsum4sbs 8, 9, 10
  vsum4shs 11, 12, 13
  vsum2sws 14, 15, 16
  vsumsws 17, 18, 19
  // VMX: logical, rotates and shifts
  vand 1, 2, 3
  vandc 4, 5, 6
  vor 7, 8, 9
  vnor 10, 11, 12
  vxor 13, 14, 15
  vsel 16, 17, 18, 19
  vrlb 1, 2, 3
  vrlh 4, 5, 6
  vrlw 7, 8, 9
  vslb 10, 11, 12
  vslh 13, 14, 15
  vslw 16, 17, 18
  vsrb 19, 20, 21
  vsrh 22, 23, 24
  vsrw 25, 26, 27
  vsrab 28, 29, 30
  vsrah 1, 2, 3
  vsraw 4, 5, 6
  vsl 7, 8, 9
  vsr 10, 11, 12
  vslo 13, 14, 15
  vsro 16, 17, 18
  // VMX: permutes, merges, splats, packs, unpacks
  vperm 1, 2, 3, 4
  vsldoi 5, 6, 7, 0
  vsldoi 8, 9, 10, 5
  vsldoi 11, 12, 13, 15
  vmrghb 1, 2, 3
  vmrghh 4, 5, 6
  vmrghw 7, 8, 9
  vmrglb 10, 11, 12
  vmrglh 13, 14, 15
  vmrglw 16, 17, 18
  vspltb 1, 2, 0
  vspltb 3, 4, 15
  vsplth 5, 6, 3
  vspltw 7, 8, 2
  vspltisb 9, -16
  vspltisb 10, 7
  vspltish 11, -1
  vspltisw 12, 15
  vspltisw 13, -9
  vpkuhum 1, 2, 3
  vpkuwum 4, 5, 6
  vpkuhus 7, 8, 9
  vpkuwus 10, 11, 12
  vpkshus 13, 14, 15
  vpkswus 16, 17, 18
  vpkshss 19, 20, 21
  vpkswss 22, 23, 24
  vpkpx 25, 26, 27
  vupkhsb 1, 2
  vupkhsh 3, 4
  vupklsb 5, 6
  vupklsh 7, 8
  vupkhpx 9, 10
  vupklpx 11, 12
  // VMX: integer compares
  vcmpequb 1, 2, 3
  vcmpequb. 4, 5, 6
  vcmpequh 7, 8, 9
  vcmpequh. 10, 11, 12
  vcmpequw 13, 14, 15
  vcmpequw. 16, 17, 18
  vcmpgtub 19, 20, 21
  vcmpgtub. 22, 23, 24
  vcmpgtuh 25, 26, 27
  vcmpgtuw. 28, 29, 30
  vcmpgtsb 1, 2, 3
  vcmpgtsh. 4, 5, 6
  vcmpgtsw 7, 8, 9
  vcmpgtsw. 10, 11, 12
  // VMX: floating point
  vaddfp 1, 2, 3
  vsubfp 4, 5, 6
  vmaddfp 7, 8, 9, 10
  vnmsubfp 11, 12, 13, 14
  vmaxfp 15, 16, 17
  vminfp 18, 19, 20
  vrefp 21, 22
  vrsqrtefp 23, 24
  vexptefp 25, 26
  vlogefp 27, 28
  vrfin 1, 2
  vrfiz 3, 4
  vrfip 5, 6
  vrfim 7, 8
  vcfux 9, 10, 0
  vcfux 11, 12, 17
  vcfsx 13, 14, 31
  vctuxs 15, 16, 0
  vctuxs 17, 18, 9
  vctsxs 19, 20, 0
  vctsxs 21, 22, 30
  vcmpeqfp 1, 2, 3
  vcmpeqfp. 4, 5, 6
  vcmpgefp 7, 8, 9
  vcmpgefp. 10, 11, 12
  vcmpgtfp 13, 14, 15
  vcmpgtfp. 16, 17, 18
  vcmpbfp 19, 20, 21
  vcmpbfp. 22, 23, 24
  mfvscr 25
  mtvscr 26
  // VMX128 (no LLVM mnemonics): loads and stores
  .long 0x10242803  // lvsl128 1, 4, 5
  .long 0x10463843  // lvsr128 2, 6, 7
  .long 0x10684883  // lvewx128 3, 8, 9
  .long 0x108A58C3  // lvx128 4, 10, 11
  .long 0x10A060C3  // lvx128 5, 0, 12
  .long 0x10CD7183  // stvewx128 6, 13, 14
  .long 0x10EF81C3  // stvx128 7, 15, 16
  .long 0x111192C3  // lvxl128 8, 17, 18
  .long 0x1133A3C3  // stvxl128 9, 19, 20
  .long 0x1155B403  // lvlx128 10, 21, 22
  .long 0x1177C443  // lvrx128 11, 23, 24
  .long 0x1199D503  // stvlx128 12, 25, 26
  .long 0x11BBE543  // stvrx128 13, 27, 28
  .long 0x11C32603  // lvlxl128 14, 3, 4
  .long 0x11E53643  // lvrxl128 15, 5, 6
  .long 0x12074703  // stvlxl128 16, 7, 8
  .long 0x12295743  // stvrxl128 17, 9, 10
  // VMX128: arithmetic, logical, permutes and packs
  .long 0x10221810  // vsldoi128 1, 2, 3, 0
  .long 0x108531D0  // vsldoi128 4, 5, 6, 7
  .long 0x10E84B10  // vsldoi128 7, 8, 9, 12
  .long 0x14221900  // vperm128 1, 2, 3, 4
  .long 0x14221810  // vaddfp128 1, 2, 3
  .long 0x14853050  // vsubfp128 4, 5, 6
  .long 0x14E84890  // vmulfp128 7, 8, 9
  .long 0x154B60D0  // vmaddfp128 10, 11, 12
  .long 0x15AE7910  // vmaddcfp128 13, 14, 15
  .long 0x16119150  // vnmsubfp128 16, 17, 18
  .long 0x1674A990  // vmsum3fp128 19, 20, 21
  .long 0x16D7C1D0  // vmsum4fp128 22, 23, 24
  .long 0x14221A00  // vpkshss128 1, 2, 3
  .long 0x14853240  // vpkshus128 4, 5, 6
  .long 0x14E84A80  // vpkswss128 7, 8, 9
  .long 0x154B62C0  // vpkswus128 10, 11, 12
  .long 0x15AE7B00  // vpkuhum128 13, 14, 15
  .long 0x16119340  // vpkuhus128 16, 17, 18
  .long 0x1674AB80  // vpkuwum128 19, 20, 21
  .long 0x16D7C3C0  // vpkuwus128 22, 23, 24
  .long 0x14221A10  // vand128 1, 2, 3
  .long 0x14853250  // vandc128 4, 5, 6
  .long 0x14E84A90  // vnor128 7, 8, 9
  .long 0x154B62D0  // vor128 10, 11, 12
  .long 0x15AE7B10  // vxor128 13, 14, 15
  .long 0x16119350  // vsel128 16, 17, 18
  .long 0x1674AB90  // vslo128 19, 20, 21
  .long 0x16D7C3D0  // vsro128 22, 23, 24
  // VMX128: compares, shifts, merges, unpacks
  .long 0x18221800  // vcmpeqfp128 1, 2, 3
  .long 0x18642840  // vcmpeqfp128. 3, 4, 5
  .long 0x18A63880  // vcmpgefp128 5, 6, 7
  .long 0x18E848C0  // vcmpgefp128. 7, 8, 9
  .long 0x192A5900  // vcmpgtfp128 9, 10, 11
  .long 0x196C6940  // vcmpgtfp128. 11, 12, 13
  .long 0x19AE7980  // vcmpbfp128 13, 14, 15
  .long 0x19F089C0  // vcmpbfp128. 15, 16, 17
  .long 0x1A329A00  // vcmpequw128 17, 18, 19
  .long 0x1A74AA40  // vcmpequw128. 19, 20, 21
  .long 0x18221850  // vrlw128 1, 2, 3
  .long 0x188530D0  // vslw128 4, 5, 6
  .long 0x18E84950  // vsraw128 7, 8, 9
  .long 0x194B61D0  // vsrw128 10, 11, 12
  .long 0x19AE7A80  // vmaxfp128 13, 14, 15
  .long 0x1A1192C0  // vminfp128 16, 17, 18
  .long 0x1A74AB00  // vmrghw128 19, 20, 21
  .long 0x1AD7C340  // vmrglw128 22, 23, 24
  .long 0x18201380  // vupkhsb128 1, 2
  .long 0x186023C0  // vupklsb128 3, 4
  .long 0x18A037A0  // vupkhsb128 5, 6, 96
  .long 0x18E047E0  // vupklsb128 7, 8, 96
  // VMX128: conversions, roundings, estimates, splats
  .long 0x18201230  // vcfpsxws128 1, 2, 0
  .long 0x187E2230  // vcfpsxws128 3, 4, 30
  .long 0x18A03270  // vcfpuxws128 5, 6, 0
  .long 0x18E94270  // vcfpuxws128 7, 8, 9
  .long 0x192052B0  // vcsxwfp128 9, 10, 0
  .long 0x197F62B0  // vcsxwfp128 11, 12, 31
  .long 0x19B172F0  // vcuxwfp128 13, 14, 17
  .long 0x18201330  // vrfim128 1, 2
  .long 0x18602370  // vrfin128 3, 4
  .long 0x18A033B0  // vrfip128 5, 6
  .long 0x18E043F0  // vrfiz128 7, 8
  .long 0x19205630  // vrefp128 9, 10
  .long 0x19606670  // vrsqrtefp128 11, 12
  .long 0x19A076B0  // vexptefp128 13, 14
  .long 0x19E086F0  // vlogefp128 15, 16
  .long 0x18201730  // vspltw128 1, 2, 0
  .long 0x18632730  // vspltw128 3, 4, 3
  .long 0x18B00770  // vspltisw128 5, -16
  .long 0x18CF0770  // vspltisw128 6, 15
  // VMX128: word permutes and rotate-insert
  .long 0x183B1210  // vpermwi128 1, 2, 27
  .long 0x186423D0  // vpermwi128 3, 4, 228
  .long 0x18B33310  // vpermwi128 5, 6, 147
  .long 0x18EF4750  // vrlimi128 7, 8, 15, 1
  .long 0x19255710  // vrlimi128 9, 10, 5, 0
  .long 0x196867D0  // vrlimi128 11, 12, 8, 3
  .long 0x19A37790  // vrlimi128 13, 14, 3, 2
  // VMX128: the D3D formats (type; size, where)
  .long 0x18211610  // vpkd3d128 1, 2, 0, 1, 0
  .long 0x186126D0  // vpkd3d128 3, 4, 0, 1, 3
  .long 0x18A23650  // vpkd3d128 5, 6, 0, 2, 1
  .long 0x18E346D0  // vpkd3d128 7, 8, 0, 3, 3
  .long 0x19255610  // vpkd3d128 9, 10, 1, 1, 0
  .long 0x19696690  // vpkd3d128 11, 12, 2, 1, 2
  .long 0x19AD7610  // vpkd3d128 13, 14, 3, 1, 0
  .long 0x19F28610  // vpkd3d128 15, 16, 4, 2, 0
  .long 0x1A369690  // vpkd3d128 17, 18, 5, 2, 2
  .long 0x1A7AA610  // vpkd3d128 19, 20, 6, 2, 0
  .long 0x1ABBB6D0  // vpkd3d128 21, 22, 6, 3, 3
  .long 0x182017F0  // vupkd3d128 1, 2, 0
  .long 0x186427F0  // vupkd3d128 3, 4, 4
  .long 0x18A837F0  // vupkd3d128 5, 6, 8
  .long 0x18EC47F0  // vupkd3d128 7, 8, 12
  .long 0x193057F0  // vupkd3d128 9, 10, 16
  .long 0x197467F0  // vupkd3d128 11, 12, 20
  .long 0x19B877F0  // vupkd3d128 13, 14, 24
  // VMX128: high registers (lifted, not run on the oracle)
  .long 0x1481FC1F  // vaddfp128 100, 65, 127
  .long 0x140100FE  // vmaddfp128 96, 33, 64
  .long 0x17E005CF  // vperm128 127, 64, 96, 7
  .long 0x130428CF  // lvx128 120, 4, 5
  .long 0x181F0569  // vcmpgtfp128. 64, 127, 32
  .long 0x186A479D  // vrlimi128 99, 40, 10, 2
  .long 0x18C0D7AA  // vupkhsb128 70, 90, 96
  blr
  .size ops, .-ops
