# Architecture

x86recomp turns a 32-bit x86 Windows game into a native 64-bit program:
the game's machine code is lifted to C++ ahead of time and linked with a
runtime that reimplements the Windows, DirectX and middleware APIs it
calls (HLE). Nothing here is specific to one game: game projects hold
that (entry point config, manifest, symbols, game hooks) and use this
tree as a library.

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
   │                 input)      └ SDL_GPU: Vulkan, Metal              │
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
__PAGEZERO and main-image range; macOS may place it elsewhere, the hint
being advisory there) and read from a global by lifted code.

Win32 memory semantics (4 KiB commit granularity, 64 KiB allocation
granularity, page protections, VirtualQuery) are tracked in a per-guest-
page table. Host protection is only "accessible or not" at host page
granularity (4 KiB on x86 Linux, 16 KiB on Apple Silicon and Asahi), so
nothing depends on host page faults for guest behaviour, with one
exception: games that demand-page reserved memory from an exception
filter (commit and fill one page per access violation). On a host with
larger pages, committing one guest page makes its whole host page
accessible, and the other guest pages in it would never fault. So after a
handler continues from a fault, the uncommitted guest pages of the same
reservation in that host page are faulted in too (read faults at their
first byte), before the faulting instruction is retried.

### Platform layer
src/runtime/platform.{h,cpp} holds what POSIX does not cover the same way
on Linux and macOS; the rest of the runtime has no OS #ifdefs:
- the guest main thread's stack switch (ucontext, deprecated but working
  on Darwin, where it needs _XOPEN_SOURCE);
- decoding a fault's signal context (write bit, pc, caller);
- leaving a fault with a C++ exception. glibc's unwinder steps through
  the kernel's signal frame, so a guest exception raised while an access
  violation is delivered (an __except unwind) is thrown straight from
  the handler. Darwin's unwinder cannot leave _sigtramp: there the
  handler catches the exception, rewrites the signal context so that the
  thread "calls" a rethrow function from the faulting instruction, and
  returns. That makes the faulting function an ordinary caller frame,
  which needs a frame record even in leaf functions: lifted code and the
  x87 model are built with -mno-omit-leaf-frame-pointer on Apple hosts
  (the link register is overwritten);
- stat's time fields and the per-user data directory
  (~/Library/Application Support/<id> on macOS).

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
  probes, resume sites (SEH, longjmp), setjmp functions.
- Output is plain C++20: no x86 intrinsics, no inline asm, no
  `long double`, no reliance on host FPU state other than the default
  environment (x87 runs on host doubles).
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
  {target va, ebp, from va}, caught by the lifted function that owns the
  target label.
- Non-local exits (`resume <va>` in the manifest, on a call or an
  indirect jmp): the call or jmp throws GuestResume{target, ebp, from}
  with the guest registers spilled to Ctx, instead of calling. Every
  function that writes fs:[0] or calls a setjmp wraps its body in
  try/catch; the activation whose EBP local equals the thrown ebp catches
  it and runs the target with Resume(): Dispatch to the target entry,
  repeated for any later GuestResume into the same frame (one thrown
  from inside the catch handler would otherwise leave the function's try
  block). The continuation is a separate lifted function; its final
  `ret` returns through Dispatch and the owning function returns, so the
  host stack keeps mirroring the guest's.
  - SEH: the site is __except_handler3's call into the __except block,
    which starts with `mov esp, [ebp-18h]`; the target is an entry
    because the handler table holds its address.
  - setjmp/longjmp: `setjmp <va>` names the CRT's setjmp (e.g. MSVC's
    __setjmp3). The return address of each direct call to it becomes a
    strong entry (the caller's body tail-calls it after setjmp returns
    0) and the caller gets the try/catch. `resume <va>` goes on longjmp's
    final `jmp [jmp_buf+14h]`, reached after it has unwound the SEH chain
    and restored ebp/ebx/esi/edi/esp from the jmp_buf, so the thrown ebp
    is the setjmp caller's. Matching by EBP assumes the setjmp caller has
    a frame of its own or that no deeper function with the same EBP also
    catches; an FPO setjmp caller inherits its caller's EBP. Calls to
    setjmp through a pointer are not recognised.
- Oracle tests (dev-time only): every x87 and integer instruction form
  found in a game EXE is run lifted and natively from the same random
  state (tests/lift_*_test.cpp, `recomp_add_lift_tests`).

### Multiple modules
Games that ship their logic in DLLs they LoadLibrary themselves (e.g.
separate game, client-game and UI modules) have every module lifted:
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
Games depend on x87 state (some run all their float maths under
PC=24/RC=chop; MSVC CRT helpers switch precision around their own work;
D3D changes the control word), so the FPU is modelled exactly. Host
doubles are used only where they provably give the same results (x87
runs on host doubles, below).

Model per thread: 8 registers of 80 bits (sign, 15-bit exponent,
64-bit explicit-integer mantissa), TOP, an empty bit per register,
control word (PC, RC, exception masks) and status word (C0-C3, sticky
exception flags, stack fault). The other tags (valid, zero, special) are
not stored: they always equal the class of the register's contents
(every write sets it, and FLDENV/FRSTOR keep only "empty" from the
image), so the tag word is derived when FNSTENV/FNSAVE store it;
x87_test compares stored tag words with the CPU's. Semantics to honour:
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
- the two modes games run in, with the operands they produce, inline
  (`run::Fast24` and `run::FastWide`, x87.h, shared by the model and
  lifted runs): PC=24 (any RC) add/sub/mul/div of 24-bit significands,
  exact in 64 bits (a quotient keeps its remainder as the sticky bit),
  and of up to 53-bit ones (m64 operands); PC=53 (any RC, Windows'
  default) add/sub/mul/div of up to 53-bit significands (m32/m64 loads
  and PC=53 results), exact in 128 bits. A zero operand is exact. They
  give up (to the paths above) on specials, denormals, exponents too far
  apart, results leaving the normal range and division by zero;
- FLD/FST m32/m64 of zero and normal values (FST rounding with RC, the
  result staying normal), and FCOM of zero and normal values (an integer
  compare of exponent and significand), inline;
- fsin/fcos/fsincos: series in 128-bit fixed point, used only when the
  result is further than 2^-100 from any rounding boundary.

### x87 runs (lifter)
Calling the model per instruction made every instruction a serial
read-modify-write of TOP, the empty bits and the status word in memory,
and games run millions of x87 instructions per frame (CPU skinning,
particles). The lifter therefore turns straight-line x87 code into runs:
- A run is a stretch of one basic block (no labels inside, no calls or
  branches; other instructions may be interleaved) of x87 instructions
  of a supported subset (loads, stores, the arithmetic forms, fxch,
  fchs, fabs, fsqrt, fcom family, ftst, fnstsw). A single instruction is
  a run too: its guard and write-back cost less than the State-based
  call.
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
- At the end the registers, empty bits, TOP and status word are written
  back. If the guard fails the original per-instruction code runs
  instead (it is emitted in the else branch).
- The fast paths above (arithmetic in the two common modes, m32/m64
  loads and stores, compares) are inlined into the lifted code; the
  rest are calls.
- The control word has a 32-bit field of its own: a run's write-back
  stores the status word, the next run's guard loads the control word,
  and a load widened over both missed store forwarding (5% of a hot
  function).
- What a run costs is the latency of its dependent operations (about
  5 ns each through the integer paths, about 1 ns on host doubles), not
  the guard and write-back (~2.5 ns per run), so keeping values in
  locals across branches would gain little.
- <ID>_X87_RUNS=0 disables runs at run time (A/B checks);
  <ID>_X87_DOUBLE=0 only their host-double form.
- Known difference from hardware, the same one lifted integer registers
  already have: while a run is in flight its state is in locals, so a
  guest exception raised inside it sees the FPU state from the start of
  the run in its CONTEXT, and an __except unwind out of the run drops
  the run's changes.

### x87 runs on host doubles
Most float code runs under Windows' default control word 0x027F (PC=53,
RC=nearest, exceptions masked); a game that never sets the CW does all
its float maths there. Under it a run with arithmetic first runs on host
doubles (x87.h, "host doubles"; lifter: EmitX87Double):
- After the guard, if PC=53 and RC=nearest (run::DoubleMode), the
  registers the run reads are converted to doubles d<k> (run::ToD). The
  same instructions then run on the doubles, with PE, C1 and C3/C2/C0 in
  the run's usual locals.
- Each instruction checks before it has any effect. Its operands must be
  held exactly (see "Range"), and its result must stay in range. If a
  check fails, the run saves the instruction's index (rk) and jumps to
  XS. There the doubles become registers again (exact), and the run
  continues on the exact path at that instruction (label XR), which
  redoes it. If a register cannot be converted at entry, the whole run
  takes the exact path.
- At the end the doubles are converted back and the normal write-back
  stores them.
- Stores of a double do not exit: an m32 store outside float's normal
  range uses the exact store. FIST and the 64-bit constants (fldpi...)
  go through the exact model; FLD1 and FLDZ are 1.0 and 0.0.

Range. Every value a double holds is zero, or has a magnitude in
[2^-511, 2^512). Such a value has at most 53 significand bits and is a
normal binary64, so the double and the 80-bit register hold the same
number. Values enter only as:
- registers that are already like that (ToD);
- m32 operands (zero or normal floats are in range);
- m64 operands in range;
- FILD with |v| <= 2^53, FLD1 and FLDZ.
NaN, infinity, denormal and out-of-range operands exit.

Proof that a result in range is exact. Take a and b in range, and the
operation fadd, fsub, fmul, fdiv or fsqrt (and their reverse, pop and
integer-operand forms). Let x be the exact result.
1. Zero results. If x = 0, both give the same signed zero: IEEE 754 and
   the x87 use the same rules at RC=nearest (x - x = +0, -0 + -0 = -0,
   products and quotients take the xor of the signs).
   A nonzero x is never that small:
   - products are at least 2^-1022;
   - quotients are greater than 2^-511 / 2^512 = 2^-1023;
   - square roots are at least 2^-256;
   - a nonzero sum is a multiple of the smaller operand's quantum,
     2^-563 or more.
   So binary64 never rounds a nonzero x to zero, and a zero double
   result means x = 0.
2. Nonzero results. The x87 at PC=53 rounds x once to 53 bits, and its
   15-bit exponent cannot overflow or underflow here. Binary64 rounds x
   the same way when x is in its normal range; below 2^-1022 it gives a
   denormal, at 2^1024 and above infinity. The run keeps a result r only
   if |r| is in [2^-511, 2^512).
   - Rounding is monotonic and powers of two are representable. So
     x below 2^-1022 gives |r| <= 2^-1022, and overflow gives infinity.
     Both are rejected.
   - An accepted r therefore comes from a normal-range x and equals the
     x87 result.
   - Conversely, an x87 result in range is accepted.
   - A run exits exactly when the x87 result leaves the range. No other
     exception can occur: operands are normal or zero, and division by
     zero gives infinity or NaN, which are rejected.
3. PE and C1. Let e = x - r. PE is e != 0, and C1 ("rounded up" in
   magnitude) is e != 0 with sign(e) != sign(r). With operands and r in
   range, e is computed exactly:
   - Add/sub: TwoSum (Knuth) gives r + e = a + b exactly when no
     operation overflows. Operands below 2^512 keep every intermediate
     below 2^514.
   - Mul: e = a*b - r is a multiple of 2^(ea+eb-104), with
     ea + eb >= er - 1 >= -512. It fits in 53 bits (|e| <= ulp(r)/2), so
     fma(a, b, -r) computes it exactly.
   - Div: the remainder p = a - r*b is representable when r is the
     correctly rounded quotient and nothing underflows (Markstein;
     Boldo and Daumas). Its quantum is 2^(er+eb-104) with
     er + eb >= ea - 1. fma(-r, b, a) computes it exactly.
     x - r = p/b, so PE is p != 0. Since sign(r) = sign(a) xor sign(b),
     C1 is p != 0 with sign(p) != sign(a). Comparing with sign(r)
     instead would be wrong for negative divisors.
   - Sqrt: p = a - r^2 is a multiple of 2^(2er-104) with er >= -256.
     fma(-r, r, a) computes it exactly; C1 is p < 0.
   No error term is smaller than 2^-616 unless it is zero.
4. Stores and compares.
   - FST m64 of a value in range is exact: no PE, C1 = 0.
   - FST m32 rounds the 53-bit value once to 24 bits, as (float)r does
     at RC=nearest. That holds while |r| >= 2^-126 (the x87 checks
     tininess before rounding) and the float is finite; otherwise the
     exact store runs. PE is (double)f != r, and C1 is |f| > |r|.
   - Compares see no NaN, so host compares give C3/C2/C0; +0 and -0
     compare equal on both. FCOM m64 accepts any zero or normal value.
   - FCHS, FABS, FXCH and register copies are exact.

Host requirements:
- binary64 evaluated as written: FLT_EVAL_METHOD 0 and no fast-math
  (x87.h refuses to compile otherwise);
- no contraction into FMA: everything that runs these paths is built
  with RECOMP_X87_FP_FLAGS;
- round-to-nearest-even, the host default, never changed;
- masked traps (a division by zero computes infinity before the check
  rejects it).
No denormal is ever an operand or a result, so FTZ and DAZ could not
change anything, but platform::HostFpProblem rejects them anyway. Every
thread that runs guest code checks it at start: the rounding mode, the
control register (MXCSR on x86-64, FPCR on arm64), and the behaviour.
std::fma is a single instruction on arm64. On x86-64 without -mfma it is
glibc's fma, which is correctly rounded too.

Tests and measurements:
- tests/x87_fast_test fuzzes every function against the SoftFloat path
  at 0x027F. A function must succeed exactly when the exact result can
  be held, and then match value, flags and C1 bit for bit. The cases
  include range edges, ties, cancellation, signed zeros, results just
  over and under the range, float range edges for m32 stores, and any
  m64/m32/register bits for the loaders.
- Two counterexamples to weaker conditions are kept as cases:
  - "normal operands, result >= 2^-1022" is not enough:
    6361 * 2^-600 times 1416003655831 * 2^-475 is (2^53 - 1) * 2^-1075.
    Binary64 rounds it to 2^-1022 (its denormal quantum); the x87 holds
    it exactly.
  - a zero result is not exact without the range: 2^-512 * 2^-791
    gives 0.
- Mutations of each check, error term, flag and conversion make the
  test fail.
- lift_x87_test runs the lifted forms and sequences from 0x027F states
  that are often double-exact, against the CPU.
- Lifter output grows by 7-16%: the run is emitted twice, plus the exits.

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
  natively from random states rich in special values (for a game whose
  EXE has 244 forms: 2.44M checks, all bit-exact).
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
2. SDL_GPU (src/runtime/gfx/gpu_device.cpp): Vulkan on Linux, native
   Metal on macOS. This is the long-term backend on both platforms.
   dxvk-native cannot be it: DXVK on macOS would need MoltenVK, which
   lacks features DXVK requires, and would not be native Metal anyway.
   Design:
   - The fixed-function pipeline is one vertex and one fragment
     uber-shader (gfx/shaders/ff.vert, ff.frag) driven by uniforms
     holding the D3D state: vertex blending, lighting (directional, point, spot;
     material sources), texgen and texture transforms (projected too),
     vertex and table fog, all texture-stage ops and arguments, alpha
     test, flat shading, user clip planes. Nothing is compiled per state.
     The GLSL is the only source: glslang builds SPIR-V for Vulkan, and
     for Metal SPIRV-Cross converts a build with Metal's buffer order
     into MSL; both are embedded at build time.
   - Vertices are pulled from a storage buffer with the FVF's offsets in
     the uniforms, so the vertex layout is not pipeline state. Pipelines
     (blend, depth/stencil, rasterizer, topology, depth format) are
     cached by state; triangle fans become lists.
   - D3D draws arrive one at a time with their data in guest memory, and
     SDL_GPU uploads only outside render passes. Draws are therefore
     recorded per render target (vertex/index data and uniforms in CPU
     arenas) and flushed as one copy pass plus one render pass when the
     target changes, a texture in use is updated, or on readback and
     present. A full-target Clear before any draw is the pass's load op.
   - Colour textures and targets are B8G8R8A8 (the memory layout of D3D's
     A8R8G8B8); other formats convert on upload and readback, as the d3d9
     backend does for its X8R8G8B8 back buffer. DXT stays BC1-3. Blends
     reading destination alpha see 1 on targets without alpha.
   - D3D9's integer pixel centres become SDL_GPU's half-integer ones
     with a half-pixel shift in the vertex shader. SDL_GPU flips Vulkan's
     viewport, so clip space is y-up on both APIs.
   <ID>_GFX=d3d9|gpu selects the backend; the default is SDL_GPU, with
   d3d9 kept (where the build has it) as the reference image.

API front ends:
- DirectDraw 7 / Direct3D 7 (recomp_d3d7): guest-visible COM objects;
  DirectDraw (surfaces, flipping chain, Lock/Blt, colour keys, display
  modes) implemented in the HLE itself.
- OpenGL 1.x: opengl32/wgl HLE as a GL state machine on gfx::Device,
  not a pass-through to host OpenGL, so the same Metal path serves it
  (macOS OpenGL is deprecated and capped at 2.1 for legacy contexts).

D3D7 FPU side effect: the HLE must reproduce whatever the real runtime
does to the guest thread's CW, because the game's own float maths runs
on that state. DDSCL_FPUSETUP = the app keeps the FPU in D3D's preferred
state; DDSCL_FPUPRESERVE = D3D saves and restores the CW around each
call; without either, the Windows runtime switches the thread to single
precision and leaves it there. Wine's ddraw does not model that default,
so Wine is not a reference for this point. d3d7.cpp models it.

### Frame pacing
Old games limit their frame rate by spinning on a millisecond timer,
which burns a core and only reaches rates of 1000/n. `FramePacer`
(runtime/pacing.h) is for game patches that replace such a limiter:
Wait(fps) at the start of each frame sleeps until the next frame's start
on a grid of exact periods on the host's monotonic clock. Lateness does
not add up (the next start is a period after the previous one, not
after the wake-up), a frame that ends late is followed by a shorter
wait, and one more than a period late moves the grid instead of causing
a burst. The sleep is the platform layer's `SleepUntilNs`: an absolute
clock_nanosleep with the thread's timer slack at 1 us on Linux,
mach_wait_until on macOS; no spinning. tests/pacing_test checks the
rate (within 1%), the CPU use and the late-frame rule; on an idle Linux
host wake-ups land 15-30 us after the deadline.

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
  Audio" model of the game's Miles version (6.1c or 6.5; docs/miles.md),
  MP3 through libavcodec. Miles' EAX providers (EAX 1.0, 2.0, 3.0, with
  Msseax.m3d's attribute names and struct layouts): voices pan like
  DirectSound3D hardware and get an LF level and a 5 kHz level
  (a two-pole low-pass) on the direct path and the send from the EAX
  source model (runtime/audio/eax), and the send goes to a reverb bus:
  our environmental reverb (runtime/audio/reverb), rebuilt from the
  card's measured impulse responses with textbook parts (tapped delay
  lines for the reflections and the start of the late reverb, a
  feedback delay network with Jot-style absorption filters for the
  rest, an echo comb). The EAX presets are the card's own read-backs.
  The Fast 2D path is unchanged. Miles 6.5's digital
  master reverb (room types, reverb time/predelay/damping, driver and
  sample/stream wet levels) is our Miles-compatible Schroeder reverb
  (runtime/audio/digital_reverb), written from a behavioural description
  of mss32 6.5e's (docs/miles.md) and bit-exact with it:
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
  table and controls, read-backs, DS3D panning, air absorption, and the
  reverb's structure: reflection and late-burst tap patterns, the tail's
  start and level, decay per band, the broader low-pass on the send.
  Still different (hardware minus render over the capture, mean |d|: RT
  mid 0.12 s, RT 4 kHz 0.11 s, EDT 0.37 s, onset 1 ms, early 0.9 dB,
  late 1.3 dB, occlusion and air on the reverb 0.3-0.4 dB): very damped
  small rooms (carpeted hallway, living room, the 1 m environment size)
  have 7-10 dB less late reverb than the card; padded cell's early
  reverb is 7 dB low and psychotic's 5-7 dB high; echo depth is a comb
  at the echo time, where the card shortens the burst and makes the
  tail periodic (alley, echo depth cases); reverb +2000 mB is 2 dB
  louder than the card's (it seems to limit there); modulation's pitch
  deviation is a guess; the outdoor presets with long delays (forest,
  mountains, plain) are barely measurable; EAX 1.0's damping and
  decay-time effects rest on two settings each.
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
parts): link dynamically. Wine and Bochs (LGPL) and GPL engine source
releases are references only: read them to learn behaviour, do not copy
code from them.

### Memory ordering on weakly ordered hosts (macOS arm64)
x86 is TSO; ARM64 is weakly ordered. Lifted code reaches guest memory
with plain loads and stores, so data shared between threads without a
lock would be correct on x86-64 hosts and racy on Apple Silicon.

- Lifted `lock`-prefixed instructions and xchg are seq_cst atomics. On
  Apple targets (LSE always present) they compile to `casal`, `ldaddal`,
  `swpal`, which are fully ordered, like the x86 instructions. Built
  for an ARMv8.0 target without LSE they would be LL/SC loops, which are
  not full barriers for the surrounding plain accesses.
- HLE synchronisation calls are full barriers, as on Windows (a locked
  instruction or a kernel entry there). The host mutex or condition
  variable behind most of them gives acquire/release between threads
  (what one thread wrote before releasing is visible to the next one
  that acquires), but lock + unlock is not a full barrier: accesses
  outside it may move into the critical section and pass each other.
  So each handler opens a `GuestSyncPoint` (kernel32_state.h): a seq_cst
  fence on entry and on exit, early returns and ExitThread's unwind
  included. That covers SetEvent, ResetEvent,
  CreateMutexA (it may take ownership), ReleaseMutex,
  WaitForSingleObject, WaitForMultipleObjects (and through it
  MsgWaitForMultipleObjects), InterlockedIncrement/Decrement/Exchange
  (seq_cst RMWs, which C++ orders only against other seq_cst atomics),
  CreateThread, ResumeThread, ExitThread, TlsAlloc, TlsFree and Sleep
  (polling loops). A thread's start fences before its first guest
  instruction (pthread_create already publishes the creator's writes)
  and its exit fences before it is signalled. TlsGetValue/TlsSetValue
  touch only the caller's TEB and are plain accesses on Windows too.
- EnterCriticalSection/LeaveCriticalSection run Windows XP's algorithm
  on the guest structure: each is one seq_cst read-modify-write of
  LockCount (`ldaddal`/`casal` on Apple targets, a locked instruction
  on x86-64), fully ordered like the guest's own, so no separate fence;
  a contended Enter waits on a host semaphore that the releasing Leave
  signals.
- Window messages (PostMessageA, PeekMessageA, GetMessageA) go through
  the user32 queue mutex: acquire/release, so a message's payload
  written before posting is visible to the receiver.

Still open: a lock-free hand-off in a game's own threads (a plain flag
polled without any of the calls above) needs explicit barriers via the
manifest once one is found.

## Roadmap
- EAX for games on DirectSound3D (IDirectSound3DBuffer property sets),
  on the same EAX model and reverb buses as the Miles providers.
- SDL_GPU backend: matched image for image against the d3d9 backend;
  bump mapping (BUMPENVMAP) and PREMODULATE, which the uber-shader only
  approximates.
- macOS arm64: app bundle.
