// Interrupt vectors for the bare-metal oracle runs (loaded at 0 next to the
// program; vectors.ld places each at its offset). `sc` (0xC00) is how a
// program ends: status 0 and its r3 as the code. Any other interrupt is a
// failure: its vector as the status and SRR0 (where it hit). Then the CPU
// spins; qemu_run.py reads the done area (oracle.h, struct OracleDone).
#define VECTOR(name, off, status) \
  .section name, "ax"; \
  mfspr 4, 26; \
  li 5, status; \
  b report
  .section .report, "ax"
  .globl report
report:
  lis 6, 0x00E0       // ORACLE_DONE
  stw 5, 4(6)
  stw 3, 8(6)
  stw 4, 20(6)
  sync
  lis 7, 0xD0E0       // ORACLE_DONE_MAGIC
  ori 7, 7, 0xD0E0
  stw 7, 0(6)
  sync
1:
  b 1b

VECTOR(.v100, 0x100, 0x100)   // system reset
VECTOR(.v200, 0x200, 0x200)   // machine check
VECTOR(.v300, 0x300, 0x300)   // data storage
VECTOR(.v380, 0x380, 0x380)   // data segment
VECTOR(.v400, 0x400, 0x400)   // instruction storage
VECTOR(.v480, 0x480, 0x480)   // instruction segment
VECTOR(.v500, 0x500, 0x500)   // external
VECTOR(.v600, 0x600, 0x600)   // alignment
VECTOR(.v700, 0x700, 0x700)   // program (illegal instruction, trap)
VECTOR(.v800, 0x800, 0x800)   // floating-point unavailable
VECTOR(.v900, 0x900, 0x900)   // decrementer
VECTOR(.vc00, 0xC00, 0)       // system call: the end of the run
VECTOR(.vd00, 0xD00, 0xD00)   // trace
VECTOR(.vf00, 0xF00, 0xF00)   // performance monitor
VECTOR(.vf20, 0xF20, 0xF20)   // VMX unavailable
