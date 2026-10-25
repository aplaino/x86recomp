# Architecture: the Xbox 360 platform module

This module turns an Xbox 360 title into a native 64-bit program: the
title's PowerPC machine code is lifted to C++ ahead of time and linked
with a runtime that reimplements the kernel (xboxkrnl), the system
library (xam) and the GPU (Xenos) it calls. Nothing here is specific to
one game: game projects hold that (manifest, symbols, game hooks) and use
this tree as a library.

It is a console platform on x86recomp's shared core (the top-level
ARCHITECTURE.md, "Console platforms"), built only with
`RECOMP_PLATFORM_XBOX360`:
- Core, shared by every PowerPC guest: the lifter `recomp_lift_ppc`
  (`tools/lifter_ppc`: decoder, function discovery, jump tables, the
  emitter) and the CPU model and dispatcher it targets (`src/ppc`, library
  `recomp_ppc`). The lifter knows no executable format; the platform
  registers an image reader for its own.
- Platform, here: XEX2 and the lifter's reader for it, LZX, the tools
  around the format, and (to come) the guest address layout, process
  bring-up, the kernel and xam HLE and the Xenos front end (libraries
  `recomp_xbox360_*`), with their tests and docs.
Paths below are relative to `platforms/xbox360` unless they start with
`src/ppc` or `tools/lifter_ppc`. An Xbox 360 game project turns
`RECOMP_X86` off and keeps the host layer every guest shares.

The module keeps the x86 path's and the Switch's shape: one C++ function
per guest function, registers cached in locals, a dispatcher for indirect
calls, an HLE registry that is fatal on anything unhandled, exact
arithmetic as the reference for hand-written replacements, and a game
project that owns everything game-specific.

## Goals and hard constraints
1. Native Linux and macOS (Apple Silicon, Metal); lifted code and the
   runtime are portable C++ with no inline assembly.
2. No emulation in the shipping binary: no interpreter, no JIT, no
   runtime PowerPC decoding. An indirect branch to an address that was
   not lifted is a fatal error naming the VA.
3. No GPL code in the product. Xenia and ReXGlue are BSD-3-Clause: they
   may be read closely and code copied from them keeps its notice (in the
   file and in NOTICE). The PowerPC disassembler in binutils (GPL) is
   never used; the decoder here is written from IBM's Power ISA.
4. Users bring their own game: no game code, assets, keys or firmware in
   any repository. The lifter and the loader read unencrypted XEX files
   only; `xbox360_xex_decrypt` makes one from an encrypted XEX with a key
   file the owner supplies.

## CPU: Xenon in 32-bit mode
Titles run on a 64-bit PowerPC in 32-bit mode (MSR[SF] = 0), as their code
shows: addresses above 2 GiB are built with `lis` (so the registers hold
their sign-extended forms) and used directly. In 32-bit mode
(ppc/lifted.h):
- registers and results are 64-bit, and 64-bit instructions (`ld`,
  `mulld`, `rldicl`, `srad` ...) work on all 64 bits;
- the low 32 bits decide CR0 of record forms, CA and OV of adds and
  subtracts (`addc`, `adde`, `addic`, `subfic`, `neg` ...), the CTR test of
  conditional branches, branch targets from LR and CTR, and effective
  addresses (the update forms write the 32-bit address back);
- OV of multiplies and divides follows the operation's own width, and the
  64-bit shifts' CA is 64-bit, in either mode.

The context (ppc/ctx.h) holds the GPRs, LR, CTR, CR as MFCR reads it, XER
as MFXER reads it (only SO, OV, CA and the byte count exist), MSR as last
written, VRSAVE, the guest arena base and the reservation of
LWARX/STWCX., a compare-and-swap as QEMU models it. Guest memory is a
4 GiB arena: guest address a is at `mem + a`, big-endian (every load and
store swaps). The base is chosen at startup.

Where the ISA leaves a result undefined the lifter picks one, and the
tests do not compare it: the quotient of a divide by zero or of the most
negative value by -1 is 0 (CR0 of the record form from that 0), and
MULHW(U)'s and DIVW(U)'s high words are 0. These wait for a check on a
console. DCBZ zeroes 32 bytes and the Xenon's DCBZ128 128.

### Floating point
The FPU (src/ppc/fp.cpp) is exact: Berkeley SoftFloat under FPSCR[RN] with
tininess detected before rounding, and the PowerPC's own rules on top:
- NaNs: the first NaN of FRA, FRB, FRC (in that order, whatever the
  operation's operand order), quieted; the default NaN 0x7FF8000000000000
  for invalid operations; FNMADD and FNMSUB do not negate a NaN.
- Single-precision arithmetic rounds the exact result of its double
  operands once, to single precision, as the ISA defines it for any
  operands: the operation runs on float128 with round-to-odd, then one
  rounding to f32 under RN. A NaN result in single precision keeps only
  single's fraction.
- FPSCR: the exception bits with each invalid operation's cause (VXSNAN
  VXISI VXIDI VXZDZ VXIMZ VXVC VXSQRT VXCVI), FX when an exception bit
  goes from 0 to 1, the VX and FEX summaries, FI, FR (the rounding
  increased the magnitude: the result against the same operation rounded
  toward zero) and FPRF. MTFSF and MTFSFI do not write the reserved bit 20.
- LFS and STFS convert by the ISA's bit-level algorithms (no rounding,
  SNaNs kept).
- An exception whose enable bit is set, and FPSCR[NI], are fatal errors
  until their semantics are modelled.
FRES and FRSQRTE are estimates whose exact values are the CPU's own: the
model returns the correctly rounded reciprocal (square root), as QEMU's
970 does; the Xenon's tables wait for a console. FCTIW(Z)'s high word is
undefined (the word's sign extension here), so is MFFS's (0).

### VMX
The context holds 128 vector registers (VMX's 32 are VMX128's first 32)
in the register's own big-endian element order, so LVX and STVX are
plain 16-byte copies, and VSCR (only NJ and SAT exist). The integer
operations are inline (src/ppc/vmx.h, one switch the constant operation
folds), saturations set SAT; the floating-point ones (src/ppc/vmx.cpp) run
lane by lane on SoftFloat: round to nearest even, no exceptions, the
first NaN of the operands quieted, the default NaN 0x7FC00000, and with
NJ set denormal operands and results flushed to zero of the same sign.
VMADDFP and VNMSUBFP are fused. VREFP and VRSQRTEFP return the correctly
rounded value, as QEMU's 970 does; VEXPTEFP and VLOGEFP compute 2^x and
log2 x accurately in double and round once (estimates both: the Xenon's
own values wait for a console, and the test does not compare the last
two). A NaN converted by VCTUXS/VCTSXS gives 0 without SAT, as on the
oracle. LVEBX/LVEHX/LVEWX leave the other elements as they were (the ISA
leaves them undefined). The Xenon's (and Cell's) LVLX, LVRX, STVLX, STVRX
and their LRU forms load and store the parts of a quadword left and right
of the address; the 970 has none of them, so they wait for a console too.

VMX128 decodes as invalid (a fatal error if reached) until it is lifted.

## Lifter
`recomp_lift_ppc --image <title.xex> --out <dir>` (tools/lifter_ppc):
- Function starts: the entry point, `.pdata` (each entry's start and
  length), code exports, import thunks, and every `bl` and out-of-range
  branch target. A function is the code reachable from its start within
  its range (`.pdata`'s length, or up to the next start), so a call into
  the middle of a run of code (the register save and restore helpers)
  gets its own overlapping function, and falling through into the next
  start is a tail call.
- Jump tables, ending in `mtctr` and `bcctr` with the index bounded by a
  `cmplwi` and a `bgt` or `bge`: absolute (`lwzx` from a table of
  addresses) and relative (`lbzx`, `lhzx` or `lwzx` of an offset, scaled,
  added to a base); constants from `lis`/`li` with `addi`/`ori`, and the
  `ori r, r, 0` padding skipped. Other `bcctr` are indirect jumps
  (dispatched).
- Calls: `bl` and `bcl` call the target's function, `blr` returns,
  `bctrl` and `blrl` dispatch; registers are spilled to the context
  around anything that can observe them.
- Import thunks become `Import(c, module, ordinal, va)`, defined by the
  platform. `sc` calls `Sc()`, traps that fire `Trap()`, `mftb`
  `TimeBase()`.

## XEX2
`src/xbox360/xex.{h,cpp}` reads the headers, the file format info, the
image (stored, "basic" runs of data and zeros, or "normal": a chain of
blocks of LZX chunks), the PE headers inside the image (sections,
`.pdata`), the import libraries (a record's word in the image says thunk
or variable and the ordinal) and the exports by ordinal of system modules
(the table the security info points at; the PE `.edata` of system modules
is stale). LZX (`src/xbox360/lzx.{h,cpp}`) is written from the format's
specification; it decodes every system module and title checked so far
byte for byte as libmspack does. Encrypted files are refused with a
pointer to `xbox360_xex_decrypt` (tools/xex_decrypt.cpp, OpenSSL's AES
linked dynamically): the security info's AES key decrypted with the
owner's key (ECB) is the session key, the data is CBC under it with a
zero IV.

## Tests and the oracle
The oracle is QEMU's PowerPC 970 bare metal (`qemu-system-ppc64 -M none
-cpu 970fx`), switched to 32-bit mode by the test programs' start-up
(tests/oracle/crt0.S): a 64-bit CPU running in 32-bit mode as a Xenon runs
a title. `tests/oracle/qemu_run.py` loads a program and the interrupt
vectors, waits for the closing `sc` and dumps what the program left in
memory; any other interrupt fails the run.
- The fixture (tests/fixture): hand-written forms of every decoded
  instruction (ops.S), the compiler's two jump table shapes (tables.S) and
  integer C, built with clang and LLD for big-endian PowerPC64 and
  turned into a XEX by `xbox360_elf2xex`. Run bare metal on the oracle
  and lifted (`xbox360_lift_fixture`), it must print the same.
- The form test: `recomp_lift_ppc --forms` writes each distinct
  instruction form as a lifted function and as the original instruction
  in a stub; the oracle runs every stub from random states (edge values,
  base registers into a buffer with random high words) and the host test
  replays them through the lifted forms, which must end bit for bit in
  the same registers, CR, XER, LR, CTR and memory. Builds with planted
  bugs in ppc/lifted.h (RECOMP_PPC_MUTATION) must fail it.
- Where QEMU departs from the ISA the test does not compare (and the
  model follows the ISA): FR (QEMU's is not the ISA's, as spot checks with exact
  arithmetic show), FX (QEMU sets it on every exception, not on a
  0 to 1 change), FI and FPRF's C bit on compares (the ISA alters only
  FPCC), FRES's FPRF and its estimate of 1/±0 (QEMU: ±0.5), and VSCR's
  reserved bits (QEMU keeps what MTVSCR wrote).
- Not covered by QEMU: VMX128 (no emulator implements it) and what the
  970 does differently from the Xenon (DCBZ's size, the undefined cases
  above, the estimates, the LVLX family). Those wait for a console.

## Roadmap
1. Integer core, XEX reader, oracle (done).
2. Floating point (exact, FPSCR) and VMX, on the same oracle (done).
3. VMX128: Xenia's implementation as the reference until a console can
   run test programs.
4. Runtime: the arena with the physical memory views, process and
   thread bring-up (r13 and the thread's kernel structures), the kernel
   and xam HLE, written against the system's own behaviour.
5. Xenos: the command processor and shaders through gfx::Device.
