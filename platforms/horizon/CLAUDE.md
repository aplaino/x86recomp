# The Switch (Horizon OS) platform module

Rules only, on top of the repository's CLAUDE.md. Design in
ARCHITECTURE.md (here); observed behaviour of the SDK, services and NVN
reimplemented here in docs/. The AArch64 core (tools/lifter_a64,
src/a64) follows these rules too.

## Build and test
- The Switch alone: `cmake --preset horizon && cmake --build --preset horizon && ctest --test-dir out/build/horizon`
  (RECOMP_X86 off, RECOMP_PLATFORM_HORIZON on). Python tools and tests
  run from the repository's `.venv/` (unicorn, capstone, lz4; override
  with -DRECOMP_PYTHON=...), never from a system Python.
- arm64 host build on x86-64: `cmake --preset horizon-aarch64-qemu`
  (crossdev's aarch64-unknown-linux-gnu sysroot, run under
  qemu-aarch64; CPU-only, RECOMP_HOST off, as the sysroot has no SDL3,
  ffmpeg or LLVM). Run its ctest after changes to a64/lifted.h, the
  emitter or the runtime. The host layer on arm64 is checked on macOS.
- Changes to the top-level CMakeLists.txt or anything shared with the x86
  path: also build and test the default preset (x86) with
  -DRECOMP_PLATFORM_HORIZON=ON, so both build in one tree.
- Lifter output must not change unintentionally: when touching
  tools/lifter_a64, diff the fixture's and a game's generated code
  before and after.

## Product constraints (see ARCHITECTURE.md)
- No emulation in the shipping binary: no interpreter, JIT, runtime
  AArch64 decoding or slow-path fallback. Unlifted code reached at
  runtime is a fatal error naming the VA; fix it in the lifter/manifest.
- Unicorn and emulators (yuzu, suyu, Ryujinx, Wanderlust) are references
  and test oracles only, never linked, never shipped.
- GPL sources (yuzu, suyu, ssbu-recomp, smo-recomp) are never copied,
  only cited for behaviour. MIT sources (Ryujinx, LASRecomp, Wanderlust)
  may be read closely; anything copied keeps its notice in the file and
  in NOTICE.
- Portable C++ in lifted code and the runtime: no x86 or arm64
  intrinsics, no inline asm, no `long double`, no host FP environment
  changes, no assumption of 4 KiB host pages, no Linux-only
  syscalls/flags outside the platform layer (src/a64/platform.h; guard
  with #ifdef).
- Host services come from the shared host layer (cmake/host.cmake):
  NVN through gfx::Device, audio through the software mixer, windows
  and input through SDL3, movies through ffmpeg; never from an HLE file
  straight to a host graphics or audio API, and no second copy of a
  host library for the Switch.
- Floating point is modelled exactly (SoftFloat ARM specialisation under
  the thread's FPCR; FPSR flags). Host float/double only inside
  proven-exact fast paths, each with its proof next to the code, a
  fallback to the model outside the proof's conditions, and a fuzz test
  that mutations of the path make fail.
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
- Nothing in src/a64 or tools/lifter_a64 knows the Switch: executable
  formats come through the lifter's ImageReader, the kernel through Svc.
- a64/lifted.h semantics changes must keep the horizon_lift_forms tests
  passing (Unicorn on x86-64 hosts, the host CPU on arm64).
- Test fixtures are source (assembly and C built by clang's integrated
  assembler and LLD for aarch64-none-elf) checked in with their build
  rule; never a binary from the internet, never anything from a game.
