// The jump table shapes of the Xbox 360 compiler, which clang does not
// emit (the C is built with -fno-jump-tables): an absolute table of words
// (`lwzx`), and a byte table of offsets from a base (`lbzx`, scaled, added
// to the base). Both take r3 and return a value per case in r3. The
// addresses are built with lis/ori (LLD has no unchecked @ha for an
// address above 2 GiB; the lifter's constant tracking takes either).
  .text
  .globl table_words
  .type table_words, @function
table_words:
  cmplwi 6, 3, 4
  bgt 6, 9f
  lis 11, .Lwords@high
  ori 11, 11, .Lwords@l
  rlwinm 0, 3, 2, 0, 29
  lwzx 0, 11, 0
  mtctr 0
  bctr
0: li 3, 100
  blr
1: li 3, 201
  blr
2: li 3, 302
  blr
3: li 3, 403
  blr
4: li 3, 504
  blr
9: li 3, -1
  blr
  .size table_words, .-table_words

  .globl table_bytes
  .type table_bytes, @function
table_bytes:
  cmplwi 6, 3, 5
  bgt 6, 9f
  lis 12, .Lbytes@high
  ori 12, 12, .Lbytes@l
  lbzx 0, 12, 3
  rlwinm 0, 0, 2, 0, 29
  lis 12, .Lbase@high
  ori 0, 0, 0
  ori 12, 12, .Lbase@l
  add 12, 12, 0
  mtctr 12
  bctr
.Lbase:
  li 3, 7
  blr
  li 3, 17
  blr
  li 3, 27
  blr
9: li 3, -2
  blr
  .size table_bytes, .-table_bytes

// table_slot: unoptimised code, which keeps the index in a stack slot and
// loads it again for the bound and for the table. table_ret: the bound is
// a conditional return (bgtlr), v itself above it.
  .globl table_slot
  .type table_slot, @function
table_slot:
  stwu 1, -32(1)
  stw 3, 16(1)
  lwz 11, 16(1)
  cmplwi 6, 11, 2
  bgt 6, .Ls9
  lwz 11, 16(1)
  lis 12, .Lslot@high
  ori 12, 12, .Lslot@l
  rlwinm 0, 11, 2, 0, 29
  lwzx 0, 12, 0
  mtctr 0
  ori 0, 0, 0
  bctr
.Ls0: li 3, 11
  b .Ls8
.Ls1: li 3, 22
  b .Ls8
.Ls2: li 3, 33
  b .Ls8
.Ls9: li 3, -3
.Ls8: addi 1, 1, 32
  blr
  .size table_slot, .-table_slot

  .globl table_ret
  .type table_ret, @function
table_ret:
  cmplwi 6, 3, 2
  bgtlr 6
  lis 11, .Lret@high
  ori 11, 11, .Lret@l
  rlwinm 0, 3, 2, 0, 29
  lwzx 0, 11, 0
  mtctr 0
  bctr
.Lr0: li 3, 44
  blr
.Lr1: li 3, 55
  blr
.Lr2: li 3, 66
  blr
  .size table_ret, .-table_ret

// table_bits: a switch on v & 3 with no bound (the index cannot exceed
// it), its table right after the bctr, up to the first case.
  .globl table_bits
  .type table_bits, @function
table_bits:
  rlwinm 5, 3, 0, 30, 31
  lis 12, .Lbits@high
  ori 12, 12, .Lbits@l
  rlwinm 0, 5, 2, 0, 29
  lwzx 0, 12, 0
  mtctr 0
  bctr
.Lbits:
  .long .Lb0, .Lb1, .Lb2, .Lb3
.Lb0: li 3, 70
  blr
.Lb1: li 3, 71
  blr
.Lb2: li 3, 72
  blr
.Lb3: li 3, 73
  blr
  .size table_bits, .-table_bits

// Functions without a .pdata entry (no sized symbol), reached only
// through a pointer, after another function's return as compilers leave
// them: ptr_data(v) = v + 11 through a word in data (ptr_table), and
// ptr_thunk, one branch to table_words, through the address call_thunk
// builds with lis/ori.
  .long 0
ptr_data:
  addi 3, 3, 11
  blr
  .long 0
ptr_thunk:
  b table_words
  .long 0

  .globl call_thunk
  .type call_thunk, @function
call_thunk:
  lis 12, ptr_thunk@high
  ori 12, 12, ptr_thunk@l
  mtctr 12
  bctr
  .size call_thunk, .-call_thunk

  .section .rodata
  .p2align 2
  .globl ptr_table
ptr_table:
  .quad ptr_data  // a C pointer here is 64-bit
  .p2align 2
.Lwords:
  .long 0b, 1b, 2b, 3b, 4b
.Lbytes:
  .byte 0, 2, 4, 2, 0, 4
  .p2align 2
.Lslot:
  .long .Ls0, .Ls1, .Ls2
.Lret:
  .long .Lr0, .Lr1, .Lr2
