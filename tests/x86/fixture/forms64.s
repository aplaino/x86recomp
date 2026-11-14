# The x64 fixture of lift_fixture_int64, lift_fixture_sse64 and
# lift_fixture_x87_64 (tests/x86/CMakeLists.txt): integer, SSE and x87
# instruction forms of 64-bit code, built like forms.s (clang for x86-64,
# LLD, flat code) and run by lift_x64_test.cpp and lift_x87_test.cpp.
# Besides the common forms it has what x64 code addresses differently:
# RIP- and EIP-relative operands, fs: and gs: (with RIP too),
# address-size prefixes on string instructions and XLAT, push and pop of
# 64- and 16-bit operands, and FXSAVE/FXRSTOR among x87 instructions.

.intel_syntax noprefix
.text

# ---- integer -----------------------------------------------------------

.irp op, add, or, adc, sbb, and, sub, xor, cmp
  \op rax, rcx
  \op r9, r10
  \op r11d, eax
  \op sil, r8b
  \op rdx, -7
  \op r12, 0x12345678
  \op rax, qword ptr [rbx]
  \op r13d, dword ptr [rsi + r8*4 + 0x10]
  \op qword ptr [r14 + 0x20], rdi
  \op byte ptr [rcx + rdx*2], 0x7F
  \op dword ptr [r9*8 + 0x100], -3
  \op rax, qword ptr [rip + 0x40]
  \op dword ptr [rip + 0x80], r10d
.endr

  mov rax, 0x123456789ABCDEF0
  mov r15, rcx
  mov r8d, dword ptr [rsi]
  mov qword ptr [rdi + r11*8 - 8], r12
  mov eax, dword ptr [rip + 0x100]
  mov qword ptr [rip - 0x40], rbx
  mov rax, qword ptr gs:[0x30]
  mov ecx, dword ptr fs:[rbx + 8]
  mov rdx, qword ptr gs:[rip + 0x10]
  mov qword ptr fs:[rdi*2 + 0x20], rsi
  mov eax, dword ptr [eip + 0x40]
  add dword ptr [eip - 0x10], ecx
  mov rdx, qword ptr gs:[eip + 0x18]
  mov ecx, dword ptr [ebx + esi*4]
  movabs rax, qword ptr [0x201000]
  movsxd rax, ecx
  movsxd r9, dword ptr [rbx + 4]
  movzx r10, word ptr [rsi]
  lea rax, [rbx + rcx*8 + 0x18]
  lea r11d, [r12 + r13]
.irp op, rol, ror, shl, shr, sar
  \op rax, cl
  \op r9, 13
  \op qword ptr [rbx], 1
.endr
.irp op, mul, imul, div, idiv, neg, not
  \op rcx
  \op qword ptr [rsi + 8]
.endr
  imul r8, r9, 1000
  cqo
  cdqe
  bswap r14
  xchg rax, r9
  cmpxchg qword ptr [rbx], rcx
  xadd qword ptr [rdi], rdx
  bt qword ptr [rbx], 40
  bsf r10, r11

  push rax
  push r12
  push ax
  push r9w
  pop rbx
  pop r13
  pop dx
  pop r10w
  push qword ptr [rbx]
  push qword ptr [rip + 0x30]
  push word ptr [rsi + 2]
  pop qword ptr [rdi + 8]
  pop word ptr [rbx]
  push 0x12
  push -0x12345678
.att_syntax
  pushw $0x1234
.intel_syntax noprefix

  movsb
  rep movsq
  rep stosd
  lodsq
  repne scasb
  repe cmpsw
  movsb byte ptr es:[rdi], byte ptr fs:[rsi]
  lodsd eax, dword ptr gs:[rsi]
  rep movsb byte ptr es:[edi], byte ptr [esi]
  rep stosd dword ptr es:[edi], eax
  lodsb al, byte ptr gs:[esi]
  repe cmpsb byte ptr [esi], byte ptr es:[edi]
  xlatb
  # xlat gs:[rbx + al], and [ebx + al] (address-size prefix), with fs: too
  # (the assembler takes no override on xlat).
  .byte 0x65, 0xD7
  .byte 0x67, 0xD7
  .byte 0x67, 0x64, 0xD7

# ---- SSE ---------------------------------------------------------------

  addps xmm0, xmm9
  mulsd xmm10, qword ptr [rbx]
  movaps xmm11, xmmword ptr [rip + 0x60]
  movdqu xmmword ptr [r8 + rcx*2 + 1], xmm12
  movss xmm1, dword ptr gs:[rdi]
  movsd qword ptr [rip + 0x20], xmm2
  cvtsi2sd xmm3, rax
  cvttsd2si r9, xmm4
  cvtsd2si rax, qword ptr [rip + 0x18]
  cvtsd2si rcx, qword ptr [eip + 0x18]
  pxor xmm13, xmmword ptr [rsi]
  movq rax, xmm5
  movq xmm6, rcx
  pshufd xmm7, xmmword ptr fs:[rbx + 0x10], 0x1B
  ucomisd xmm8, xmm14

# ---- x87 ---------------------------------------------------------------

  fld dword ptr [rbx]
  fld qword ptr [rsi + rdi*8 + 0x10]
  fld tbyte ptr [rip + 0x40]
  fld st(2)
  fstp qword ptr [rdi]
  fst dword ptr gs:[rcx + 4]
  fild dword ptr [rbx + 8]
  fistp qword ptr [rsi]
  fadd qword ptr [rbx]
  fmul st, st(1)
  fsubp st(1), st
  fdiv dword ptr [rdx + rax*4]
  fcom qword ptr [rsi]
  fucompp
  fxch st(3)
  fchs
  fsqrt
  fld1
  fldpi
  fnstsw ax
  fnstcw word ptr [rdi + 2]
  # FXSAVE and FXRSTOR read and write the whole x87 state: a lifted x87
  # run ends at them.
  fxsave [rbx]
  fxrstor [rsi + 0x20]
  fxsave64 [rdi]
  fxrstor64 [rbx]
