# Programs for tests/lift_flags_test.cpp (recomp_lift --flags-test): flags
# that cross calls, returns, tail calls and indirect calls. Each tN calls
# pN and then stores every flag (pushfd) at [0x200F00 + 4*N]. Functions
# reached through a pointer read no flags at entry (the analysis assumes
# so, tools/lifter/flags.h). Programs with indirect calls end with a test,
# so that only the flags they read after those reach `indirect`.
.intel_syntax noprefix
.text
.macro WRAP n
t\n: call p\n
    pushfd
    pop dword ptr [0x200F00 + 4*\n]
    ret
.endm

# 1: through a callee that writes no flag
WRAP 1
p1: cmp eax, ebx
    call g_noflags
    jb 1f
    mov edx, 1
    ret
1:  mov edx, 2
    ret
g_noflags:
    mov ecx, 5
    lea esi, [esi+4]
    ret

# 2: a callee that reads CF at entry
WRAP 2
p2: add eax, ebx
    call g_adc
    ret
g_adc:
    adc eax, 0
    ret

# 3: flags a callee returns
WRAP 3
p3: call g_test
    setz al
    setp ah
    ret
g_test:
    test eax, eax
    ret

# 4: a tail call into a function that reads flags at entry
WRAP 4
p4: cmp eax, ebx
    jmp g_setb
g_setb:
    setb cl
    seto ch
    ret

# 5: two calls deep
WRAP 5
p5: call g_mid
    jc 1f
    xor eax, eax
1:  ret
g_mid:
    call g_stc
    ret
g_stc:
    stc
    ret

# 6: pushfd at a callee's entry
WRAP 6
p6: cmp eax, ebx
    call g_pushfd
    ret
g_pushfd:
    pushfd
    pop eax
    ret

# 7: DF across calls, both ways
WRAP 7
p7: std
    call g_lods
    cld
    call g_lods
    call g_std
    movsb
    cld
    ret
g_lods:
    lodsb
    ret
g_std:
    std
    ret

# 8: rep cmpsb with ecx maybe 0 (flags unchanged)
WRAP 8
p8: cmp eax, ebx
    repe cmpsb
    call g_noflags2
    setb dl
    sete dh
    ret
g_noflags2:
    lea edi, [edi+1]
    ret

# 9: a shift by cl, maybe 0 (flags unchanged)
WRAP 9
p9: cmp eax, ebx
    shl edx, cl
    call g_noflags2
    setb al
    ret

# 10: indirect calls: flags through one, flags out of another
WRAP 10
p10: call L10_1
L10_1: pop edx
.att_syntax
     addl $(g_noflags2 - L10_1), %edx
.intel_syntax noprefix
     cmp eax, ebx
     call edx
     setb ch
     call L10_2
L10_2: pop edx
.att_syntax
     addl $(g_test - L10_2), %edx
.intel_syntax noprefix
     call edx
     setz cl
     test ecx, ecx
     ret

# 11: a loop with its flags across a call
WRAP 11
p11: and ecx, 3
     inc ecx
1:   dec ecx
     call g_noflags2
     jnz 1b
     ret

# 12: a callee that writes ZF but keeps CF
WRAP 12
p12: cmp eax, ebx
     call g_inc
     setb cl
     setz ch
     ret
g_inc:
     inc edx
     ret

# 13: flags returned through a tail call
WRAP 13
p13: call g_tailtest
     setz cl
     ret
g_tailtest:
     mov eax, ebx
     jmp g_test

# 14: an indirect tail jump
WRAP 14
p14: call L14_1
L14_1: pop edx
.att_syntax
     addl $(g_test - L14_1), %edx
.intel_syntax noprefix
     cmp eax, ebx
     call g_jmpreg
     setb cl
     test ecx, ecx
     ret
g_jmpreg:
     jmp edx

# 15: lahf in a callee; popfd and pushfd around a call
WRAP 15
p15: cmp eax, ebx
     call g_lahf
     push ebx
     popfd
     call g_noflags
     pushfd
     pop edx
     cld
     ret
g_lahf:
     lahf
     ret

# 16: a conditional tail call
WRAP 16
p16: cmp eax, ebx
     jb g_setb
     sbb ecx, ecx
     ret

# 17: a call whose callee returns its caller's flags, then adc chains
WRAP 17
p17: sub eax, ebx
     call g_noflags2
     sbb edx, edx
     call g_adc
     ret
