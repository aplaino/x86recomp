# Architecture: the Switch (Horizon OS) platform module

This module turns a Nintendo Switch game into a native 64-bit program:
the game's AArch64 machine code is lifted to C++ ahead of time and linked
with a runtime that reimplements the SDK (nnSdk), the graphics API (NVN)
and the system services it calls (HLE). Nothing here is specific to one
game: game projects hold that (entry point config, manifest, symbols,
game hooks) and use this tree as a library.

It is a console platform on x86recomp's shared core (the top-level
ARCHITECTURE.md, "Console platforms"), built only with
`RECOMP_PLATFORM_HORIZON`:
- Core, shared by every AArch64 guest: the lifter `recomp_lift_a64`
  (`tools/lifter_a64`: decoder, function discovery, jump tables, the
  emitter) and the CPU model and dispatcher it targets (`src/a64`,
  library `recomp_a64`). The lifter knows no executable format; the
  platform registers an image reader for its own.
- Platform, here: the NRO/NSO formats and the lifter's reader for them,
  the guest address layout, process bring-up, the kernel's supervisor
  calls, the SDK and service HLE, NVN and the Maxwell shader front end
  (libraries `recomp_horizon_*`), with their tests and docs.
Paths below are relative to `platforms/horizon` unless they start with
`src/a64` or `tools/lifter_a64`. A Switch game project turns
`RECOMP_X86` off: it needs none of the x86 path's code or dependencies.

The module keeps the x86 path's shape wherever the guest allows: one
C++ function per guest function, registers cached in locals, a
dispatcher for indirect calls, an HLE registry that is fatal on
anything unhandled, exact arithmetic as the reference for hand-written
replacements, and a game project that owns everything game-specific.
Where the two differ, this file says why.

## Scaffold, not the destination
Recompilation is how a game first runs natively, not how it stays. The
end state of each port is native source: lifted functions are replaced
by hand-written C++ one subsystem at a time until nothing lifted is
left, and the lifter, guest layout and HLE fall away with them.
- The lifted code stays exact (flags, FPCR/FPSR, NEON lanes, memory
  ordering), so it is the reference every replacement is diffed against.
- Replacements take over any function by address and work on the same
  guest state while their neighbours are still lifted.
- Nothing only the scaffold needs (the dispatcher, thunks, the guest
  scheduler) leaks into the native code that replaces it.

## Goals and hard constraints
1. Native Linux first; macOS on Apple Silicon with a native Metal
   renderer is a requirement, not an afterthought. Nothing in the lifter
   output or the runtime may assume an x86 or an arm64 host: lifted code
   is portable C++ on both, with no inline assembly, so one build has one
   behaviour everywhere and the arm64 host is only a second oracle.
2. No emulation in the shipping binary: no interpreter, no JIT, no
   runtime AArch64 decoding, no fallback path. An indirect branch to an
   address that was not lifted is a fatal error naming the VA, never a
   slow path. Unicorn is used by tests only.
3. No emulator code in the product: yuzu, suyu, Ryujinx, Wanderlust and
   the published recompilations are references for console behaviour,
   cited where used. GPL sources (yuzu, suyu, ssbu-recomp, smo-recomp)
   are never copied. MIT sources (Ryujinx, LASRecomp, Wanderlust) may be
   read closely; copied code keeps its notice. This tree is MIT; if that
   ever has to change it becomes GPL-3.0-or-later, never a fixed version.
4. 64-bit everywhere, host and guest. Guest addresses are host
   addresses (below).
5. Users bring their own game: no game code, assets, keys or firmware in
   any repository. Decryption and extraction happen on the owner's
   console; this tree only reads an already extracted `exefs`/`romfs`.

```
exefs/{rtld,main,subsdk*,sdk} ── recomp_lift_a64 (AArch64 -> C++) ── generated/*.cpp
                                     │  manifest, symbols         (gitignored, per game)
                                     ▼
   ┌───────── game binary (native 64-bit, portable C++) ───────────────────┐
   │ lifted game code ── modules mapped at their console VAs (0x71.. )     │
   │        │ imports by symbol name / NVN entry points / svc              │
   │        ▼                                                              │
   │ HLE: nn::os nn::fs nn::hid nn::audio nn::time nn::oe nn::vi ... nvn   │
   │        │          │            │                  │                   │
   │     POSIX       SDL3       ffmpeg (H.264)     gfx: Vulkan, Metal      │
   │   (threads,  (window,                      maxwell -> GLSL ->       │
   │    files)     input,                        SPIR-V / MSL              │
   │               audio)                                                  │
   └───────────────────────────────────────────────────────────────────────┘
```

## What "64-bit" means here
The guest already is 64-bit, so unlike the x86 path there is no arena and
no base register: each module is mapped at the guest VA it would have on
the console, with host `mmap` at that exact address, and a guest pointer
is a host pointer. A guest load is one host load. HLE code dereferences
guest pointers directly; struct layouts are the AArch64 ones the host
also uses (LP64, same alignment rules), so guest-layout mirror types are
only needed where the SDK's ABI differs from the host compiler's
(packed or over-aligned SDK types, documented in docs/ as found).

Layout, fixed for every game and documented so addresses in crash logs
and comments mean the same thing everywhere:
- `main` at 0x7100000000; `rtld`, `subsdk*` and `sdk` follow at the next
  2 MiB-aligned addresses in ExeFS order, like the console's loader.
- The guest heap (`nn::os` memory, the game's allocators) and guest
  thread stacks live in reservations below 1<<39, the console's user
  address-space size, so code that packs or compares addresses behaves
  as on the console. A heap region is a host reservation that is
  committed on demand; the console's memory-state queries
  (`svcQueryMemory`) answer from a per-region table.
- Thunks (addresses the game may call that reach host code: imports it
  resolves at run time, NVN entry points, callbacks the runtime hands
  out) live in one reserved range so the dispatcher recognises them.

On macOS the kernel honours fixed mappings in this range (it is far
above the main image and below the 47-bit limit); on Linux they are
free. Both are checked at startup and a collision is fatal, not worked
around.

## Game projects
A game project adds x86recomp with `add_subdirectory()` (with
`RECOMP_PLATFORM_HORIZON` on and `RECOMP_X86` off), links
`recomp_horizon` plus the optional HLE modules its modules import
(`recomp_horizon_nvn`, `recomp_horizon_audio`,
`recomp_horizon_movie`), adds its lifted code with
`recomp_add_lifted_a64()`, and fills a `recomp::horizon::GameConfig`
in its `main()`:
- `id`: per-user data directory ($XDG_DATA_HOME/<id>) and the prefix of
  dev/test switches (<ID>_TRACE, <ID>_TEST_INPUT, <ID>_BACKGROUND, ...);
- `exefs`, `romfs`: the owner's extracted directories (host paths);
- `title`: what `nn::oe`/`nn::ns` report (title id, display version,
  language list) from the owner's `control.nacp`, read at run time;
- `scheduler`: free host scheduling or the console model (below);
- hooks for the rare behaviour that needs game knowledge.

The game's executable holds its lifted code, its replacements and
`main()`; the runtime, the FP model, the HLE modules and the libraries
under them are shared libraries (librecomp_horizon_*.so, .dylib on
macOS), one copy of state each, rebuilt independently. Lifted code
reaches the dispatcher through `RegisterLiftedFunctions`, compiled into the lifted object by
`recomp_add_lifted_a64()`, as on the x86 path.

Game-specific patches live in the game project as hand-written
replacements of lifted functions: `replace <va>` in the manifest emits
the lifted body as `f_<VA>_lifted` and the game project defines
`f_<VA>`. Widescreen, frame-rate and quality-of-life changes are
replacements, never lifter special cases.

## Decisions

### Modules and function discovery
NSO modules (LZ4 segments, `MOD0` header, `.dynamic`, `.dynsym`,
`.eh_frame_hdr`) are decoded by the lifter and by the runtime loader
from the same code (`src/horizon/nso_image`). Function boundaries are
exact: starts from `.eh_frame_hdr`, sized function symbols, relocation
targets that point into `.text` (vtables, callbacks), direct `BL`
targets and `B` targets outside the branching function. A function's
instructions are the ones reachable from its start within its range
(the FDE or symbol size, else up to the next start), so data after a
`ret` (literal pools) is never decoded. Ghidra is a dev-time aid for
naming and for jump tables, as on the x86 path, not a required input. NRO
(homebrew) is loaded the same way and is the lifter's first fixture
format.

### Lifter: AArch64 -> portable C++
- Decoder: our own, from the ARM encoding tables: AArch64 is fixed
  width and regular, the decoder is a few thousand lines, and owning it
  gives the emitter the operand model it needs. Capstone is a build-time
  cross-check in the tools and tests only.
- One `void f_<va>(Ctx&)` per guest function. General registers, SP and
  NZCV are cached in locals and spilled around calls, HLE and anything
  that can observe them; `v0`-`v31` live in `Ctx` and are operated on in
  place through the FP/SIMD model, like the x86 path's SSE registers.
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
  construction because it uses the same atomics the console did.
- `svc` calls the runtime's supervisor-call table; `brk`/`udf` are fatal
  with the VA. `mrs` of `tpidr_el0`, the counter and CPU id registers
  read `Ctx` or the runtime's clock.
- Exactness is tested the way the x86 lifter is, on a fixture NRO built
  from checked-in assembly and C (`tests/fixture`). `recomp_lift_a64
  --forms` emits each distinct instruction form (`tests/lift_forms.h`) as a function of
  its own; the Unicorn oracle (`tests/oracle/oracle.py`, Python, so
  Unicorn is never linked into anything) runs the original instruction
  from random and special register states and writes input and output
  states; `horizon_lift_forms_test` replays them on the lifted forms and
  compares registers, NZCV, the thread pointer and memory bit for bit.
  Building it with `RECOMP_A64_MUTATION=<n>` plants a known bug in
  `lifted.h`, and those builds must fail. `horizon_lift_fixture` runs the whole
  fixture lifted and under Unicorn and compares their output. On an
  arm64 host the CPU itself is a second oracle: `forms_native.S` holds
  each form's original instruction in a stub, run from the same inputs,
  for every form whose result does not depend on its address or on the
  host's SP and thread pointer (x18 is skipped on macOS, which reserves
  it). The `aarch64-qemu` preset cross-builds everything for aarch64
  Linux and runs it under `qemu-aarch64`; QEMU shares Unicorn's CPU
  model, so that checks the arm64 build and the harness, and only real
  arm64 hardware is an independent oracle.
  Lifter output must not change unintentionally: diff a fixture's
  `generated/` when touching the lifter.

### FP/SIMD: exact by model, fast by proof
The console's floating point is IEEE binary32/64 under FPCR control:
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

### Threads and the scheduler
Each guest thread is a host thread with its own `Ctx` and a guest stack.
`nn::os` objects (threads, mutexes, condition variables, events,
semaphores, message queues, timers) are host objects behind the guest
structures the game allocates. Two scheduling modes, chosen per game:
- *free*: host threads run when the host runs them;
- *console*: three cores, each running only its highest-priority ready
  thread, with the game's core masks and priorities. Published
  recompilations needed this for stability (allocator races during
  loads under free scheduling), so it exists from the first game, as
  run tokens per core that a thread holds while it runs guest code and
  releases on every blocking call.
`tpidr_el0` points at the guest TLS block the SDK expects, built by the
runtime from the module's TLS template.

### HLE boundary: the SDK by name, the kernel by number
Games import nnSdk from the `sdk` module by mangled symbol name, and
that is the HLE boundary: a handler is registered under the symbol
(`HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", ...)`), takes its
arguments the AAPCS64 way (`x0`-`x7`, `v0`-`v7`, then the guest stack)
through typed helpers, and returns in `x0`/`v0`. Imports without a
handler bind to a fatal stub that names them; there are no silent
default stubs. Statically linked SDK and middleware code (nn::gfx,
nn::ui2d, physics, audio middleware) is simply lifted and runs until it
reaches an import or a supervisor call.

Some SDK code is cheaper to lift than to reimplement: the audio
renderer and its DSP commands, allocators, the vibration player.
`sdklift` roots in the manifest lift those functions from the `sdk`
module with the game; lifted SDK code then reaches the kernel, so the
runtime has a supervisor-call table for what it needs: sleep, the lock
arbiter and process-wide keys (`nn::os` mutexes and condition variables
are built on them), memory queries and the clock. A supervisor call that
would need an IPC session (`svcSendSyncRequest`) is fatal and names the
service and command: services are reimplemented at the SDK API, never
over IPC. Games without a `sdk` module (statically linked SDK) are
handled by `replace` at the SDK functions' addresses, found by symbol
or signature in the game project.

### Graphics: NVN on the host GPU
NVN is a thin command-buffer API whose shaders are Maxwell machine
code; the game obtains its entry points through `nvnBootstrapLoader`
and `nvnDeviceGetProcAddress`, which the runtime answers with thunks.
NVN is reimplemented at the API level (`src/horizon/hle/nvn_*`): command
buffers are recorded on the game's threads and replayed on one render
thread, queues, syncs and memory pools map to host objects, and texture
layouts (block-linear, BCn, ASTC) are decoded by our own code.
- Two backends behind one internal device interface shaped by NVN's
  needs (compute, geometry stages, texture views, many bound textures,
  multiple viewports, bindless-style texture handles): Vulkan on Linux
  and macOS is the first; native Metal is the second and is required.
  SDL_GPU is not enough for NVN (no geometry stage, limited binding
  model), so unlike the x86 path this tree talks to Vulkan and Metal
  directly.
- Shaders: `src/horizon/maxwell` decodes Maxwell SASS, builds
  structured control flow and emits GLSL; shaderc compiles it to SPIR-V
  and SPIRV-Cross derives MSL. Translation is cached per shader hash
  under the user data directory and can be run offline over every
  shader in a game's files (`horizon_shaderc`) as a test. References:
  Ryujinx's shader project and LASRecomp's translator (both MIT, read),
  NVIDIA's public Maxwell documentation (cited per instruction).

### Audio, video, input, files
- `nn::audio`: output through SDL3 audio; the renderer is lifted SDK
  code when the game uses it (above), otherwise the API is answered by
  our own mixer. Exactness is checked by rendering captured command
  streams against the lifted renderer.
- `nn::movie`/`nn::mmv`: H.264 through ffmpeg into NVN textures.
- `nn::hid`: SDL3 gamepads, keyboard and mouse as the console's
  controller styles; motion and vibration where SDL exposes them;
  `<ID>_BACKGROUND` keeps automated runs off the host's input.
- `nn::fs`: the RomFS directory read in place; save data under the user
  data directory with the console's two-phase commit, so a killed
  process leaves a whole save. Mount names, path rules and error codes
  are documented in docs/fs.md as observed.
- `nn::oe`, `nn::vi`, `nn::applet`: one window, 60 Hz pacing from one
  clock that drives both the game's vsync event and presentation, the
  operation modes (handheld/docked) as a setting.

### C++ exceptions through lifted code
Games throw and catch C++ exceptions through SDK code (`__cxa_throw`
and the unwinder are `sdk` imports). Lifted frames are host frames, so
host unwinding cannot find the guest's landing pads. The runtime's
`__cxa_throw` handler walks the guest stack with the modules' own
`.eh_frame` and LSDA tables to find the handling frame and landing pad,
then throws a host exception carrying the target; the lifted function
that owns that frame catches it and continues at the landing pad with
the callee-saved registers restored per the CFI. This is the x86 path's
`GuestResume` for SEH, generalised to table-driven unwinding; the lifter
emits the catch only in functions whose LSDA has landing pads.

### Platform layer
`src/a64/platform.{h,cpp}` (core) holds what POSIX does not cover the same
way on Linux and macOS (fixed-address mappings and their page sizes,
thread names and priorities, the fault handler's context, the user data
directory); the rest of the runtime has no OS #ifdefs.

### Diagnostics
Every fatal names the guest VA and symbol (`main+0x1234
nn::gfx::...`), prints the guest registers, the guest backtrace from
frame records and the host stack, and writes a crash file under the user
data directory. The watchdog reports every thread's position when no
frame has been presented for five seconds. `<ID>_TRACE` traces HLE calls
by family; `<ID>_SCHED` logs scheduler decisions.

## Milestones
1. `recomp_lift_a64` lifts a homebrew NRO built from checked-in assembly
   and C (no SDK, supervisor calls only) and the differential tests pass
   against Unicorn on x86-64 and the CPU on arm64. Done on
   x86-64 and on arm64 under qemu-aarch64 (the `horizon_lift_*` tests);
   QEMU's CPU is emulated, so real arm64 hardware is still to run.
2. A game's `main` lifts whole; with `sdk` imports bound to fatal
   stubs the game reaches `nnMain`, mounts RomFS and reads its first
   files through `nn::fs` HLE.
3. NVN thunks and the Vulkan backend present the first frame; the Metal
   backend presents the same frame on macOS.
4. The Maxwell translator passes the offline run over every shader in
   that game's files; the game plays.
5. Audio by lifting the SDK renderer; scripted runs as the oracle for
   replacements, as in the PC recomps.
