# Indirect calls and jumps (recomp_lift --flags-test, second program):
# flags read after them, through a callee that writes none and out of one
# that does, and returned through an indirect tail jump.
.intel_syntax noprefix
.text
.macro WRAP n
t\n: call p\n
    pushfd
    pop dword ptr [0x200F80 + 4*\n]
    ret
.endm

# 1: an indirect call whose callee writes no flag, one whose ZF is read
WRAP 1
p1:  call L1_1
L1_1: pop edx
.att_syntax
     addl $(g_none - L1_1), %edx
.intel_syntax noprefix
     cmp eax, ebx
     call edx
     sbb ch, ch
     call L1_2
L1_2: pop edx
.att_syntax
     addl $(g_test - L1_2), %edx
.intel_syntax noprefix
     call edx
     setz cl
     test ecx, ecx
     ret
g_none:
     lea eax, [eax+1]
     ret
g_test:
     test eax, eax
     ret

# 2: an indirect tail jump, CF read after it
WRAP 2
p2:  call L2_1
L2_1: pop edx
.att_syntax
     addl $(g_test - L2_1), %edx
.intel_syntax noprefix
     cmp eax, ebx
     call g_jmpreg
     setb cl
     test ecx, ecx
     ret
g_jmpreg:
     jmp edx
