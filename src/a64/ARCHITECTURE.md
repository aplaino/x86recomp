# Architecture: the AArch64 core

The lifter `recomp_lift_a64` (`tools/lifter_a64`: decoder, function
discovery, jump tables, the emitter) and the CPU model and dispatcher
lifted code runs on (`src/a64`, library `recomp_a64`), shared by every
console platform with an AArch64 CPU (the top-level ARCHITECTURE.md,
"Layout: base, CPU cores, platform modules"). It is built with
`RECOMP_A64`, which such a platform turns on, and alone with its tests
by the `a64` preset (no x86 core, no host layer). Nothing here knows a
console's system or executable format: the lifter reads static-PIE ELF
itself (`a64/image.h`, library `recomp_a64_image`, which a platform's
loaders build on), a platform registers an image reader for its own,
and the platform defines the kernel's supervisor calls (`Svc`;
a64/lifted.h). Its tests are in `tests/a64`. The Switch
(platforms/horizon) is the first platform on it, and the reason for the
decisions below.

The core keeps the x86 core's shape wherever the guest allows: one C++
function per guest function, registers cached in locals, a dispatcher
for indirect calls, exact arithmetic as the reference for hand-written
replacements. Where the two differ, this file says why.

## Guest addresses are host addresses
The guest already is 64-bit, so unlike the x86 core there is no arena
and no base register: each module is mapped at the guest VA the
platform gives it, with host `mmap` at that exact address, and a guest
pointer is a host pointer. A guest load is one host load. The platform
chooses the layout; it has to fit the window both hosts leave free at
startup, [0x7000000000, 0x7180000000) (macOS arm64 keeps
[0xfc0000000, 0x7000000000) unmappable and reserves malloc regions at
random above it), and the core's own tests load their fixture there too
(`kElfBase`).

## Modules and function discovery
A module (a64/image.h) is its memory image with segments at their
offsets from 0, `.dynamic`'s symbols and relocations, and
`.eh_frame_hdr`. Function boundaries are exact: starts from
`.eh_frame_hdr`, sized function symbols, relocation targets that point
into `.text` (vtables, callbacks), direct `BL` targets and `B` targets
outside the branching function (`ImageFromModule` and the lifter's
discovery). A function's instructions are the ones reachable from its
start within its range (the FDE or symbol size, else up to the next
start), so data after a `ret` (literal pools) is never decoded. Ghidra
is a dev-time aid for naming and for jump tables, as on the x86 core,
not a required input.

## Lifter: AArch64 -> portable C++
- Decoder: our own, from the ARM encoding tables: AArch64 is fixed
  width and regular, the decoder is a few thousand lines, and owning it
  gives the emitter the operand model it needs. Capstone is a build-time
  cross-check in the tools and tests only.
- One `void f_<va>(Ctx&)` per guest function. General registers, SP and
  NZCV are cached in locals and spilled around calls, HLE and anything
  that can observe them; `v0`-`v31` live in `Ctx` and are operated on in
  place through the FP/SIMD model, like the x86 core's SSE registers.
- `BL`/`B` to a known function: direct call (a `B` tail call is a call
  followed by return). `BLR`/`BR`: `Dispatch(c, target, va)`, a hash
  table from guest VA to lifted function; thunk-range targets go to the
  HLE registry. `RET`: return. Compiler jump tables (`adr`/`adrp`;
  scaled `ldrb/ldrh/ldr/ldrsw`; `add`; `br`, bounded by the `cmp` +
  `b.hi`/`b.hs` or `and` mask above them) are recognised by pattern:
  the `br` becomes a switch on the target the code computed, with one
  case per table entry, computed as the code computes it; any other
  target is `BadJump` with the VA, fixed through the manifest.
- Memory ordering: `ldar/stlr` become acquire/release atomics; `ldxr`
  records the address and the value it read in `Ctx`'s exclusive
  monitor and `stxr` is a compare-and-swap against them, as QEMU and
  Unicorn model the monitor (the guest's own retry loop stays as it
  was); `dmb/dsb` become fences. Lifted code is thread-safe by
  construction because it uses the same atomics the guest did.
- `svc` calls the platform's `Svc` with its immediate; `brk`/`udf` are fatal
  with the VA. `mrs` of `tpidr_el0`, the counter and CPU id registers
  read `Ctx` or the runtime's clock.
- Exactness is tested the way the x86 lifter is, on a fixture built from
  checked-in assembly and C (`tests/a64/fixture`), a static-PIE ELF the
  lifter reads itself, whose two supervisor calls (exit, write) are the
  test platform's (`tests/a64/fixture_platform.cpp`). `recomp_lift_a64
  --forms` emits each distinct instruction form
  (`tests/a64/lift_forms.h`) as a function of its own; the Unicorn oracle
  (`tests/a64/oracle/oracle.py`, Python, so Unicorn is never linked into
  anything) runs the original instruction from random and special
  register states and writes input and output states;
  `a64_lift_forms_test` replays them on the lifted forms and compares
  registers, NZCV, the thread pointer and memory bit for bit. Building
  it with `RECOMP_A64_MUTATION=<n>` plants a known bug in `lifted.h`, and
  those builds must fail. `a64_lift_fixture` runs the whole fixture
  lifted and under Unicorn and compares their output; a platform runs
  the same program in its own format with a crt0 of its own
  (`recomp_a64_fixture`) and compares with the same oracle run. On an
  arm64 host the CPU itself is a second oracle: `forms_native.S` holds
  each form's original instruction in a stub, run from the same inputs,
  for every form whose result does not depend on its address or on the
  host's SP and thread pointer (x18 is skipped on macOS, which reserves
  it). The `a64-aarch64-qemu` preset cross-builds the core and its tests
  for aarch64 Linux and runs them under `qemu-aarch64`; QEMU shares
  Unicorn's CPU model, so that checks the arm64 build and the harness,
  and only real arm64 hardware is an independent oracle.
  Lifter output must not change unintentionally: diff the fixture's and
  a platform's `generated/` when touching the lifter.

## FP/SIMD: exact by model, fast by proof
AArch64 floating point is IEEE binary32/64 under FPCR control:
rounding mode, flush-to-zero (`FZ`, inputs and outputs), default NaN
(`DN`), and ARM's NaN propagation order; `FPSR` accumulates the
exception flags. Games set `FZ` and `DN` and some read `FPSR`.
- The definition of correct is Berkeley SoftFloat with its ARM-VFPv2
  specialisation (and the default-NaN variant), driven by the thread's
  FPCR. Every scalar and NEON lane operation goes through it.
- Host `float`/`double` only inside proven-exact fast paths: an IEEE
  host operation in round-to-nearest with no contraction gives the same
  bits for add, sub, mul, div, sqrt and fma when neither input nor the
  result is NaN or denormal and `FZ` is clear; the fast path checks
  those conditions and falls back to the model otherwise. Each path has
  its proof next to the code, a fuzz test against the model that
  mutations make fail, and is built with `RECOMP_A64_FP_FLAGS`
  (`-ffp-contract=off`, no fast-math) in the host's default environment.
- NEON integer lanes are plain C++ on `uint8_t[16]` registers; the
  tests cover saturation, narrowing and the lane-ordering cases.
- `fcvt`, `frint*`, `fjcvtzs` and the reciprocal-estimate instructions
  (`frecpe`, `frsqrte`, with their table-based results) are in the model
  because games use them in physics and in sorting keys.

## Platform layer
`src/a64/platform.{h,cpp}` holds what POSIX does not cover the same
way on Linux and macOS (fixed-address mappings and their page sizes,
thread names and priorities, the fault handler's context, the user data
directory); the rest of the runtime has no OS #ifdefs.
