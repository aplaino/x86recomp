// The PRX of orbis_process_test (libTestPrx): an export the eboot calls,
// data it reads, general-dynamic TLS, setjmp and longjmp, and an _init the
// loader runs before the eboot's entry point.

int prx_inited;
static __thread int prx_tls = 100;

int prx_add(int a, int b) { return a + b; }
int prx_tls_bump(void) { return ++prx_tls; }

void _init(void) { prx_inited = 1; }

// setjmp and longjmp shaped like the system libc's (which games ship as
// a PRX): longjmp restores rsp from the buffer, stores the return address
// there and returns. The lifter knows them by NID (lifter/orbis_reader.cpp).
int setjmp(long* b) __attribute__((returns_twice));
__attribute__((noreturn)) void longjmp(long* b, int v);
__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".type setjmp, @function\n"
    "setjmp:\n"
    "  movq (%rsp), %rdx\n"
    "  movq %rdx, (%rdi)\n"
    "  movq %rbx, 8(%rdi)\n"
    "  movq %rsp, 16(%rdi)\n"
    "  movq %rbp, 24(%rdi)\n"
    "  movq %r12, 32(%rdi)\n"
    "  movq %r13, 40(%rdi)\n"
    "  movq %r14, 48(%rdi)\n"
    "  movq %r15, 56(%rdi)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".globl longjmp\n"
    ".type longjmp, @function\n"
    "longjmp:\n"
    "  movl %esi, %eax\n"
    "  movq (%rdi), %rcx\n"
    "  movq 8(%rdi), %rbx\n"
    "  movq 16(%rdi), %rsp\n"
    "  movq 24(%rdi), %rbp\n"
    "  movq 32(%rdi), %r12\n"
    "  movq 40(%rdi), %r13\n"
    "  movq 48(%rdi), %r14\n"
    "  movq 56(%rdi), %r15\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  incl %eax\n"
    "1:\n"
    "  movq %rcx, (%rsp)\n"
    "  ret\n");
