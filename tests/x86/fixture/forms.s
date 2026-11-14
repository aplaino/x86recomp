# The 32-bit fixture of lift_fixture_int, lift_fixture_sse and
# lift_fixture_x87 (tests/x86/CMakeLists.txt): integer, SSE/SSE2 and x87
# instruction forms, assembled by clang for i686 and linked by LLD to flat
# code, which `recomp_lift --code` decodes from its first byte to its last
# (so instructions only, no data). Each test takes its own kind of forms
# from it and runs every one lifted and natively.
#
# Memory operands are written in the addressing modes 32-bit code uses
# (base, base + displacement, base + index * scale + displacement, index
# only, absolute, moffs, fs:, [esp + ...], [ebp - ...]); the generator keeps
# each as written where the native run can reproduce it and rewrites the
# rest (KeepAddress, tools/lifter_x86/x87test.cpp).

.intel_syntax noprefix
.text

# ---- integer -----------------------------------------------------------

.irp op, add, or, adc, sbb, and, sub, xor, cmp
  \op eax, ecx
  \op cl, dh
  \op dx, si
  \op ah, bl
  \op eax, 0x12345678
  \op ecx, -5
  \op si, 0x1234
  \op di, -3
  \op al, 0x7F
  \op bl, 0x80
  \op ax, 0x8001
  \op edi, dword ptr [ebx]
  \op ecx, dword ptr [esi + 0x10]
  \op dl, byte ptr [edi - 0x20]
  \op ax, word ptr [ebp + 0x1234]
  \op dword ptr [ebx + esi*4], eax
  \op byte ptr [ecx + edx*8 + 0x40], al
  \op word ptr [esi + edi*2 - 8], dx
  \op dword ptr [edx*2 + 0x500], 0x11223344
  \op byte ptr [0x404000], 0x5A
  \op word ptr [ebx + 0x7FFFF0], -2
  \op dword ptr [esi], 7
  \op eax, dword ptr fs:[0x30]
  \op dword ptr [esp + 8], ecx
  \op eax, dword ptr [ebp - 0x0C]
.endr

.irp op, test
  \op eax, ecx
  \op cl, dh
  \op dx, si
  \op eax, 0x80000001
  \op al, 0x81
  \op ax, 0x4001
  \op ebx, 0x100
  \op bh, 3
  \op dword ptr [esi + 4], edx
  \op byte ptr [ebx + ecx*4 + 2], 0x40
  \op word ptr [edi], 0x8000
.endr

  mov eax, ecx
  mov dl, ah
  mov si, bx
  mov edi, 0x87654321
  mov bh, 0x9C
  mov cx, 0xFFFE
  mov eax, dword ptr [ebx]
  mov ecx, dword ptr [esi + 0x7C]
  mov dh, byte ptr [ebx + edi*2]
  mov di, word ptr [ecx + edx*4 - 0x100]
  mov dword ptr [edi + ebx*8 + 0x18], esi
  mov byte ptr [ebp - 1], cl
  mov word ptr [edx], ax
  mov dword ptr [esi + 0x2000], 0xDEADBEEF
  mov byte ptr [edi], 0x11
  mov word ptr [ecx*4 + 0x40], 0x2222
  mov eax, dword ptr [0x404000]
  mov al, byte ptr [0x404010]
  mov ax, word ptr [0x404020]
  mov dword ptr [0x404000], eax
  mov byte ptr [0x404008], al
  mov ecx, dword ptr [0x404004]
  mov dword ptr [0x404040], 0x1234
  mov eax, dword ptr fs:[0x18]
  mov ecx, dword ptr fs:[0]
  mov dword ptr fs:[0], edx
  mov edx, dword ptr fs:[eax + 0x2C]
  mov ebx, dword ptr [esp + 4]
  mov dword ptr [esp + 0x10], eax

.irp op, movzx, movsx
  \op eax, cl
  \op ecx, dh
  \op edx, si
  \op ax, bl
  \op esi, byte ptr [ebx + 3]
  \op edi, word ptr [ecx + edx*2]
  \op bx, byte ptr [esi]
.endr

.irp op, inc, dec, neg, not
  \op eax
  \op esi
  \op cl
  \op ah
  \op dx
  \op dword ptr [ebx]
  \op byte ptr [esi + edi*4 + 0x30]
  \op word ptr [ecx - 6]
.endr

.irp op, rol, ror, rcl, rcr, shl, shr, sar
  \op eax, 1
  \op ecx, cl
  \op edx, 7
  \op bl, 1
  \op dh, cl
  \op al, 5
  \op si, 1
  \op di, cl
  \op bx, 13
  \op dword ptr [esi], 1
  \op byte ptr [edi + 0x11], cl
  \op word ptr [ebx + esi*2], 3
  \op dword ptr [edx*8 + 0x80], 31
.endr

.irp op, shld, shrd
  \op eax, ecx, 5
  \op edx, ebx, cl
  \op si, di, 9
  \op ax, dx, cl
  \op dword ptr [ebx + 8], esi, 17
  \op word ptr [edi], dx, cl
.endr

.irp op, mul, imul, div, idiv
  \op ecx
  \op bl
  \op si
  \op dword ptr [ebx + 4]
  \op byte ptr [esi + edi*2]
  \op word ptr [edi - 2]
.endr
  imul eax, ecx
  imul dx, si
  imul esi, dword ptr [ebx]
  imul ecx, edx, 7
  imul eax, ebx, 0x12345
  imul di, bx, -3
  imul edx, dword ptr [esi + 0x20], 1000
  cdq
  cwde
  cbw
  cwd

.irp cc, o, no, b, ae, e, ne, be, a, s, ns, p, np, l, ge, le, g
  set\cc al
  set\cc byte ptr [ebx + esi]
  cmov\cc eax, ecx
  cmov\cc dx, si
  cmov\cc edi, dword ptr [ebx + 0x40]
.endr

  xchg eax, ecx
  xchg eax, esi
  xchg dl, bh
  xchg si, di
  xchg dword ptr [ebx], ecx
  xchg byte ptr [esi + 1], al
  xadd eax, ecx
  xadd dword ptr [edi + 8], edx
  xadd byte ptr [ebx], cl
  lock xadd dword ptr [esi], eax
  cmpxchg ecx, edx
  cmpxchg dword ptr [ebx], esi
  cmpxchg byte ptr [edi + 2], dl
  lock cmpxchg dword ptr [ebx + ecx*4], edx
  lock add dword ptr [esi], 1
  lock inc dword ptr [ebx]
  lock dec dword ptr [edi + 4]
  lock or byte ptr [ebx], 0x80
  lock xchg dword ptr [esi], eax

.irp op, bt, bts, btr, btc
  \op eax, ecx
  \op dx, si
  \op ebx, 17
  \op dword ptr [esi], 5
  \op word ptr [edi + 2], 12
  \op dword ptr [ebx], ecx
.endr
  bsf eax, ecx
  bsr edx, esi
  bsf si, di
  bsr ecx, dword ptr [ebx + 4]
  bswap eax
  bswap edi

  lea eax, [ebx + ecx*4 + 0x10]
  lea esi, [edi + 0x12345678]
  lea edx, [eax*8]
  lea ecx, [ebp - 4]
  lea di, [ebx + esi*2 + 6]

  sahf
  lahf
  clc
  stc
  cmc
  cld
  std

  movsb
  movsw
  movsd
  stosb
  stosw
  stosd
  lodsb
  lodsw
  lodsd
  scasb
  scasw
  scasd
  cmpsb
  cmpsw
  cmpsd
  rep movsb
  rep movsd
  rep stosb
  rep stosw
  rep stosd
  repe cmpsb
  repne scasb
  repe scasd
  repne cmpsd
  # The source's segment override (fs: runs natively as gs:), and 16-bit
  # addressing where a segment base makes it reachable.
  movsb byte ptr es:[edi], byte ptr fs:[esi]
  rep movsd dword ptr es:[edi], dword ptr fs:[esi]
  lodsw ax, word ptr fs:[esi]
  cmpsb byte ptr fs:[esi], byte ptr es:[edi]
  lodsb al, byte ptr fs:[si]
  xlatb
  # xlat fs:[ebx + al] and, 16-bit, fs:[bx + al] (the assembler takes no
  # override on xlat).
  .byte 0x64, 0xD7
  .byte 0x67, 0x64, 0xD7

  # 16-bit push and pop (the 32-bit ones move 8 bytes in 64-bit mode).
  push ax
  push si
  pop dx
  pop bx
  push word ptr [ebx]
  push word ptr [esi + edi*2 + 6]
  pop word ptr [edi]
  pop word ptr [ebx + 0x10]
.att_syntax
  pushw $0x1234
  pushw $-3
.intel_syntax noprefix

# ---- SSE and SSE2 ------------------------------------------------------

.irp op, add, sub, mul, div, min, max, sqrt
  \op\()ps xmm0, xmm1
  \op\()ss xmm2, xmm3
  \op\()pd xmm4, xmm5
  \op\()sd xmm6, xmm7
  \op\()ps xmm1, xmmword ptr [ebx]
  \op\()ss xmm3, dword ptr [esi + 0x10]
  \op\()pd xmm5, xmmword ptr [edi + ecx*2 + 0x20]
  \op\()sd xmm7, qword ptr [edx*8 + 0x100]
.endr

.irp op, and, andn, or, xor
  \op\()ps xmm0, xmm1
  \op\()pd xmm2, xmm3
  \op\()ps xmm4, xmmword ptr [esi]
  \op\()pd xmm6, xmmword ptr [ebx + 0x30]
.endr

.irp imm, 0, 1, 2, 3, 4, 5, 6, 7
  cmpps xmm0, xmm1, \imm
  cmpss xmm2, xmm3, \imm
  cmppd xmm4, xmm5, \imm
  cmpsd xmm6, xmm7, \imm
.endr
  cmpps xmm1, xmmword ptr [ebx], 1
  cmpsd xmm2, qword ptr [esi + 8], 2

  comiss xmm0, xmm1
  ucomiss xmm2, xmm3
  comisd xmm4, xmm5
  ucomisd xmm6, xmm7
  comiss xmm1, dword ptr [ebx]
  ucomisd xmm2, qword ptr [esi + edi*4]

  movaps xmm0, xmm1
  movaps xmm2, xmmword ptr [ebx]
  movaps xmmword ptr [esi + 0x20], xmm3
  movups xmm4, xmmword ptr [edi + 3]
  movups xmmword ptr [ebx + ecx*2 + 1], xmm5
  movss xmm0, xmm1
  movss xmm2, dword ptr [esi]
  movss dword ptr [edi - 4], xmm3
  movsd xmm4, xmm5
  movsd xmm6, qword ptr [ebx + 8]
  movsd qword ptr [esi + edx*8], xmm7
  movapd xmm0, xmmword ptr [ebx + 0x10]
  movupd xmmword ptr [edi + 7], xmm1
  movdqa xmm2, xmm3
  movdqa xmm4, xmmword ptr [esi]
  movdqa xmmword ptr [ebx + 0x40], xmm5
  movdqu xmm6, xmmword ptr [edi + 5]
  movdqu xmmword ptr [ecx*4 + 0x200], xmm7
  movdqa xmm0, xmmword ptr [0x404000]
  movss xmm1, dword ptr [0x404010]
  movlps xmm0, qword ptr [ebx]
  movlps qword ptr [esi + 8], xmm1
  movhps xmm2, qword ptr [edi]
  movhps qword ptr [ebx + 0x18], xmm3
  movlpd xmm4, qword ptr [esi]
  movhpd qword ptr [edi + 8], xmm5
  movhlps xmm6, xmm7
  movlhps xmm0, xmm1
  movd xmm0, eax
  movd xmm1, dword ptr [ebx]
  movd ecx, xmm2
  movd dword ptr [esi + 4], xmm3
  movq xmm4, xmm5
  movq xmm6, qword ptr [edi]
  movq qword ptr [ebx + 0x10], xmm7
  movmskps eax, xmm1
  movmskpd edx, xmm2
  pmovmskb ecx, xmm3
  movntps xmmword ptr [ebx], xmm0
  movntdq xmmword ptr [esi + 0x20], xmm1
  movnti dword ptr [edi], eax
  stmxcsr dword ptr [ebx + 4]

  cvtsi2ss xmm0, eax
  cvtsi2ss xmm1, dword ptr [ebx]
  cvtsi2sd xmm2, ecx
  cvtsi2sd xmm3, dword ptr [esi + 4]
  cvtss2si eax, xmm4
  cvtss2si edx, dword ptr [edi]
  cvttss2si ecx, xmm5
  cvtsd2si esi, xmm6
  cvtsd2si eax, qword ptr [ebx + 8]
  cvttsd2si edi, xmm7
  cvttsd2si ecx, qword ptr [esi]
  cvtps2pd xmm0, xmm1
  cvtpd2ps xmm2, xmm3
  cvtss2sd xmm4, xmm5
  cvtsd2ss xmm6, xmm7
  cvtss2sd xmm0, dword ptr [ebx]
  cvtdq2ps xmm1, xmm2
  cvtps2dq xmm3, xmm4
  cvttps2dq xmm5, xmm6
  cvtdq2pd xmm7, xmm0
  cvtpd2dq xmm1, xmm2
  cvttpd2dq xmm3, xmm4
  cvtps2pd xmm5, qword ptr [esi]

  shufps xmm0, xmm1, 0x1B
  shufps xmm2, xmmword ptr [ebx], 0xE4
  shufpd xmm3, xmm4, 1
  unpcklps xmm5, xmm6
  unpckhps xmm7, xmm0
  unpcklpd xmm1, xmm2
  unpckhpd xmm3, xmmword ptr [esi]
  pshufd xmm0, xmm1, 0x4E
  pshufd xmm2, xmmword ptr [edi], 0x93
  pshuflw xmm3, xmm4, 0x1B
  pshufhw xmm5, xmm6, 0xB1

.irp op, paddb, paddw, paddd, paddq, psubb, psubw, psubd, psubq, paddsb, paddsw, paddusb, paddusw, psubsb, psubsw, psubusb, psubusw, pmullw, pmulhw, pmulhuw, pmuludq, pmaddwd, pavgb, pavgw, pmaxsw, pmaxub, pminsw, pminub, psadbw, pand, pandn, por, pxor, pcmpeqb, pcmpeqw, pcmpeqd, pcmpgtb, pcmpgtw, pcmpgtd, packsswb, packssdw, packuswb, punpcklbw, punpcklwd, punpckldq, punpcklqdq, punpckhbw, punpckhwd, punpckhdq, punpckhqdq
  \op xmm0, xmm1
  \op xmm2, xmmword ptr [ebx + 0x10]
.endr
.irp op, psllw, pslld, psllq, psrlw, psrld, psrlq, psraw, psrad
  \op xmm0, xmm1
  \op xmm2, 3
  \op xmm3, 17
  \op xmm4, xmmword ptr [esi]
.endr
  pslldq xmm5, 3
  psrldq xmm6, 9
  pextrw eax, xmm1, 3
  pinsrw xmm2, ecx, 5
  pinsrw xmm3, word ptr [ebx], 1

# ---- x87 ---------------------------------------------------------------

  fld dword ptr [ebx]
  fld qword ptr [esi + 8]
  fld tbyte ptr [edi + ecx*2 + 0x10]
  fld st(3)
  fld dword ptr [0x404000]
  fld qword ptr fs:[0x40]
  fld dword ptr [esp + 4]
  fst dword ptr [ebx + 4]
  fst qword ptr [edx*8 + 0x200]
  fst st(2)
  fstp dword ptr [esi]
  fstp qword ptr [ebp - 8]
  fstp tbyte ptr [edi]
  fstp st(1)
  fild word ptr [ebx]
  fild dword ptr [esi + 4]
  fild qword ptr [edi + 0x18]
  fist word ptr [ebx + 2]
  fist dword ptr [ecx + edx*4]
  fistp word ptr [esi]
  fistp dword ptr [edi + 4]
  fistp qword ptr [ebx + 8]

.irp op, fadd, fmul, fsub, fsubr, fdiv, fdivr
  \op dword ptr [ebx]
  \op qword ptr [esi + edi*8 + 0x20]
  \op st, st(1)
  \op st(2), st
  \op\()p st(1), st
.endr
.irp op, fiadd, fimul, fisub, fisubr, fidiv, fidivr, ficom, ficomp
  \op word ptr [ebx]
  \op dword ptr [esi + 4]
.endr
  fcom dword ptr [ebx]
  fcom qword ptr [esi]
  fcom st(1)
  fcomp dword ptr [edi + 4]
  fcomp qword ptr [ebx + 8]
  fcomp st(2)
  fcompp
  fucom st(1)
  fucomp st(3)
  fucompp
  fcomi st, st(1)
  fcomip st, st(2)
  fucomi st, st(3)
  fucomip st, st(1)
  ftst
  fxam

  fchs
  fabs
  fsqrt
  frndint
  fscale
  fxtract
  fprem
  fprem1
  fsin
  fcos
  fsincos
  fptan
  fpatan
  f2xm1
  fyl2x
  fyl2xp1
  fld1
  fldz
  fldpi
  fldl2t
  fldl2e
  fldlg2
  fldln2
  fxch st(1)
  fxch st(5)
  fcmovb st, st(1)
  fcmove st, st(2)
  fcmovbe st, st(3)
  fcmovu st, st(4)
  fcmovnb st, st(1)
  fcmovne st, st(2)
  fcmovnbe st, st(3)
  fcmovnu st, st(4)
  ffree st(2)
  fincstp
  fdecstp
  fnstsw ax
  fnstsw word ptr [ebx]
  fnstcw word ptr [esi + 2]
  fldcw word ptr [edi]
  fnclex
  fnop
  # FXSAVE and FXRSTOR read and write the whole x87 state: a lifted x87
  # run ends at them.
  fxsave [ebx]
  fxrstor [esi + 0x10]
