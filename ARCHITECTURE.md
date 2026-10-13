# Architecture

x86recomp turns a 32-bit x86 Windows game into a native 64-bit program:
the game's machine code is lifted to C++ ahead of time and linked with a
runtime that reimplements the Windows, DirectX and middleware APIs it
calls (HLE). It was built for Dungeon Siege (DS1-Rebuilt) and is shared
with the Call of Duty port; the game projects hold everything specific
to one game (entry point config, manifest, symbols, game hooks).

## Scaffold, not the destination
Recompilation is how a game first runs natively, not how it stays. The
end state of each port is native source: lifted functions are replaced
by hand-written C++ one subsystem at a time until nothing lifted is
left, and the lifter, guest arena and HLE fall away with them. That
shapes the design:
- The lifted code stays exact (integer flags, x87, SSE, MXCSR), so it is
  the reference every hand-written replacement is diffed against.
- Replacements must be able to take over any function by address and
  work on the same guest state while their neighbours are still lifted.
- Nothing that only the scaffold needs (CPUID answers, the guest heap,
  thunks) should leak into the native code that replaces it.

## Goals and hard constraints
1. Native Linux first; macOS on Apple Silicon with a native Metal
   renderer is a requirement, not an afterthought. Nothing in the lifter
   output or the runtime may assume an x86 host, 4 KiB pages, or
   Linux-only APIs outside a platform layer.
2. No emulation in the shipping binary: no interpreter, no JIT, no
   runtime x86 decoding, no fallback path. Every guest instruction that
   can execute is lifted to C++ ahead of time. An indirect branch to an
   address that was not lifted is a fatal error (reported with its VA so
   it can be added to the manifest), never a slow path.
3. No Wine, in any form, in the product: the Win32/DirectX surface games
   use is reimplemented natively. Wine (with the original EXE) is allowed
   only as a dev-time reference to compare behaviour, and is never
   linked or shipped.
4. 64-bit everywhere on the host: host code, the runtime and every
   library are 64-bit; nothing needs i386 multilib.
5. Users bring their own game: no game code or data in any repository.

```
game.exe ──Ghidra──> functions.tsv, jump tables ─┐
     │                                           ▼
     └──────── exports ──────────────>  recomp_lift (x86-32 -> C++)
                                                 │
                                  generated/*.cpp (gitignored, per game)
                                                 │
   ┌────────── game binary (native 64-bit, portable C++) ─┴───────────┐
   │ lifted game code ── guest arena (4 GiB, base chosen at startup)   │
   │        │ calls to import thunks / COM vtables (0x7F000000+)       │
   │        ▼                                                          │
   │ HLE: kernel32 user32 gdi32 advapi32 ole32 mss32 binkw32 ddraw ... │
   │        │          │                 │                 │           │
   │     POSIX       SDL3            gfx::Device       SDL3 audio,     │
   │                (window,     ├ d3d9 (dxvk-native)  ffmpeg          │
   │                 input)      └ SDL_GPU: Vulkan / Metal (planned)   │
   └───────────────────────────────────────────────────────────────────┘
```

## What "64-bit" means here
The host process, the runtime and all libraries are 64-bit. The game's
own data layout stays 32-bit: its structures contain 32-bit pointers,
its heap lives in a 4 GiB arena, and the lifted code computes 32-bit
addresses, because that is what the compiled machine code does.
Recompilation keeps the program's semantics; it does not retype its
data. The route to anything beyond that is incremental: replace lifted
functions with hand-written C++ one subsystem at a time.

The arena is not emulation: it is how the program's address space is
represented. A guest load is one add of a base register plus the load
itself, on both x86-64 and ARM64.

Host libraries (SDL3, dxvk/SDL_GPU, ffmpeg, libc) are ordinary 64-bit
libraries and never see guest addresses. The HLE layer is the only
place where the two worlds meet:
- Guest -> host: a guest pointer argument becomes `base + addr`
  (e.g. CreateFileA's path, a vertex array passed to DrawPrimitive).
  Little-endian on both sides, so data is used in place, no copies.
  Structs that contain pointers are read through guest-layout mirror
  types whose pointer fields are `u32`.
- Host -> guest: host objects are never exposed as 64-bit pointers.
  Kernel objects (files, events, threads) become 32-bit handles indexing
  a host table. COM objects are small guest-memory objects whose vtables
  point at thunk addresses and whose payload holds a host table index.
  Memory the game must read or write (Lock() results, MapViewOfFile
  views, heap blocks) is placed inside the arena.

## Game projects
A game project adds this repository with `add_subdirectory()`, links
`recomp_runtime` plus the optional HLE modules its EXE imports
(`recomp_mss32`, `recomp_bink`, `recomp_d3d7`, `recomp_dbghelp`), adds
its lifted code with `recomp_add_lifted()`, and fills a
`recomp::GameConfig` in its `main()`:
- `id`: per-user data directory ($XDG_DATA_HOME/<id>) and the prefix of
  dev/test switches (<ID>_TRACE, <ID>_TEST_INPUT, ...);
- `guest_dir`, `exe_name`: where the game believes it is installed;
- `registry`: what the retail installer would have written;
- hooks for the rare behaviour that needs game knowledge (e.g.
  `game_cursor` for games that re-centre the pointer).

Game-specific patches to lifted code (fixes, enhancements such as
widescreen) live in the game project as hand-written replacements of
specific lifted functions. `replace <va>` in a manifest makes the lifter
emit that function's body as `f_<VA>_lifted`; every call site and the
dispatch table still reach `f_<VA>`, which the game project defines (in
`recomp::gen`) and which may call the lifted body.

## Decisions

### Guest arena
One 4 GiB reservation; guest VA A lives at host `base + A`. The image is
mapped at its real VAs (0x400000..), so every constant address in the
code is valid unchanged. `base` is chosen at startup (preferred
0x10_0000_0000, which is free on Linux and outside macOS's 4 GiB
__PAGEZERO and main-image range) and read from a global by lifted code.

Win32 memory semantics (4 KiB commit granularity, 64 KiB allocation
granularity, page protections, VirtualQuery) are tracked in a per-guest-
page table. Host protection is only "accessible or not" at host page
granularity (4 KiB on x86 Linux, 16 KiB on Apple Silicon and Asahi), so
nothing depends on host page faults for guest behaviour.

### Address-space headroom: 2 GiB -> ~4 GiB (planned, opt-in)
EXEs that are not large-address-aware never saw an address at or above
0x80000000 on Windows, and their code may rely on that. Until audited
per game, the guest allocator hands out only addresses below 0x80000000
(the thunk region at 0x7F000000 stays there either way). Then:
1. Audit for code that breaks with high addresses: signed compares or
   sign-extending shifts on pointers (`cmp ptr; jl/jg`, `sar`, `cdq`
   on addresses), "negative means error" checks on pointer-or-error
   returns, top-bit tagging of pointers, pointer differences stored in
   signed ints, and address-to-int conversions in hashing/sorting.
2. Add a top-down allocation stress mode (like Windows'
   AllocationPreference=MEM_TOP_DOWN test for LAA apps): every
   VirtualAlloc/heap/MapViewOfFile result is placed as high as possible,
   so bugs show up at startup instead of hours into a session.
3. Fix what the audit finds with manifest overrides or host
   replacements, then allow allocations up to 0xFFFE0000 behind a config
   flag. It becomes a game's default only after a full playthrough in
   stress mode.

### Lifter: x86-32 -> portable C++
No maintained tool lifts Win32 x86 PE to portable C. Early-2000s MSVC
code is integer + x87, ~190 mnemonics, few SEH frames, so a purpose-built
lifter is tractable (rexglue-style: one C++ function per guest function).

- Decoder: Zydis (fetched via CMake FetchContent), build-time only.
- Function list: Ghidra functions.tsv + exports + a per-game line-based
  manifest (lifter.manifest) for overrides: extra entries, names, entry
  probes, SEH resume sites.
- Output is plain C++20: no x86 intrinsics, no inline asm, no
  `long double`, no reliance on host FPU state.
- Context: `struct Ctx { u32 eax..edi, esp, flags; X87 fpu; fs base }`,
  one per guest thread. Registers are cached in locals per function and
  flags computed eagerly into locals (clang drops dead ones).
- Calls: direct `call` -> C++ call; the real return address is pushed to
  the guest stack so code that reads it (__alloca_probe, SEH) still
  works. `ret N` pops. Tail `jmp` to another entry -> tail call.
- Indirect calls/jumps: dense table indexed by (va - .text start);
  addresses in the thunk region go to HLE handlers; anything else is
  fatal (constraint 2).
- SEH: the fs:[0] chain lives in guest memory as on Windows.
  RaiseException walks it and calls the lifted handlers. Resuming at a
  continuation (catch/except target) throws a host exception carrying
  {target va, esp, ebp}, caught by the lifted function that owns the
  target label.
- Oracle tests (dev-time only): every x87 and integer instruction form
  found in a game EXE is run lifted and natively from the same random
  state (tests/lift_*_test.cpp, `recomp_add_lift_tests`).

### Multiple modules
Games that ship their logic in DLLs they LoadLibrary themselves (Call of
Duty: gamex86/cgamex86/uix86.dll) have every module lifted:
- One lifter run takes the EXE plus each DLL (`--module`, with its own
  symbols and manifest). The images are mapped side by side as at run
  time, so a VA, and the f_<va> name of its lifted function, identifies
  its module; table.cpp lists them all. Pointer scans only accept
  targets in the module the pointer was found in.
- A DLL loads at its preferred base. If two modules overlap, the lifter
  refuses and one gets `--base`: it is lifted from its relocated image,
  and the game's GameConfig names the same base, because lifted code has
  the module's addresses compiled in. A mismatch is caught at startup
  (the DLL's entry point is not in the function table).
- GameConfig::lifted_dlls lists them. All are mapped at startup (their
  ranges are reserved before the allocator hands out memory); imports
  are bound to HLE thunks like the EXE's. The dispatcher keeps one dense
  table per module's .text.
- LoadLibrary counts references and runs DllMain under a loader lock;
  GetProcAddress returns the real export VA (by name or ordinal),
  GetModuleHandle/GetModuleFileName know the module while it is loaded.
  After the last FreeLibrary (DllMain detach) the next load maps the file
  afresh, so the DLL starts from its initial data as after a real unload.
- Static TLS in a lifted DLL is refused: Windows XP did not set it up for
  LoadLibrary'd DLLs, so DLLs of that era have none.
  DLL_THREAD_ATTACH/DETACH are not sent (not needed so far).

### x87: full 80-bit model
Games depend on x87 state (Dungeon Siege runs all its float maths under
PC=24/RC=chop; MSVC CRT helpers switch precision around their own work;
D3D changes the control word), so the FPU is modelled exactly, not
mapped to double.

Model per thread: 8 registers of 80 bits (sign, 15-bit exponent,
64-bit explicit-integer mantissa), TOP, tag word, control word (PC, RC,
exception masks) and status word (C0-C3, sticky exception flags, stack
fault). Semantics to honour:
- PC rounds the significand of fadd/fsub/fmul/fdiv/fsqrt (and their
  pop/reverse/integer-operand forms) to 24/53/64 bits while the exponent
  keeps the full 15-bit range (so PC=24 does not flush to float range).
  Loads, stores, transcendentals and fprem are unaffected by PC.
- RC applies to every rounding: arithmetic, fst/fstp to m32/m64,
  fist/fistp, frndint.
- fist/fistp out of range stores the integer indefinite (0x80000000 /
  0x8000 / 0x8000000000000000); masked invalid sets IE.
- fcom/fucom/ftst/fxam set C0/C2/C3 for the `fnstsw ax; sahf` idiom,
  NaN/unordered included.
- New threads start with the Windows default CW 0x027F (PC=53,
  RC=nearest, all exceptions masked), independent of the creator.

Implementation: Berkeley SoftFloat 3e `extF80` (BSD-3; has
`extF80_roundingPrecision` 32/64/80 and all four rounding modes, and is
portable C, so it works on ARM64 where `long double` is only 64 bits).
SoftFloat has no transcendentals: fsin, fcos, fsincos, fptan, fpatan,
fyl2x, fyl2xp1, f2xm1 and fscale have our own extF80 implementations
(reference: Bochs' x87 code, LGPL, read only).

Fast paths are integer-only (no host FPU) and provably identical to the
SoftFloat path they bypass, which stays the reference
(tests/x87_fast_test compares them; x87_test and lift_x87_test compare
against the hardware):
- add/sub/mul/div of normal or zero operands: exact 128-bit result,
  rounded by the model's RoundPack;
- PC=24 add/sub/mul of two 24-bit significands (any RC): exact in 64
  bits (`run::Fast24`, shared with lifted runs);
- FST m32/m64 of values that stay normal;
- fsin/fcos/fsincos: series in 128-bit fixed point, used only when the
  result is further than 2^-100 from any rounding boundary.

### x87 runs (lifter)
Calling the model per instruction made every instruction a serial
read-modify-write of TOP, the tag word and the status word in memory,
and games run millions of x87 instructions per frame (CPU skinning,
particles). The lifter therefore turns straight-line x87 code into runs:
- A run is a stretch of one basic block (no labels inside, no calls or
  branches; other instructions may be interleaved) with at least three
  x87 instructions, all of a supported subset (loads, stores, the
  arithmetic forms, fxch, fchs, fabs, fsqrt, fcom family, ftst, fnstsw).
- A symbolic stack (X87Sim) resolves every st(i) to an entry-relative
  slot at lift time; slots become locals (`run::V`, padding-free so they
  stay in registers), fxch and fld st(i) become copies.
- One guard at the start (`run::Guard`): all exceptions masked, every
  slot the run reads before writing is non-empty and every slot it
  pushes into is empty. Then no instruction of the run can fault, so
  the value-level `run::` functions (the same code the State-based
  operations use) give exactly the per-instruction results; sticky flags,
  C1 and C3/C2/C0 are tracked in locals; fnstsw inside a run builds the
  status word from them.
- At the end the registers, tags, TOP and status word are written back.
  If the guard fails the original per-instruction code runs instead
  (it is emitted in the else branch).
- Common PC=24 operations, m32 loads and exact m32 stores are inlined
  into the lifted code; the rest are calls.
- <ID>_X87_RUNS=0 disables runs at run time (A/B checks).
- Known difference from hardware, the same one lifted integer registers
  already have: while a run is in flight its state is in locals, so a
  guest exception raised inside it sees the FPU state from the start of
  the run in its CONTEXT, and an __except unwind out of the run drops
  the run's changes.

### SSE/SSE2
Every PC these games still run on has SSE2, so their SSE paths (MSVC's
SSE2 maths library, engine SIMD loops) are what they actually execute;
CPUID reports a fixed SSE2-class CPU (Pentium M, family 6 model 13),
never the host's: lifted code must behave the same on every machine,
Apple Silicon included (it has no CPUID), and nothing is claimed that
the lifter does not implement (no SSE3+). The XMM registers and MXCSR
live in Ctx and are used in place (never cached in locals).
- Float lanes go through SoftFloat with the SSE NaN rules applied in
  runtime/sse.cpp (the shared SoftFloat build carries the x87 ones):
  one NaN operand is returned quieted, two give the first source, SNaN
  raises IE; invalid operations give the default NaN. Rounding control,
  every sticky flag (IE DE ZE OE UE PE) and precedence (a zero divisor
  suppresses DE for a denormal dividend) match the hardware.
- Not modelled, fatal if reached: unmasked SIMD exceptions (#XM), and
  FTZ/DAZ actually changing a result.
- Integer and data-movement forms are plain portable C++.
- tests/lift_sse_test runs every SSE form in a game EXE lifted and
  natively from random states rich in special values (2.44M checks over
  Call of Duty's 244 forms, all bit-exact).
- In the native port these sites go away: CRT maths becomes the host
  libm, engine SIMD becomes portable C++ (or SIMDe/sse2neon where speed
  matters), validated against the lifted reference.

### Graphics: API HLE over gfx::Device
Games' graphics APIs are implemented as HLE over one backend-neutral
fixed-function device, `gfx::Device` (src/runtime/gfx/device.h, D3D7
semantics: render and texture-stage states, transforms, lights, FVF
draws). Backends:

1. d3d9 via dxvk-native (Linux bring-up). D3D9 still has the D3D7
   fixed-function pipeline and DXVK implements it, so this backend is
   mostly 1:1 state translation and gets pixels on screen fastest. It
   is also the reference image for backend 2.
2. SDL_GPU with our own fixed-function shader generator (planned):
   Vulkan on Linux and native Metal on macOS (shaders compiled from one
   source to SPIR-V and MSL at build time). This is the long-term
   backend on both platforms. dxvk-native cannot be it: DXVK on macOS
   would need MoltenVK, which lacks features DXVK requires, and would
   not be native Metal anyway.

API front ends:
- DirectDraw 7 / Direct3D 7 (recomp_d3d7): guest-visible COM objects;
  DirectDraw (surfaces, flipping chain, Lock/Blt, colour keys, display
  modes) implemented in the HLE itself.
- OpenGL 1.x (planned, for Call of Duty): opengl32/wgl HLE as a GL
  state machine on gfx::Device, not a pass-through to host OpenGL, so
  the same Metal path serves it (macOS OpenGL is deprecated and capped
  at 2.1 for legacy contexts).

D3D7 FPU side effect: the HLE must reproduce whatever the real runtime
does to the guest thread's CW, because the game's own float maths runs
on that state. DDSCL_FPUSETUP = the app keeps the FPU in D3D's preferred
state; DDSCL_FPUPRESERVE = D3D saves and restores the CW around each
call; without either, the Windows runtime switches the thread to single
precision and leaves it there. Wine's ddraw does not model that default,
so Wine is not a reference for this point. d3d7.cpp models it.

### HLE surface
- kernel32: heap/virtual memory in the arena, files with case-insensitive
  path mapping onto the game dir, MapViewOfFile as mmap into the arena,
  threads (one Ctx + guest stack each), events, mutexes, critical
  sections, TLS (TEB at a fixed guest VA, fs:[0x2c]), time, locale.
- user32/gdi32: SDL3 windows; SDL events translated to WM_* and fed to
  the guest WndProc through PeekMessage/DispatchMessage.
- advapi32: registry backed by a small file, seeded per game.
- ole32: CoCreateInstance for what games need (fails cleanly otherwise).
- mss32 (Miles): our own mixer on an SDL3 audio stream (runtime/audio):
  resampling, loop counts, gain ramps, the "Miles Fast 2D Positional
  Audio" model, MP3 through libavcodec. Miles' EAX providers (EAX 1.0,
  2.0, 3.0, with Msseax.m3d's attribute names and struct layouts): voices
  pan like DirectSound3D hardware and get an LF level and a 5 kHz level
  (a two-pole low-pass) on the direct path and the send from the EAX
  source model (runtime/audio/eax), and the send goes to a reverb bus
  rendered by OpenAL Soft's EFX EAX reverb on a loopback device; direct
  sound never leaves our mixer, so the Fast 2D path is unchanged. Miles 6.5's digital
  master reverb (room types, reverb time/predelay/damping, driver and
  sample/stream wet levels) is our Miles-compatible Schroeder reverb
  (runtime/audio/digital_reverb), written from a behavioural description
  of mss32 6.5e's (Call of Duty's FINDINGS.md) and bit-exact with it:
  recomp_add_digital_reverb_test lifts the original routines from the
  user's installed DLL at build time (`recomp_lift --only`) and compares;
  tests/digital_reverb_test checks a hash of our output on every host.
  runtime/audio is its own library (`recomp_audio`, no HLE);
  tools/eax_capture renders with it offline to compare against
  recordings of real Creative hardware.
- Measured against hardware (an Audigy 2 ZS under Windows 98 SE through
  Miles 6.5e's Msseax.m3d; tools/eax_capture): where EAX leaves behaviour
  to the card, the model follows that card (details next to the code):
  the HF low-pass, occlusion on the room path, size scaling, EAX 1.0's
  table and controls, read-backs, DS3D panning, air absorption. Still
  different (hardware minus render over the capture): OpenAL's late
  reverb builds up over 20-30 ms where the card's starts at full level
  (early -1.5 dB, late +1 dB, onset found at the reflections); RT at
  4 kHz is 0.4 s off on average and long bright presets (drugged, dizzy)
  get half the card's, as OpenAL loses HF per pass through its lines;
  the card's HF damping also shortens the mid band (decay HF ratio 0.2:
  0.85 s against 1.45); air absorption's reverb level matches in total,
  not per band; reverb delays over 50 ms come out shorter on the card;
  EAX 1.0's damping and decay-time effects rest on two settings each.
- binkw32: ffmpeg (Bink demuxer from memory, binkvideo/binkaudio,
  swscale), audio into the Miles mixer.
- dbghelp: UnDecorateSymbolName (LLVM's Microsoft demangler, normalised
  to MSVC's output); the rest stubbed.
- shfolder, wsock32 (host-name queries), imm32, version: small shims.

Every import needs an explicit handler; an unhandled one is fatal by
design. All of it is written against POSIX + SDL3 + gfx::Device, with
platform specifics (mmap flags, paths, page size) in one place.

### Distribution: users bring their own EXE
The lifter is a permanent build stage: a game's logic in the port is its
output, so it is re-run whenever its semantics, the manifest or the
function list change. It never ships inside the binary. What can be
published: this library, game projects (manifests, hooks, replacements,
docs). What is never published or committed: game EXEs/DLLs/data,
generated/ (lifted C++ is derived from the EXE), symbols/, Ghidra
projects, memory dumps, extracted archives.

Licence hygiene: dxvk (zlib), SDL3 (zlib), SoftFloat 3e (BSD-3), Zydis
(MIT) are fine to vendor or link. ffmpeg is LGPL (built without GPL
parts): link dynamically. Wine and Bochs (LGPL) and GPL engine sources
(ioquake3, iortcw) are references only: read them to learn behaviour, do
not copy code from them.

### Known macOS risk: memory ordering
x86 is TSO; ARM64 is weakly ordered. Lifted code that shares data
between threads without a lock would be correct on x86-64 hosts and
racy on Apple Silicon. Plan: every HLE synchronisation call (critical
sections, events, Interlocked*, waits) is a full barrier, `lock`-prefixed
instructions become seq_cst atomics, and any lock-free hand-off found in
a game's threads gets explicit barriers via the manifest. Revisit before
the macOS port.

## Roadmap
- opengl32/wgl HLE on gfx::Device (Call of Duty).
- EAX for games on DirectSound3D (IDirectSound3DBuffer property sets),
  on the same EAX model and reverb buses as the Miles providers.
- SDL_GPU backend (Vulkan) with the fixed-function generator, matched
  against the d3d9 backend.
- macOS arm64: Metal via SDL_GPU, platform layer (signals, ucontext,
  mmap), memory-ordering audit.
