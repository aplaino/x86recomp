// Control flow the compiler does not emit for the fixture's C.
  .text

// Position-independent code reading its own address: `bl .+4` and
// `bcl 20,31,.+4` (and a conditional form, taken or not) set LR to the
// next instruction's address and fall through; they are not calls. Each
// returns v + 1 + (the address read - the label's address): v + 1 when
// the code after it runs once.
  .globl pic_bl
  .type pic_bl, @function
pic_bl:
  mflr 0
  bl .Lpic_bl
.Lpic_bl:
  mflr 11
  mtlr 0
  lis 12, .Lpic_bl@high
  ori 12, 12, .Lpic_bl@l
  subf 11, 12, 11
  add 3, 3, 11
  addi 3, 3, 1
  blr
  .size pic_bl, .-pic_bl

  .globl pic_bcl
  .type pic_bcl, @function
pic_bcl:
  mflr 0
  bcl 20, 31, .Lpic_bcl
.Lpic_bcl:
  mflr 11
  mtlr 0
  lis 12, .Lpic_bcl@high
  ori 12, 12, .Lpic_bcl@l
  subf 11, 12, 11
  add 3, 3, 11
  addi 3, 3, 1
  blr
  .size pic_bcl, .-pic_bcl

  .globl pic_beql
  .type pic_beql, @function
pic_beql:
  mflr 0
  cmpwi 3, 5
  beql .Lpic_beql
.Lpic_beql:
  mflr 11
  mtlr 0
  lis 12, .Lpic_beql@high
  ori 12, 12, .Lpic_beql@l
  subf 11, 12, 11
  add 3, 3, 11
  addi 3, 3, 1
  blr
  .size pic_beql, .-pic_beql

// A loop whose back edge crosses function starts. cross_mid enters it in
// its middle, so the lifter starts a function at .Lcross_mid (a branch out
// of cross_mid's range) and one at .Lcross_head (the back edge, out of
// .Lcross_mid's): each iteration is two tail calls, which must not grow
// the host stack. No sizes: a sized symbol's range would hold the loop.
// r3 = the iterations (> 0).
  .globl cross_loop
  .type cross_loop, @function
cross_loop:
  mtctr 3
  li 4, 0
.Lcross_head:
  addi 4, 4, 3
.Lcross_mid:
  rlwinm 4, 4, 1, 0, 31
  xori 4, 4, 0x55
  bdnz .Lcross_head
  mr 3, 4
  blr

  .globl cross_mid
  .type cross_mid, @function
cross_mid:
  mtctr 3
  mr 4, 3
  b .Lcross_mid
