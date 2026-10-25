# The Xbox 360 platform module

Rules only, on top of the repository's CLAUDE.md. Design in
ARCHITECTURE.md (here); observed behaviour of the kernel, xam and Xenos
reimplemented here in docs/. The PowerPC core (tools/lifter_ppc, src/ppc)
follows these rules too.

## Build and test
- The Xbox 360 alone: `cmake --preset xbox360 && cmake --build --preset xbox360 && ctest --test-dir out/build/xbox360`
  (RECOMP_X86 off, RECOMP_PLATFORM_XBOX360 on). The oracle tests need
  qemu-system-ppc64, clang, ld.lld and the repository's `.venv/` Python
  (standard library only; override with -DRECOMP_PYTHON=...).
- Changes to the top-level CMakeLists.txt or anything shared with the
  other guests (src/text_hash.h, the host layer): also build and test the
  default preset and `horizon`.
- Lifter output must not change unintentionally: when touching
  tools/lifter_ppc, diff the fixture's and a title's generated code before
  and after.

## Product constraints (see ARCHITECTURE.md)
- No emulation in the shipping binary: no interpreter, JIT, runtime
  PowerPC decoding or slow-path fallback. Unlifted code reached at
  runtime is a fatal error naming the VA; fix it in the lifter/manifest.
- QEMU and emulators (Xenia) are references and test oracles only, never
  linked, never shipped.
- GPL sources are never copied, only cited for behaviour (binutils'
  PowerPC disassembler among them). BSD sources (Xenia, ReXGlue) may be
  read closely; anything copied keeps its notice in the file and in
  NOTICE.
- No keys, firmware or game files in the tree, in tests or in docs: the
  owner decrypts their XEX with xbox360_xex_decrypt and their own key
  file.
- Portable C++ in lifted code and the runtime: no x86 or arm64
  intrinsics, no inline asm, no `long double`, no host FP environment
  changes, no assumption of 4 KiB host pages.
- The CPU is modelled exactly in 32-bit mode (ARCHITECTURE.md, "CPU");
  floating point, when it comes, through SoftFloat with the PowerPC's NaN
  rules under FPSCR, never host float arithmetic outside proven-exact fast
  paths with their proof and fuzz test.

## Rules
- Nothing game-specific here, as for the rest of the repository: title
  ids, game names, paths, struct offsets inside a game and game patches
  belong in the game project. The kernel, xam and middleware are not
  games: they may be named and identified by version.
- Every import needs an explicit handler, registered by module and
  ordinal; an unhandled one is fatal by design. No catch-all stubs that
  return 0.
- Nothing in src/ppc or tools/lifter_ppc knows the Xbox 360: executable
  formats come through the lifter's ImageReader, the kernel through
  Import/Sc/Trap/TimeBase.
- ppc/lifted.h semantics changes must keep xbox360_lift_forms passing
  against the oracle, and a planted bug (RECOMP_PPC_MUTATION) for each new
  helper that the test catches.
- Where the ISA leaves a result undefined, say so next to the code, keep
  it out of the oracle comparison (tests/lift_forms_test.cpp, Undefined)
  and note it for the console check in ARCHITECTURE.md.
- Test fixtures are source (assembly and C built by clang and LLD for
  big-endian PowerPC64) checked in with their build rule; never a binary
  from the internet, never anything from a game or the console's
  firmware.
