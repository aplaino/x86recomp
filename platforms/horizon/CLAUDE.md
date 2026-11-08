# The Switch (Horizon OS) platform module

Rules only, on top of the repository's CLAUDE.md (its "AArch64 core"
section holds for this module too). Design in ARCHITECTURE.md (here);
observed behaviour of the SDK, services and NVN reimplemented here in
docs/.

## Build and test
- The Switch alone: `cmake --preset horizon && cmake --build --preset horizon && ctest --test-dir out/build/horizon`
  (RECOMP_X86 and RECOMP_PLATFORM_WIN32 off, RECOMP_PLATFORM_HORIZON
  on); it builds and runs the AArch64 core's tests too. Python tools and
  tests run from the repository's `.venv/` (unicorn, capstone, lz4;
  override with -DRECOMP_PYTHON=...), never from a system Python.
- Changes to the top-level CMakeLists.txt or anything shared with the x86
  core or Win32: also build and test the default preset (debug) with
  -DRECOMP_PLATFORM_HORIZON=ON, so both build in one tree.

## Product constraints (see ARCHITECTURE.md)
- No emulation in the shipping binary: no interpreter, JIT, runtime
  AArch64 decoding or slow-path fallback. Unlifted code reached at
  runtime is a fatal error naming the VA; fix it in the lifter/manifest.
- Emulators (yuzu, suyu, Ryujinx, Wanderlust) are references only, never
  linked, never shipped.
- GPL sources (yuzu, suyu, ssbu-recomp, smo-recomp) are never copied,
  only cited for behaviour. MIT sources (Ryujinx, LASRecomp, Wanderlust)
  may be read closely; anything copied keeps its notice in the file and
  in NOTICE.
- Portable C++ in lifted code and the runtime: no x86 or arm64
  intrinsics, no inline asm, no `long double`, no host FP environment
  changes, no assumption of 4 KiB host pages, no Linux-only
  syscalls/flags outside the core's platform layer
  (src/a64/platform.h).
- Host services come from the shared host layer (cmake/host.cmake):
  NVN through gfx::Device, audio through the software mixer, windows
  and input through SDL3, movies through ffmpeg; never from an HLE file
  straight to a host graphics or audio API, and no second copy of a
  host library for the Switch.
- Guest addresses are host addresses: modules at their console VAs,
  everything guest-visible in layout.h's window below 1<<39 (the range
  macOS arm64 leaves free; ARCHITECTURE.md). Never hand the game a host
  address outside those reservations.

## Rules
- Nothing game-specific here, as for the rest of the repository: title
  ids, game names, paths inside a RomFS, struct offsets inside a game and
  game patches belong in the game project. The SDK and middleware are
  not games: nnSdk, NVN and middleware may be named and identified by
  version.
- Every import and every supervisor call needs an explicit handler; an
  unhandled one is fatal by design. No catch-all stubs that return 0.
  IPC (svcSendSyncRequest) is never implemented: services are
  reimplemented at the SDK API.
- HLE handlers are registered under the mangled import name and take
  their arguments through the AAPCS64 helpers (x0-x7, v0-v7, stack);
  declare the signature once, in the handler, never by hand-reading
  registers.
- HLE source files belong to the matching recomp_horizon_* shared
  library in CMakeLists.txt (here).
- Nothing of the Switch in src/a64, tools/lifter_a64 or tests/a64: its
  formats, layout and kernel stay here. The NRO test runs the core's
  fixture with this module's crt0 (tests/fixture/crt0.S, source like the
  fixture).
