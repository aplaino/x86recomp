# Architecture: the PowerPC core

The lifter `recomp_lift_ppc` (`tools/lifter_ppc`: decoder, function
discovery, jump tables, the emitter) and the CPU model and dispatcher
lifted code runs on (`src/ppc`, library `recomp_ppc`), shared by every
console platform with a PowerPC CPU (the top-level ARCHITECTURE.md,
"Console platforms"). It is built with `RECOMP_PPC`, which such a
platform turns on, and alone with its tests by the `ppc` preset (no x86
path, no host layer). Nothing here knows a console's system or
executable format: the lifter reads ELF itself (`elf.h`), a platform
registers an image reader for its own, and the platform defines what
lifted code calls (`Import`, `Sc`, `Trap`, `TimeBase`; ppc/lifted.h).
Its tests are in `tests/ppc`.

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

## CPU: a 64-bit PowerPC in 32-bit mode
Xbox 360 titles run on a 64-bit PowerPC in 32-bit mode (MSR[SF] = 0), as
their code shows: addresses above 2 GiB are built with `lis` (so the
registers hold their sign-extended forms) and used directly. In 32-bit
mode (ppc/lifted.h):
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

So far the core lifts the fixed-point, branch, condition register, storage
control and special purpose register instructions; floating point, VMX
and VMX128 decode as invalid (a fatal error if reached) until they are
lifted, exactly like the other models (SoftFloat with the PowerPC's NaN
rules under FPSCR; VMX's non-IEEE mode; VMX128's extra registers and
Direct3D packing instructions).

## Lifter
`recomp_lift_ppc --image <module> --out <dir>` (tools/lifter_ppc):
- Function starts: those the image reader gives (ELF: the entry point and
  the sized function symbols; a XEX: the entry point, `.pdata` with each
  entry's length, code exports and import thunks), and every `bl` and
  out-of-range branch target. A function is the code reachable from its
  start within its range (the length the reader gives, or up to the next
  start), so a call into the middle of a run of code (the register save
  and restore helpers) gets its own overlapping function, and falling
  through into the next start is a tail call.
- Jump tables, ending in `mtctr` and `bcctr` with the index bounded by a
  `cmplwi` and a `bgt` or `bge`: absolute (`lwzx` from a table of
  addresses) and relative (`lbzx`, `lhzx` or `lwzx` of an offset, scaled,
  added to a base); constants from `lis`/`li` with `addi`/`ori`, and the
  `ori r, r, 0` padding skipped. Other `bcctr` are indirect jumps
  (dispatched).
- Calls: `bl` and `bcl` call the target's function, `blr` returns,
  `bctrl` and `blrl` dispatch; registers are spilled to the context
  around anything that can observe them.
- Import thunks (from a platform's image reader) become `Import(c, module,
  ordinal, va)`, defined by the platform. `sc` calls `Sc()`, traps that
  fire `Trap()`, `mftb` `TimeBase()`.

## Tests and the oracle
The oracle is QEMU's PowerPC 970 bare metal (`qemu-system-ppc64 -M none
-cpu 970fx`), switched to 32-bit mode by the test programs' start-up
(tests/ppc/oracle/crt0.S): a 64-bit CPU running in 32-bit mode as a Xenon
runs a title. `tests/ppc/oracle/qemu_run.py` loads a program and the
interrupt vectors, waits for the closing `sc` and dumps what the program
left in memory; any other interrupt fails the run.
- The fixture (tests/ppc/fixture): hand-written forms of every decoded
  instruction (ops.S), the compiler's two jump table shapes (tables.S) and
  integer C, built with clang and LLD for big-endian PowerPC64 and lifted
  from that ELF. Run bare metal on the oracle and lifted
  (`ppc_lift_fixture`), it must print the same.
- The form test: `recomp_lift_ppc --forms` writes each distinct
  instruction form as a lifted function and as the original instruction
  in a stub; the oracle runs every stub from random states (edge values,
  base registers into a buffer with random high words) and the host test
  replays them through the lifted forms, which must end bit for bit in
  the same registers, CR, XER, LR, CTR and memory. Builds with planted
  bugs in ppc/lifted.h (RECOMP_PPC_MUTATION) must fail it.
- Not covered by QEMU: VMX128 (no emulator implements it) and what the
  970 does differently from the Xenon (DCBZ's size, the undefined cases
  above). Those wait for a console.
