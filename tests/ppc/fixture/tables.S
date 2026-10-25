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

  .section .rodata
  .p2align 2
.Lwords:
  .long 0b, 1b, 2b, 3b, 4b
.Lbytes:
  .byte 0, 2, 4, 2, 0, 4
