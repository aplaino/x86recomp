# Instruction forms for lift_isa64_int/lift_isa64_sse (tests/CMakeLists.txt):
# the x86-64 extensions the fixed x64 CPU model (AMD family 16h) has beyond
# what the x64 test images cover. One instruction per line, Intel syntax;
# memory operands are rewritten to [base] by the form extraction anyway.
# Re-encode with tools/x64_forms.py after editing.

# BMI1
andn eax, ebx, ecx
andn rax, rbx, rcx
andn r9d, r10d, dword ptr [rsi]
andn r9, r10, qword ptr [rsi]
bextr eax, ebx, ecx
bextr rax, rbx, rcx
bextr r11d, dword ptr [rsi], r12d
bextr r11, qword ptr [rsi], r12
blsi eax, ebx
blsi rax, rbx
blsi r8d, dword ptr [rsi]
blsr eax, ebx
blsr rax, rbx
blsr r8, qword ptr [rsi]
blsmsk eax, ebx
blsmsk rax, rbx
blsmsk r8d, dword ptr [rsi]
tzcnt ax, bx
tzcnt eax, ebx
tzcnt rax, qword ptr [rsi]

# xchg with a redundant lock prefix
lock xchg byte ptr [rsi], al
lock xchg word ptr [rsi], si
lock xchg dword ptr [rsi], esi
lock xchg qword ptr [rsi], rdx

# POPCNT, LZCNT (ABM)
popcnt ax, bx
popcnt eax, ebx
popcnt rax, rbx
popcnt r9d, dword ptr [rsi]
popcnt r9, qword ptr [rsi]
lzcnt ax, bx
lzcnt r9, qword ptr [rsi]

# MOVBE
movbe ax, word ptr [rsi]
movbe eax, dword ptr [rsi]
movbe rax, qword ptr [rsi]
movbe r10d, dword ptr [rsi]
movbe word ptr [rsi], bx
movbe dword ptr [rsi], ebx
movbe qword ptr [rsi], r11

# Non-temporal store
movnti dword ptr [rsi], eax
movnti qword ptr [rsi], r9

# ---- SSE family: forms new to the model (legacy encodings) ----
pshufb xmm1, xmm2
pshufb xmm9, xmmword ptr [rsi]
pmulhrsw xmm1, xmm2
pmulhrsw xmm9, xmmword ptr [rsi]
pmaddwd xmm1, xmm2
pmaddwd xmm9, xmmword ptr [rsi]
pmaddubsw xmm1, xmm2
pmaddubsw xmm9, xmmword ptr [rsi]
psadbw xmm1, xmm2
psadbw xmm9, xmmword ptr [rsi]
phminposuw xmm1, xmm2
phminposuw xmm9, xmmword ptr [rsi]
pcmpeqq xmm1, xmm2
pcmpeqq xmm9, xmmword ptr [rsi]
pcmpgtq xmm1, xmm2
pcmpgtq xmm9, xmmword ptr [rsi]
pmulhw xmm1, xmm2
pmulhw xmm9, xmmword ptr [rsi]
pmulhuw xmm1, xmm2
pmulhuw xmm9, xmmword ptr [rsi]
pavgb xmm1, xmm2
pavgb xmm9, xmmword ptr [rsi]
pavgw xmm1, xmm2
pavgw xmm9, xmmword ptr [rsi]
paddsb xmm1, xmm2
paddsb xmm9, xmmword ptr [rsi]
paddsw xmm1, xmm2
paddsw xmm9, xmmword ptr [rsi]
paddusb xmm1, xmm2
paddusb xmm9, xmmword ptr [rsi]
paddusw xmm1, xmm2
paddusw xmm9, xmmword ptr [rsi]
psubsb xmm1, xmm2
psubsb xmm9, xmmword ptr [rsi]
psubsw xmm1, xmm2
psubsw xmm9, xmmword ptr [rsi]
psubusb xmm1, xmm2
psubusb xmm9, xmmword ptr [rsi]
psubusw xmm1, xmm2
psubusw xmm9, xmmword ptr [rsi]
phaddw xmm1, xmm2
phaddw xmm9, xmmword ptr [rsi]
phaddd xmm1, xmm2
phaddd xmm9, xmmword ptr [rsi]
phsubw xmm1, xmm2
phsubw xmm9, xmmword ptr [rsi]
phsubd xmm1, xmm2
phsubd xmm9, xmmword ptr [rsi]
haddps xmm1, xmm2
haddps xmm9, xmmword ptr [rsi]
haddpd xmm1, xmm2
haddpd xmm9, xmmword ptr [rsi]
hsubps xmm1, xmm2
hsubps xmm9, xmmword ptr [rsi]
hsubpd xmm1, xmm2
hsubpd xmm9, xmmword ptr [rsi]
addsubps xmm1, xmm2
addsubps xmm9, xmmword ptr [rsi]
addsubpd xmm1, xmm2
addsubpd xmm9, xmmword ptr [rsi]
movsldup xmm1, xmm2
movsldup xmm9, xmmword ptr [rsi]
movshdup xmm1, xmm2
movshdup xmm9, xmmword ptr [rsi]
ptest xmm1, xmm2
ptest xmm9, xmmword ptr [rsi]
aesenc xmm1, xmm2
aesenc xmm9, xmmword ptr [rsi]
aesenclast xmm1, xmm2
aesenclast xmm9, xmmword ptr [rsi]
aesdec xmm1, xmm2
aesdec xmm9, xmmword ptr [rsi]
aesdeclast xmm1, xmm2
aesdeclast xmm9, xmmword ptr [rsi]
aesimc xmm1, xmm2
aesimc xmm9, xmmword ptr [rsi]
movddup xmm1, xmm2
movddup xmm1, qword ptr [rsi]
movntdqa xmm1, xmmword ptr [rsi]
movntdq xmmword ptr [rsi], xmm3
movntps xmmword ptr [rsi], xmm3
movntpd xmmword ptr [rsi], xmm3
pblendvb xmm1, xmm2
pblendvb xmm1, xmmword ptr [rsi]
roundps xmm1, xmm2, 0x0
roundps xmm1, xmm2, 0x1
roundps xmm1, xmm2, 0x2
roundps xmm1, xmm2, 0x3
roundps xmm1, xmm2, 0x4
roundps xmm1, xmm2, 0x8
roundps xmm1, xmm2, 0x9
roundps xmm1, xmm2, 0xb
roundps xmm1, xmm2, 0xc
roundps xmm1, xmmword ptr [rsi], 0x1
roundpd xmm1, xmm2, 0x0
roundpd xmm1, xmm2, 0x1
roundpd xmm1, xmm2, 0x2
roundpd xmm1, xmm2, 0x3
roundpd xmm1, xmm2, 0x4
roundpd xmm1, xmm2, 0xc
roundpd xmm1, xmmword ptr [rsi], 0x1
roundss xmm1, xmm2, 0x0
roundss xmm1, xmm2, 0x1
roundss xmm1, xmm2, 0x2
roundss xmm1, xmm2, 0x3
roundss xmm1, xmm2, 0x4
roundss xmm1, xmm2, 0x8
roundss xmm1, xmm2, 0xc
roundss xmm1, dword ptr [rsi], 0x1
roundsd xmm1, xmm2, 0x0
roundsd xmm1, xmm2, 0x1
roundsd xmm1, xmm2, 0x2
roundsd xmm1, xmm2, 0x3
roundsd xmm1, xmm2, 0x4
roundsd xmm1, xmm2, 0xc
roundsd xmm1, qword ptr [rsi], 0x1
dpps xmm1, xmm2, 0xff
dpps xmm1, xmm2, 0xf1
dpps xmm1, xmm2, 0x31
dpps xmm1, xmm2, 0xa5
dpps xmm1, xmm2, 0x7e
dpps xmm1, xmmword ptr [rsi], 0xf1
dppd xmm1, xmm2, 0x33
dppd xmm1, xmm2, 0x31
dppd xmm1, xmm2, 0x12
dppd xmm1, xmmword ptr [rsi], 0x31
pblendw xmm1, xmm2, 0x0
pblendw xmm1, xmm2, 0xa5
pblendw xmm1, xmm2, 0xff
pblendw xmm1, xmmword ptr [rsi], 0xa5
blendps xmm1, xmm2, 0x5
blendps xmm1, xmm2, 0xa
blendps xmm1, xmmword ptr [rsi], 0xa
blendpd xmm1, xmm2, 0x1
blendpd xmm1, xmm2, 0x2
blendpd xmm1, xmmword ptr [rsi], 0x2
pclmulqdq xmm1, xmm2, 0x0
pclmulqdq xmm1, xmm2, 0x1
pclmulqdq xmm1, xmm2, 0x10
pclmulqdq xmm1, xmm2, 0x11
pclmulqdq xmm1, xmmword ptr [rsi], 0x1
aeskeygenassist xmm1, xmm2, 0x0
aeskeygenassist xmm1, xmm2, 0x1
aeskeygenassist xmm1, xmm2, 0x1b
aeskeygenassist xmm1, xmm2, 0x36
aeskeygenassist xmm1, xmmword ptr [rsi], 0x1
insertps xmm1, xmm2, 0x0
insertps xmm1, xmm2, 0x1d
insertps xmm1, xmm2, 0x6a
insertps xmm1, xmm2, 0xf0
insertps xmm1, xmm2, 0x3f
insertps xmm1, dword ptr [rsi], 0x1d
pextrb eax, xmm2, 0
pextrb eax, xmm2, 1
pextrb eax, xmm2, 15
pextrb byte ptr [rsi], xmm2, 1
pextrd eax, xmm2, 0
pextrd eax, xmm2, 1
pextrd eax, xmm2, 3
pextrd dword ptr [rsi], xmm2, 1
pextrq rax, xmm2, 0
pextrq rax, xmm2, 1
pextrq rax, xmm2, 1
pextrq qword ptr [rsi], xmm2, 1
extractps eax, xmm2, 0
extractps eax, xmm2, 1
extractps eax, xmm2, 3
extractps dword ptr [rsi], xmm2, 1
pinsrb xmm1, ebx, 0
pinsrb xmm1, ebx, 1
pinsrb xmm1, ebx, 3
pinsrb xmm1, byte ptr [rsi], 1
pinsrd xmm1, ebx, 0
pinsrd xmm1, ebx, 1
pinsrd xmm1, ebx, 3
pinsrd xmm1, dword ptr [rsi], 1
pinsrq xmm1, rbx, 0
pinsrq xmm1, rbx, 1
pinsrq xmm1, rbx, 1
pinsrq xmm1, qword ptr [rsi], 1
pcmpestri xmm1, xmm2, 0x0
pcmpistri xmm1, xmm2, 0x0
pcmpestri xmm1, xmm2, 0x1
pcmpistri xmm1, xmm2, 0x1
pcmpestri xmm1, xmm2, 0x2
pcmpistri xmm1, xmm2, 0x2
pcmpestri xmm1, xmm2, 0x3
pcmpistri xmm1, xmm2, 0x3
pcmpestri xmm1, xmm2, 0x4
pcmpistri xmm1, xmm2, 0x4
pcmpestri xmm1, xmm2, 0x8
pcmpistri xmm1, xmm2, 0x8
pcmpestri xmm1, xmm2, 0xc
pcmpistri xmm1, xmm2, 0xc
pcmpestri xmm1, xmm2, 0x10
pcmpistri xmm1, xmm2, 0x10
pcmpestri xmm1, xmm2, 0x30
pcmpistri xmm1, xmm2, 0x30
pcmpestri xmm1, xmm2, 0x40
pcmpistri xmm1, xmm2, 0x40
pcmpestri xmm1, xmm2, 0x44
pcmpistri xmm1, xmm2, 0x44
pcmpestri xmm1, xmm2, 0xd
pcmpistri xmm1, xmm2, 0xd
pcmpestri xmm1, xmm2, 0x1a
pcmpistri xmm1, xmm2, 0x1a
pcmpestri xmm1, xmm2, 0x4c
pcmpistri xmm1, xmm2, 0x4c
pcmpestri xmm1, xmm2, 0x7f
pcmpistri xmm1, xmm2, 0x7f
pcmpestri xmm1, xmmword ptr [rsi], 0x0c
pcmpistri xmm1, xmmword ptr [rsi], 0x0c
rex.w pcmpestri xmm1, xmm2, 0x00

# ---- VEX.128 forms (the bridge): dst = src1, dst = src2, distinct ----
vpaddd xmm1, xmm2, xmm3
vpaddd xmm1, xmm2, xmm1
vpaddd xmm1, xmm1, xmm3
vpaddd xmm9, xmm10, xmmword ptr [rsi]
vpsubw xmm1, xmm2, xmm3
vpsubw xmm1, xmm2, xmm1
vpsubw xmm1, xmm1, xmm3
vpsubw xmm9, xmm10, xmmword ptr [rsi]
vpxor xmm1, xmm2, xmm3
vpxor xmm1, xmm2, xmm1
vpxor xmm1, xmm1, xmm3
vpxor xmm9, xmm10, xmmword ptr [rsi]
vpand xmm1, xmm2, xmm3
vpand xmm1, xmm2, xmm1
vpand xmm1, xmm1, xmm3
vpand xmm9, xmm10, xmmword ptr [rsi]
vpor xmm1, xmm2, xmm3
vpor xmm1, xmm2, xmm1
vpor xmm1, xmm1, xmm3
vpor xmm9, xmm10, xmmword ptr [rsi]
vpandn xmm1, xmm2, xmm3
vpandn xmm1, xmm2, xmm1
vpandn xmm1, xmm1, xmm3
vpandn xmm9, xmm10, xmmword ptr [rsi]
vpshufb xmm1, xmm2, xmm3
vpshufb xmm1, xmm2, xmm1
vpshufb xmm1, xmm1, xmm3
vpshufb xmm9, xmm10, xmmword ptr [rsi]
vpmulld xmm1, xmm2, xmm3
vpmulld xmm1, xmm2, xmm1
vpmulld xmm1, xmm1, xmm3
vpmulld xmm9, xmm10, xmmword ptr [rsi]
vpmullw xmm1, xmm2, xmm3
vpmullw xmm1, xmm2, xmm1
vpmullw xmm1, xmm1, xmm3
vpmullw xmm9, xmm10, xmmword ptr [rsi]
vpmulhuw xmm1, xmm2, xmm3
vpmulhuw xmm1, xmm2, xmm1
vpmulhuw xmm1, xmm1, xmm3
vpmulhuw xmm9, xmm10, xmmword ptr [rsi]
vpmuludq xmm1, xmm2, xmm3
vpmuludq xmm1, xmm2, xmm1
vpmuludq xmm1, xmm1, xmm3
vpmuludq xmm9, xmm10, xmmword ptr [rsi]
vpmaddwd xmm1, xmm2, xmm3
vpmaddwd xmm1, xmm2, xmm1
vpmaddwd xmm1, xmm1, xmm3
vpmaddwd xmm9, xmm10, xmmword ptr [rsi]
vpcmpeqd xmm1, xmm2, xmm3
vpcmpeqd xmm1, xmm2, xmm1
vpcmpeqd xmm1, xmm1, xmm3
vpcmpeqd xmm9, xmm10, xmmword ptr [rsi]
vpcmpgtq xmm1, xmm2, xmm3
vpcmpgtq xmm1, xmm2, xmm1
vpcmpgtq xmm1, xmm1, xmm3
vpcmpgtq xmm9, xmm10, xmmword ptr [rsi]
vpminud xmm1, xmm2, xmm3
vpminud xmm1, xmm2, xmm1
vpminud xmm1, xmm1, xmm3
vpminud xmm9, xmm10, xmmword ptr [rsi]
vpmaxsd xmm1, xmm2, xmm3
vpmaxsd xmm1, xmm2, xmm1
vpmaxsd xmm1, xmm1, xmm3
vpmaxsd xmm9, xmm10, xmmword ptr [rsi]
vpunpcklbw xmm1, xmm2, xmm3
vpunpcklbw xmm1, xmm2, xmm1
vpunpcklbw xmm1, xmm1, xmm3
vpunpcklbw xmm9, xmm10, xmmword ptr [rsi]
vpunpckhqdq xmm1, xmm2, xmm3
vpunpckhqdq xmm1, xmm2, xmm1
vpunpckhqdq xmm1, xmm1, xmm3
vpunpckhqdq xmm9, xmm10, xmmword ptr [rsi]
vpackuswb xmm1, xmm2, xmm3
vpackuswb xmm1, xmm2, xmm1
vpackuswb xmm1, xmm1, xmm3
vpackuswb xmm9, xmm10, xmmword ptr [rsi]
vpackssdw xmm1, xmm2, xmm3
vpackssdw xmm1, xmm2, xmm1
vpackssdw xmm1, xmm1, xmm3
vpackssdw xmm9, xmm10, xmmword ptr [rsi]
vpavgb xmm1, xmm2, xmm3
vpavgb xmm1, xmm2, xmm1
vpavgb xmm1, xmm1, xmm3
vpavgb xmm9, xmm10, xmmword ptr [rsi]
vpsadbw xmm1, xmm2, xmm3
vpsadbw xmm1, xmm2, xmm1
vpsadbw xmm1, xmm1, xmm3
vpsadbw xmm9, xmm10, xmmword ptr [rsi]
vphaddd xmm1, xmm2, xmm3
vphaddd xmm1, xmm2, xmm1
vphaddd xmm1, xmm1, xmm3
vphaddd xmm9, xmm10, xmmword ptr [rsi]
vpaddusw xmm1, xmm2, xmm3
vpaddusw xmm1, xmm2, xmm1
vpaddusw xmm1, xmm1, xmm3
vpaddusw xmm9, xmm10, xmmword ptr [rsi]
vpsubsb xmm1, xmm2, xmm3
vpsubsb xmm1, xmm2, xmm1
vpsubsb xmm1, xmm1, xmm3
vpsubsb xmm9, xmm10, xmmword ptr [rsi]
vaddps xmm1, xmm2, xmm3
vaddps xmm1, xmm2, xmm1
vaddps xmm1, xmm1, xmm3
vaddps xmm9, xmm10, xmmword ptr [rsi]
vsubpd xmm1, xmm2, xmm3
vsubpd xmm1, xmm2, xmm1
vsubpd xmm1, xmm1, xmm3
vsubpd xmm9, xmm10, xmmword ptr [rsi]
vmulss xmm1, xmm2, xmm3
vmulss xmm1, xmm2, xmm1
vmulss xmm1, xmm1, xmm3
vmulss xmm9, xmm10, dword ptr [rsi]
vdivsd xmm1, xmm2, xmm3
vdivsd xmm1, xmm2, xmm1
vdivsd xmm1, xmm1, xmm3
vdivsd xmm9, xmm10, qword ptr [rsi]
vminps xmm1, xmm2, xmm3
vminps xmm1, xmm2, xmm1
vminps xmm1, xmm1, xmm3
vminps xmm9, xmm10, xmmword ptr [rsi]
vmaxsd xmm1, xmm2, xmm3
vmaxsd xmm1, xmm2, xmm1
vmaxsd xmm1, xmm1, xmm3
vmaxsd xmm9, xmm10, qword ptr [rsi]
vandps xmm1, xmm2, xmm3
vandps xmm1, xmm2, xmm1
vandps xmm1, xmm1, xmm3
vandps xmm9, xmm10, xmmword ptr [rsi]
vxorpd xmm1, xmm2, xmm3
vxorpd xmm1, xmm2, xmm1
vxorpd xmm1, xmm1, xmm3
vxorpd xmm9, xmm10, xmmword ptr [rsi]
vunpcklps xmm1, xmm2, xmm3
vunpcklps xmm1, xmm2, xmm1
vunpcklps xmm1, xmm1, xmm3
vunpcklps xmm9, xmm10, xmmword ptr [rsi]
vunpckhpd xmm1, xmm2, xmm3
vunpckhpd xmm1, xmm2, xmm1
vunpckhpd xmm1, xmm1, xmm3
vunpckhpd xmm9, xmm10, xmmword ptr [rsi]
vhaddps xmm1, xmm2, xmm3
vhaddps xmm1, xmm2, xmm1
vhaddps xmm1, xmm1, xmm3
vhaddps xmm9, xmm10, xmmword ptr [rsi]
vhsubpd xmm1, xmm2, xmm3
vhsubpd xmm1, xmm2, xmm1
vhsubpd xmm1, xmm1, xmm3
vhsubpd xmm9, xmm10, xmmword ptr [rsi]
vaddsubps xmm1, xmm2, xmm3
vaddsubps xmm1, xmm2, xmm1
vaddsubps xmm1, xmm1, xmm3
vaddsubps xmm9, xmm10, xmmword ptr [rsi]
vsqrtss xmm1, xmm2, xmm3
vsqrtss xmm1, xmm2, xmm1
vsqrtss xmm1, xmm1, xmm3
vsqrtss xmm9, xmm10, dword ptr [rsi]
vsqrtsd xmm1, xmm2, xmm3
vsqrtsd xmm1, xmm2, xmm1
vsqrtsd xmm1, xmm1, xmm3
vsqrtsd xmm9, xmm10, qword ptr [rsi]
vcvtss2sd xmm1, xmm2, xmm3
vcvtss2sd xmm1, xmm2, xmm1
vcvtss2sd xmm1, xmm1, xmm3
vcvtss2sd xmm9, xmm10, dword ptr [rsi]
vcvtsd2ss xmm1, xmm2, xmm3
vcvtsd2ss xmm1, xmm2, xmm1
vcvtsd2ss xmm1, xmm1, xmm3
vcvtsd2ss xmm9, xmm10, qword ptr [rsi]
vpsllw xmm1, xmm2, xmm3
vpsllw xmm1, xmm2, xmm1
vpsllw xmm1, xmm1, xmm3
vpsllw xmm9, xmm10, xmmword ptr [rsi]
vpsrld xmm1, xmm2, xmm3
vpsrld xmm1, xmm2, xmm1
vpsrld xmm1, xmm1, xmm3
vpsrld xmm9, xmm10, xmmword ptr [rsi]
vpsraw xmm1, xmm2, xmm3
vpsraw xmm1, xmm2, xmm1
vpsraw xmm1, xmm1, xmm3
vpsraw xmm9, xmm10, xmmword ptr [rsi]
vmovss xmm1, xmm2, xmm3
vmovss xmm1, xmm2, xmm1
vmovss xmm1, xmm1, xmm3
vmovsd xmm1, xmm2, xmm3
vmovsd xmm1, xmm2, xmm1
vmovsd xmm1, xmm1, xmm3
vmovlhps xmm1, xmm2, xmm3
vmovlhps xmm1, xmm2, xmm1
vmovlhps xmm1, xmm1, xmm3
vmovhlps xmm1, xmm2, xmm3
vmovhlps xmm1, xmm2, xmm1
vmovhlps xmm1, xmm1, xmm3
vaesenc xmm1, xmm2, xmm3
vaesenc xmm1, xmm2, xmm1
vaesenc xmm1, xmm1, xmm3
vaesenc xmm9, xmm10, xmmword ptr [rsi]
vaesdeclast xmm1, xmm2, xmm3
vaesdeclast xmm1, xmm2, xmm1
vaesdeclast xmm1, xmm1, xmm3
vaesdeclast xmm9, xmm10, xmmword ptr [rsi]
vmovaps xmm1, xmm2
vmovaps xmm9, xmmword ptr [rsi]
vmovups xmm1, xmm2
vmovups xmm9, xmmword ptr [rsi]
vmovdqa xmm1, xmm2
vmovdqa xmm9, xmmword ptr [rsi]
vmovdqu xmm1, xmm2
vmovdqu xmm9, xmmword ptr [rsi]
vpshufd xmm1, xmm2, 0x1b
vpshufd xmm9, xmmword ptr [rsi], 0x1b
vpabsw xmm1, xmm2
vpabsw xmm9, xmmword ptr [rsi]
vpmovzxbw xmm1, xmm2
vpmovzxbw xmm9, qword ptr [rsi]
vpmovsxwd xmm1, xmm2
vpmovsxwd xmm9, qword ptr [rsi]
vmovddup xmm1, xmm2
vmovddup xmm9, qword ptr [rsi]
vmovshdup xmm1, xmm2
vmovshdup xmm9, xmmword ptr [rsi]
vcvtdq2ps xmm1, xmm2
vcvtdq2ps xmm9, xmmword ptr [rsi]
vcvttps2dq xmm1, xmm2
vcvttps2dq xmm9, xmmword ptr [rsi]
vcvtps2pd xmm1, xmm2
vcvtps2pd xmm9, qword ptr [rsi]
vcvtdq2pd xmm1, xmm2
vcvtdq2pd xmm9, qword ptr [rsi]
vcvttpd2dq xmm1, xmm2
vcvttpd2dq xmm9, xmmword ptr [rsi]
vptest xmm1, xmm2
vptest xmm9, xmmword ptr [rsi]
vucomiss xmm1, xmm2
vucomiss xmm9, dword ptr [rsi]
vucomisd xmm1, xmm2
vucomisd xmm9, qword ptr [rsi]
vcomiss xmm1, xmm2
vcomiss xmm9, dword ptr [rsi]
vphminposuw xmm1, xmm2
vphminposuw xmm9, xmmword ptr [rsi]
vaesimc xmm1, xmm2
vaesimc xmm9, xmmword ptr [rsi]
vsqrtps xmm1, xmm2
vsqrtps xmm9, xmmword ptr [rsi]
vroundps xmm1, xmm2, 0x3
vroundps xmm9, xmmword ptr [rsi], 0x3
vmovaps xmmword ptr [rsi], xmm2
vmovdqu xmmword ptr [rsi], xmm2
vmovss dword ptr [rsi], xmm2
vmovsd xmm1, qword ptr [rsi]
vmovd xmm1, ebx
vmovd eax, xmm2
vmovq xmm1, rbx
vmovq rax, xmm2
vmovq xmm1, xmm2
vmovq xmm1, qword ptr [rsi]
vmovq qword ptr [rsi], xmm2
vmovlps xmm1, xmm2, qword ptr [rsi]
vmovhps xmm1, xmm2, qword ptr [rsi]
vmovlps qword ptr [rsi], xmm2
vmovhpd qword ptr [rsi], xmm2
vcvtsi2ss xmm1, xmm2, ebx
vcvtsi2sd xmm1, xmm2, rbx
vcvtsi2ss xmm1, xmm2, dword ptr [rsi]
vcvttss2si eax, xmm2
vcvttsd2si rax, xmm2
vcvtss2si eax, xmm2
vpsrldq xmm1, xmm2, 0
vpslld xmm1, xmm2, 0
vpsrlq xmm1, xmm1, 0
vpsrldq xmm1, xmm2, 1
vpslld xmm1, xmm2, 1
vpsrlq xmm1, xmm1, 1
vpsrldq xmm1, xmm2, 11
vpslld xmm1, xmm2, 11
vpsrlq xmm1, xmm1, 11
vpinsrd xmm1, xmm2, ebx, 2
vpinsrb xmm1, xmm2, ebx, 9
vpinsrq xmm1, xmm2, qword ptr [rsi], 1
vpextrb eax, xmm2, 5
vpextrd dword ptr [rsi], xmm2, 3
vpextrq rax, xmm2, 1
vextractps eax, xmm2, 2
vinsertps xmm1, xmm2, xmm3, 0x5c
vinsertps xmm1, xmm2, dword ptr [rsi], 0x20
vpblendw xmm1, xmm2, xmm3, 0x5a
vblendps xmm1, xmm2, xmm1, 0x6
vpalignr xmm1, xmm2, xmm3, 5
vroundss xmm1, xmm2, xmm3, 0x9
vroundsd xmm1, xmm2, qword ptr [rsi], 0x1
vdpps xmm1, xmm2, xmm3, 0xf1
vdppd xmm1, xmm2, xmm3, 0x31
vshufps xmm1, xmm2, xmm3, 0x4e
vpclmulqdq xmm1, xmm2, xmm3, 0x11
vpclmulqdq xmm1, xmm2, xmmword ptr [rsi], 0x01
vaeskeygenassist xmm1, xmm2, 0x8
vcmpps xmm1, xmm2, xmm3, 0
vcmpps xmm1, xmm2, xmm3, 1
vcmpps xmm1, xmm2, xmm3, 2
vcmpps xmm1, xmm2, xmm3, 3
vcmpps xmm1, xmm2, xmm3, 4
vcmpps xmm1, xmm2, xmm3, 5
vcmpps xmm1, xmm2, xmm3, 6
vcmpps xmm1, xmm2, xmm3, 7
vcmpps xmm1, xmm2, xmm3, 8
vcmpps xmm1, xmm2, xmm3, 9
vcmpps xmm1, xmm2, xmm3, 10
vcmpps xmm1, xmm2, xmm3, 11
vcmpps xmm1, xmm2, xmm3, 12
vcmpps xmm1, xmm2, xmm3, 13
vcmpps xmm1, xmm2, xmm3, 14
vcmpps xmm1, xmm2, xmm3, 15
vcmpps xmm1, xmm2, xmm3, 16
vcmpps xmm1, xmm2, xmm3, 17
vcmpps xmm1, xmm2, xmm3, 18
vcmpps xmm1, xmm2, xmm3, 19
vcmpps xmm1, xmm2, xmm3, 20
vcmpps xmm1, xmm2, xmm3, 21
vcmpps xmm1, xmm2, xmm3, 22
vcmpps xmm1, xmm2, xmm3, 23
vcmpps xmm1, xmm2, xmm3, 24
vcmpps xmm1, xmm2, xmm3, 25
vcmpps xmm1, xmm2, xmm3, 26
vcmpps xmm1, xmm2, xmm3, 27
vcmpps xmm1, xmm2, xmm3, 28
vcmpps xmm1, xmm2, xmm3, 29
vcmpps xmm1, xmm2, xmm3, 30
vcmpps xmm1, xmm2, xmm3, 31
vcmpss xmm1, xmm2, xmm3, 0
vcmppd xmm1, xmm2, xmm3, 0
vcmpsd xmm1, xmm2, qword ptr [rsi], 0
vcmpss xmm1, xmm2, xmm3, 1
vcmppd xmm1, xmm2, xmm3, 1
vcmpsd xmm1, xmm2, qword ptr [rsi], 1
vcmpss xmm1, xmm2, xmm3, 8
vcmppd xmm1, xmm2, xmm3, 8
vcmpsd xmm1, xmm2, qword ptr [rsi], 8
vcmpss xmm1, xmm2, xmm3, 13
vcmppd xmm1, xmm2, xmm3, 13
vcmpsd xmm1, xmm2, qword ptr [rsi], 13
vcmpss xmm1, xmm2, xmm3, 15
vcmppd xmm1, xmm2, xmm3, 15
vcmpsd xmm1, xmm2, qword ptr [rsi], 15
vcmpss xmm1, xmm2, xmm3, 17
vcmppd xmm1, xmm2, xmm3, 17
vcmpsd xmm1, xmm2, qword ptr [rsi], 17
vcmpss xmm1, xmm2, xmm3, 25
vcmppd xmm1, xmm2, xmm3, 25
vcmpsd xmm1, xmm2, qword ptr [rsi], 25
vcmpss xmm1, xmm2, xmm3, 31
vcmppd xmm1, xmm2, xmm3, 31
vcmpsd xmm1, xmm2, qword ptr [rsi], 31
cmpps xmm1, xmm2, 0
cmpps xmm1, xmm2, 1
cmpps xmm1, xmm2, 2
cmpps xmm1, xmm2, 3
vpcmpestri xmm1, xmm2, 0x0c
vpcmpistri xmm1, xmm2, 0x44
vpcmpistri xmm1, xmmword ptr [rsi], 0x1a
vblendvps xmm1, xmm2, xmm3, xmm4
vblendvpd xmm1, xmm2, xmmword ptr [rsi], xmm4
vpblendvb xmm1, xmm2, xmm3, xmm1
vmaskmovps xmm1, xmm2, xmmword ptr [rsi]
vmaskmovps xmmword ptr [rsi], xmm2, xmm3
vmaskmovpd xmm1, xmm2, xmmword ptr [rsi]
vpermilpd xmm1, xmm2, 0x1
vpermilpd xmm1, xmmword ptr [rsi], 0x2
vpermilps xmm1, xmm2, 0x1b

# ---- VEX.256 (AVX float) ----
vaddps ymm1, ymm2, ymm3
vaddps ymm1, ymm2, ymm1
vaddps ymm9, ymm10, ymmword ptr [rsi]
vmulpd ymm1, ymm2, ymm3
vmulpd ymm1, ymm2, ymm1
vmulpd ymm9, ymm10, ymmword ptr [rsi]
vminps ymm1, ymm2, ymm3
vminps ymm1, ymm2, ymm1
vminps ymm9, ymm10, ymmword ptr [rsi]
vmaxpd ymm1, ymm2, ymm3
vmaxpd ymm1, ymm2, ymm1
vmaxpd ymm9, ymm10, ymmword ptr [rsi]
vandnps ymm1, ymm2, ymm3
vandnps ymm1, ymm2, ymm1
vandnps ymm9, ymm10, ymmword ptr [rsi]
vxorps ymm1, ymm2, ymm3
vxorps ymm1, ymm2, ymm1
vxorps ymm9, ymm10, ymmword ptr [rsi]
vunpcklps ymm1, ymm2, ymm3
vunpcklps ymm1, ymm2, ymm1
vunpcklps ymm9, ymm10, ymmword ptr [rsi]
vunpckhpd ymm1, ymm2, ymm3
vunpckhpd ymm1, ymm2, ymm1
vunpckhpd ymm9, ymm10, ymmword ptr [rsi]
vhaddps ymm1, ymm2, ymm3
vhaddps ymm1, ymm2, ymm1
vhaddps ymm9, ymm10, ymmword ptr [rsi]
vhsubpd ymm1, ymm2, ymm3
vhsubpd ymm1, ymm2, ymm1
vhsubpd ymm9, ymm10, ymmword ptr [rsi]
vaddsubpd ymm1, ymm2, ymm3
vaddsubpd ymm1, ymm2, ymm1
vaddsubpd ymm9, ymm10, ymmword ptr [rsi]
vmovsldup ymm1, ymm2
vmovsldup ymm1, ymmword ptr [rsi]
vmovshdup ymm1, ymm2
vmovshdup ymm1, ymmword ptr [rsi]
vmovddup ymm1, ymm2
vmovddup ymm1, ymmword ptr [rsi]
vsqrtps ymm1, ymm2
vsqrtps ymm1, ymmword ptr [rsi]
vcvtdq2ps ymm1, ymm2
vcvtdq2ps ymm1, ymmword ptr [rsi]
vcvttps2dq ymm1, ymm2
vcvttps2dq ymm1, ymmword ptr [rsi]
vcvtps2dq ymm1, ymm2
vcvtps2dq ymm1, ymmword ptr [rsi]
vroundps ymm1, ymm2, 0x2
vroundpd ymm1, ymmword ptr [rsi], 0x1
vblendps ymm1, ymm2, ymm3, 0xa5
vblendpd ymm1, ymm2, ymm3, 0x9
vshufpd ymm1, ymm2, ymm3, 0x6
vshufps ymm1, ymm2, ymm3, 0x93
vdpps ymm1, ymm2, ymm3, 0xf3
vcmpps ymm1, ymm2, ymm3, 0x1d
vcmppd ymm1, ymm2, ymmword ptr [rsi], 0x2
vblendvps ymm1, ymm2, ymm3, ymm4
vblendvpd ymm1, ymm2, ymmword ptr [rsi], ymm4
vmaskmovps ymm1, ymm2, ymmword ptr [rsi]
vmaskmovpd ymmword ptr [rsi], ymm2, ymm3
vptest ymm1, ymm2
vptest ymm1, ymmword ptr [rsi]
vpermilpd ymm1, ymm2, 0x5

# ---- the rest of SSE3, SSSE3, SSE4.1, SSE4.2 ----
phaddsw xmm1, xmm2
phaddsw xmm9, xmmword ptr [rsi]
phsubsw xmm1, xmm2
phsubsw xmm9, xmmword ptr [rsi]
psignb xmm1, xmm2
psignb xmm9, xmmword ptr [rsi]
psignw xmm1, xmm2
psignw xmm9, xmmword ptr [rsi]
psignd xmm1, xmm2
psignd xmm9, xmmword ptr [rsi]
mpsadbw xmm1, xmm2, 0
mpsadbw xmm1, xmm2, 1
mpsadbw xmm1, xmm2, 2
mpsadbw xmm1, xmm2, 3
mpsadbw xmm1, xmm2, 4
mpsadbw xmm1, xmm2, 5
mpsadbw xmm1, xmm2, 7
mpsadbw xmm1, xmmword ptr [rsi], 6
lddqu xmm1, xmmword ptr [rsi]
vlddqu xmm1, xmmword ptr [rsi]
crc32 eax, bl
crc32 eax, bx
crc32 eax, ebx
crc32 rax, bl
crc32 rax, rbx
crc32 r9d, dword ptr [rsi]
crc32 r9, qword ptr [rsi]
pcmpestrm xmm1, xmm2, 0x0
pcmpistrm xmm1, xmm2, 0x0
pcmpestrm xmm1, xmm2, 0x8
pcmpistrm xmm1, xmm2, 0x8
pcmpestrm xmm1, xmm2, 0xc
pcmpistrm xmm1, xmm2, 0xc
pcmpestrm xmm1, xmm2, 0x40
pcmpistrm xmm1, xmm2, 0x40
pcmpestrm xmm1, xmm2, 0x44
pcmpistrm xmm1, xmm2, 0x44
pcmpestrm xmm1, xmm2, 0x49
pcmpistrm xmm1, xmm2, 0x49
pcmpestrm xmm1, xmm2, 0x1a
pcmpistrm xmm1, xmm2, 0x1a
pcmpestrm xmm1, xmm2, 0x5d
pcmpistrm xmm1, xmm2, 0x5d
pcmpistrm xmm1, xmmword ptr [rsi], 0x40
vpcmpestrm xmm1, xmm2, 0x4c
vpcmpistrm xmm3, xmm2, 0x08
vphaddsw xmm1, xmm2, xmm3
vpsignd xmm1, xmm2, xmm1
vmpsadbw xmm1, xmm2, xmm3, 5
