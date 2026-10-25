# SwARc: shared lifter + runtime for the recomps

Rules only. Design in ARCHITECTURE.md (its layers: "Layout: base, CPU
cores, platform modules"). Each platform module has its own rules
(platforms/<name>/CLAUDE.md, on top of these) and docs of the behaviour
it reimplements (platforms/<name>/docs). Game projects keep their own
CLAUDE.md/FINDINGS.md for what is specific to that game; behaviour of an
OS or middleware API goes here, even when a game is where it came up.

## Layout
- Library: ~/Documents/Programming/Recomp/SwARc
- Users (sibling checkouts, they add this tree with add_subdirectory):
  - ~/Documents/Programming/Recomp/Dungeon Siege/DS1 (DS1-Rebuilt)
  - ~/Documents/Programming/Recomp/CoD/CoD1 (Call of Duty)
  - ~/Documents/Programming/Recomp/Travellers_Tales/LSWTCS (LEGO Star
    Wars: The Complete Saga)
  - ~/Documents/Programming/Recomp/KEX/Q2E (Quake II Enhanced, x64)
- The base is src/runtime (with cmake/host.cmake); the x86 core is
  src/x86, tools/lifter_x86 and tests/x86, the PowerPC core src/ppc,
  tools/lifter_ppc and tests/ppc; each platform module is
  platforms/<name> (Win32: platforms/win32).

## Build and test
- Standalone (library tests only):
  `cmake --preset debug -DDXVK_NATIVE_DIR=... && cmake --build --preset debug && ctest --test-dir out/build/debug`
- The x86 core without Win32: `cmake --preset x86 && cmake --build --preset x86 && ctest --test-dir out/build/x86`.
  Changes to the base or src/x86 must keep it building: it shows that
  the core does not depend on a platform.
- Platform modules (platforms/<name>) are built only with their
  RECOMP_PLATFORM_<NAME> option; their own rules are in
  platforms/<name>/CLAUDE.md. The Xbox 360 and its PowerPC core alone:
  `cmake --preset xbox360 && cmake --build --preset xbox360 && ctest --test-dir out/build/xbox360`.
- Real validation happens in the game projects: after a change here,
  rebuild each one listed under Layout, re-lift if the lifter changed, and
  run its ctest (with <ID>_GAME_EXE set, the EXE-derived oracle tests run
  too).

## Product constraints (see ARCHITECTURE.md)
- No emulation in the shipping binary: no interpreter, JIT, runtime
  guest-code decoding or slow-path fallback. Unlifted code reached at
  runtime is a fatal error naming the VA; fix it in the lifter/manifest.
  Emulators only as dev-time references and test oracles (Unicorn only
  in tests).
- 64-bit host only; no i386/multilib dependencies.
- macOS arm64 + Metal is a requirement: in lifted code and the runtime,
  no x86 intrinsics, no inline asm, no `long double`, no host FPU/x87
  state (beyond checking that it is the default one), no assumption of
  4 KiB host pages, no Linux-only syscalls/flags outside the platform
  layer (guard with #ifdef). New graphics front ends (OpenGL, ...) go
  through gfx::Device, never straight to a host graphics API.
- The recompilation is scaffolding for a native port: keep lifted code
  exact (it is the reference for hand-written replacements) and keep
  scaffold-only mechanisms out of replacement code.

## Rules
- Layers: the base uses neither a core nor a platform, a core no
  platform. A platform reaches its core through the core's interfaces
  (x86/runtime.h's Process hooks, the lifter's ImageReader); what two
  platforms need goes into their core or the base, never from one
  platform into another.
- Nothing game-specific in this repo: game paths, names, registry
  contents, struct offsets inside a game and game patches belong in the
  game project (GameConfig fields, hooks, replacements). Code never
  checks for a particular game (EXE or file names, addresses, hashes of
  game files); per-game differences go through GameConfig or a hook.
- No game references in code, comments, docs or tests: no game or
  engine names, no game install paths, no pointers into a game
  project's files (FINDINGS.md, ...). Explain why behaviour exists in
  generic terms ("games that ..."). Middleware is not a game: Miles,
  Bink, EAX builds may be named and identified by version and SHA-256.
  The only place game projects are named here is Layout above; their
  list and status live on the project site (sibling checkout
  ../SwARc.site), which README.md links to.
- Dev/test switches are read with Env("NAME") (the game's <ID>_NAME),
  never getenv with a hard-coded prefix, and never at static-init time.
- Every import needs an explicit HLE handler; an unhandled one is fatal
  by design. No catch-all stubs that return 0.
- HLE source files belong to the matching recomp_* shared library in
  their CMakeLists.txt (their registrars run when it loads). Every
  library links what it uses: --no-undefined (Linux) and ld64 (macOS)
  refuse anything else.
- Another session may be working in the main checkout: do parallel work
  in a separate git worktree (e.g. ../SwARc-<topic>) and build games
  against it in their own build dirs (-DSWARC_DIR=...).
- The guest arena base is chosen at startup and is not a compile-time
  constant. Do not hand host pointers to guest code; convert with
  GuestMemory::to_guest.
- dxvk-native throws in Direct3DCreate9 unless DXVK_WSI_DRIVER is set;
  host code sets `setenv("DXVK_WSI_DRIVER", "SDL3", 0)` before any D3D9
  call. Keep it that way.
- Wine, Bochs, GPL engine source releases: references only, never copy
  code.
- Commits: GPG-signed (global config), no Claude co-author trailers or
  generated-by markers.

## x86 core (src/x86, tools/lifter_x86, tests/x86)
- Lifter output must not change unintentionally: when touching
  tools/lifter_x86, diff a game's generated/ before and after.
- The x87 FPU is modelled exactly (80-bit registers, CW precision and
  rounding, SW condition codes, per-thread state); the 80-bit model is
  the definition of correct. Host double only inside proven-exact fast
  paths, each with:
  - its proof next to the code or in ARCHITECTURE.md;
  - a fallback to the exact model for everything outside the proof's
    conditions;
  - fuzz tests against the SoftFloat model (tests/x86/x87_fast_test) that
    mutations of the path make fail.
  Only in the host's default floating-point environment (never change
  the rounding mode), and code that runs such a path is built with
  RECOMP_X87_FP_FLAGS (-ffp-contract=off, no fast-math).
- SSE and AVX: lanes through x86/sse.cpp (SoftFloat + x86 NaN
  rules + exact MXCSR flags), never host float/double arithmetic.
  Changes must keep tests/x86/lift_sse_test and (for x64 images)
  tests/x86/lift_x64_test passing.
- CPUID is a fixed CPU, never the host's: SSE2-class for 32-bit guests,
  AMD family 16h for x64 ones (ARCHITECTURE.md, "64-bit guests"). Claim
  a feature only once the lifter implements it bit-exactly and an oracle
  test covers it.
- lifted.h semantics changes must keep tests/x86/lift_semantics_test.cpp
  passing (bit-exact against the host CPU).
- HLE handlers take arguments with Arg(c, i) and return EAX; declare the
  stdcall argument count correctly in HLE(...) or the guest stack breaks.
- Nothing of one platform in the core: a platform's process, threads'
  control blocks, exception dispatch and window events reach the core
  through Process's hooks (x86/runtime.h).

## PowerPC core (src/ppc, tools/lifter_ppc, tests/ppc)
Design in src/ppc/ARCHITECTURE.md. These rules hold for every PowerPC
platform module too.
- Alone: `cmake --preset ppc && cmake --build --preset ppc && ctest --test-dir out/build/ppc`
  (no x86 core, no host layer). The oracle tests need qemu-system-ppc64,
  clang, ld.lld and the repository's `.venv/` Python (standard library
  only; override with -DRECOMP_PYTHON=...).
- Lifter output must not change unintentionally: when touching
  tools/lifter_ppc, diff the fixture's and a platform's lifted code
  before and after.
- Nothing in src/ppc, tools/lifter_ppc or tests/ppc knows a console:
  executable formats come through the lifter's ImageReader (ELF is its
  own), the system through Import/Sc/Trap/TimeBase. A CPU's extensions
  of the ISA decode only under its Cpu (tools/lifter_ppc/ppc.h); the
  generic one is the user instruction set with floating point and VMX.
- QEMU and emulators are references and test oracles only, never
  linked, never shipped. GPL sources are never copied, only cited for
  behaviour (binutils' PowerPC disassembler among them); BSD sources may
  be read closely, and anything copied keeps its notice in the file and
  in NOTICE.
- The CPU is modelled exactly in 32-bit mode (src/ppc/ARCHITECTURE.md,
  "CPU"); floating point through SoftFloat with the PowerPC's NaN rules
  under FPSCR (src/ppc/fp.cpp), never host float arithmetic outside
  proven-exact fast paths with their proof and fuzz test.
- Where the oracle (QEMU) departs from the ISA, follow the ISA, show the
  difference with exact arithmetic or the ISA's text, and leave that bit
  out of the comparison with the reason next to it.
- ppc/lifted.h semantics changes must keep ppc_lift_forms passing
  against the oracle (ppc_vmx128_test too for VMX128, which the oracle
  runs only as VMX twins), and a planted bug (RECOMP_PPC_MUTATION) for
  each new helper that a test catches.
- Where the ISA leaves a result undefined, say so next to the code, keep
  it out of the oracle comparison (tests/ppc/lift_forms_test.cpp,
  Undefined) and note it for the hardware check in
  src/ppc/ARCHITECTURE.md.
- Test fixtures are source (assembly and C built by clang and LLD for
  big-endian PowerPC64) checked in with their build rule; never a binary
  from the internet, never anything from a game or a console's firmware.
