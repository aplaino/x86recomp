// Control flow the compiler does not emit for the fixture's C.
  .text

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
