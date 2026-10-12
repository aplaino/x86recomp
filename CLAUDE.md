# x86recomp: shared lifter + runtime for the PC recomps

Rules only. Design in ARCHITECTURE.md. Game projects keep their own
CLAUDE.md/FINDINGS.md; findings about this library go in the FINDINGS.md
of the game project where they came up.

## Layout
- Library: ~/Documents/Programming/Recomp/x86recomp
- Users (sibling checkouts, they add this tree with add_subdirectory):
  - ~/Documents/Programming/Recomp/Dungeon Siege/DS1 (DS1-Rebuilt)
  - ~/Documents/Programming/Recomp/CoD/CoD1 (Call of Duty)

## Build and test
- Standalone (library tests only):
  `cmake --preset debug -DDXVK_NATIVE_DIR=... && cmake --build --preset debug && ctest --test-dir out/build/debug`
- Real validation happens in a game project: after a change here, rebuild
  DS1 (and CoD1 once it runs), re-lift if the lifter changed, and run its
  ctest (with DS1_GAME_EXE set, the EXE-derived oracle tests run too).
- Lifter output must not change unintentionally: when touching
  tools/lifter, diff a game's generated/ before and after.

## Product constraints (see ARCHITECTURE.md)
- No emulation in the shipping binary: no interpreter, JIT, runtime x86
  decoding or slow-path fallback. Unlifted code reached at runtime is a
  fatal error naming the VA; fix it in the lifter/manifest.
- No Wine in any product, ever. Wine running an original EXE is allowed
  only as a dev-time behaviour reference. Unicorn only in tests.
- 64-bit host only; no i386/multilib dependencies.
- macOS arm64 + Metal is a requirement: in lifted code and the runtime,
  no x86 intrinsics, no inline asm, no `long double`, no host FPU/x87
  state, no assumption of 4 KiB host pages, no Linux-only
  syscalls/flags outside the platform layer (guard with #ifdef). New
  graphics front ends (OpenGL, ...) go through gfx::Device, never
  straight to a host graphics API.
- The x87 FPU is modelled exactly (80-bit registers, CW precision and
  rounding, SW condition codes, per-thread state). Never map x87 to
  host double.
- SSE/SSE2 likewise: lanes through runtime/sse.cpp (SoftFloat + x86 NaN
  rules + exact MXCSR flags), never host float/double arithmetic.
  Changes must keep tests/lift_sse_test passing.
- CPUID is a fixed SSE2-class CPU, never the host's. Claim a feature only
  once the lifter implements it bit-exactly and an oracle test covers it.
- The recompilation is scaffolding for a native port: keep lifted code
  exact (it is the reference for hand-written replacements) and keep
  scaffold-only mechanisms out of replacement code.

## Rules
- Nothing game-specific in this repo: game paths, names, registry
  contents, struct offsets inside a game and game patches belong in the
  game project (GameConfig fields, hooks, replacements). A game example
  in a comment is fine when it explains why behaviour exists.
- Dev/test switches are read with Env("NAME") (the game's <ID>_NAME),
  never getenv with a hard-coded prefix, and never at static-init time.
- Every import needs an explicit HLE handler; an unhandled one is fatal
  by design. No catch-all stubs that return 0.
- HLE handlers take arguments with Arg(c, i) and return EAX; declare the
  stdcall argument count correctly in HLE(...) or the guest stack breaks.
- HLE source files belong to an OBJECT library (registrars must link);
  put new ones in the matching recomp_* target in CMakeLists.txt.
- lifted.h semantics changes must keep tests/lift_semantics_test.cpp
  passing (bit-exact against the host CPU).
- Another session may be working in the main checkout: do parallel work
  in a separate git worktree (e.g. ../x86recomp-<topic>) and build games
  against it in their own build dirs (-DX86RECOMP_DIR=...).
- Guest addresses stay below 0x80000000 unless a game has done the
  large-address audit (ARCHITECTURE.md), and then only behind its flag.
- The guest arena base is chosen at startup and is not a compile-time
  constant. Do not hand host pointers to guest code; convert with
  GuestMemory::to_guest.
- dxvk-native throws in Direct3DCreate9 unless DXVK_WSI_DRIVER is set;
  host code sets `setenv("DXVK_WSI_DRIVER", "SDL3", 0)` before any D3D9
  call. Keep it that way.
- Wine, Bochs, ioquake3, iortcw: references only, never copy code.
- Commits: GPG-signed (global config), no Claude co-author trailers or
  generated-by markers.
