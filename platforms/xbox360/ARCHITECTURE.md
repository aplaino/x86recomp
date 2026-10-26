# Architecture: the Xbox 360 platform module

This module turns an Xbox 360 title into a native 64-bit program: the
title's PowerPC machine code is lifted to C++ ahead of time and linked
with a runtime that reimplements the kernel (xboxkrnl), the system
library (xam) and the GPU (Xenos) it calls. Nothing here is specific to
one game: game projects hold that (manifest, symbols, game hooks) and use
this tree as a library.

It is a console platform on x86recomp's shared core (the top-level
ARCHITECTURE.md, "Console platforms"), built only with
`RECOMP_PLATFORM_XBOX360`:
- Core, shared by every PowerPC guest: the lifter `recomp_lift_ppc`
  (`tools/lifter_ppc`: decoder, function discovery, jump tables, the
  emitter) and the CPU model and dispatcher it targets (`src/ppc`, library
  `recomp_ppc`). The lifter knows no executable format; the platform
  registers an image reader for its own.
- Platform, here: XEX2 and the lifter's reader for it, LZX, the tools
  around the format, the runtime (the guest address layout, process
  bring-up, the kernel and xam HLE, the Xenos's command processor;
  drawing to come; libraries `recomp_xbox360_*`), with their tests and
  docs.
Paths below are relative to `platforms/xbox360` unless they start with
`src/ppc` or `tools/lifter_ppc`. An Xbox 360 game project turns
`RECOMP_X86` off and keeps the host layer every guest shares.

The module keeps the x86 path's and the Switch's shape: one C++ function
per guest function, registers cached in locals, a dispatcher for indirect
calls, an HLE registry that is fatal on anything unhandled, exact
arithmetic as the reference for hand-written replacements, and a game
project that owns everything game-specific.

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
4. Users bring their own game: no game code, assets, keys or firmware in
   any repository. The lifter and the loader read unencrypted XEX files
   only; `xbox360_xex_decrypt` makes one from an encrypted XEX with a key
   file the owner supplies.

## CPU: Xenon in 32-bit mode
Titles run on a 64-bit PowerPC in 32-bit mode (MSR[SF] = 0), as their code
shows: addresses above 2 GiB are built with `lis` (so the registers hold
their sign-extended forms) and used directly. In 32-bit mode
(ppc/lifted.h):
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

### Floating point
The FPU (src/ppc/fp.cpp) is exact: Berkeley SoftFloat under FPSCR[RN] with
tininess detected before rounding, and the PowerPC's own rules on top:
- NaNs: the first NaN of FRA, FRB, FRC (in that order, whatever the
  operation's operand order), quieted; the default NaN 0x7FF8000000000000
  for invalid operations; FNMADD and FNMSUB do not negate a NaN.
- Single-precision arithmetic rounds the exact result of its double
  operands once, to single precision, as the ISA defines it for any
  operands: the operation runs on float128 with round-to-odd, then one
  rounding to f32 under RN. A NaN result in single precision keeps only
  single's fraction.
- FPSCR: the exception bits with each invalid operation's cause (VXSNAN
  VXISI VXIDI VXZDZ VXIMZ VXVC VXSQRT VXCVI), FX when an exception bit
  goes from 0 to 1, the VX and FEX summaries, FI, FR (the rounding
  increased the magnitude: the result against the same operation rounded
  toward zero) and FPRF. MTFSF and MTFSFI do not write the reserved bit 20.
- LFS and STFS convert by the ISA's bit-level algorithms (no rounding,
  SNaNs kept).
- An exception whose enable bit is set, and FPSCR[NI], are fatal errors
  until their semantics are modelled, whether or not the thread enabled
  traps (KeEnableFpuExceptions, MSR[FE0/FE1] on the console).
FRES and FRSQRTE are estimates whose exact values are the CPU's own: the
model returns the correctly rounded reciprocal (square root), as QEMU's
970 does; the Xenon's tables wait for a console. FCTIW(Z)'s high word is
undefined (the word's sign extension here), so is MFFS's (0).

### VMX
The context holds 128 vector registers (VMX's 32 are VMX128's first 32)
in the register's own big-endian element order, so LVX and STVX are
plain 16-byte copies, and VSCR (only NJ and SAT exist). The integer
operations are inline (src/ppc/vmx.h, one switch the constant operation
folds), saturations set SAT; the floating-point ones (src/ppc/vmx.cpp) run
lane by lane on SoftFloat: round to nearest even, no exceptions, the
first NaN of the operands quieted, the default NaN 0x7FC00000, and with
NJ set denormal operands and results flushed to zero of the same sign.
VMADDFP and VNMSUBFP are fused. VREFP and VRSQRTEFP return the correctly
rounded value, as QEMU's 970 does; VEXPTEFP and VLOGEFP compute 2^x and
log2 x accurately in double and round once (estimates both: the Xenon's
own values wait for a console, and the test does not compare the last
two). A NaN converted by VCTUXS/VCTSXS gives 0 without SAT, as on the
oracle. LVEBX/LVEHX/LVEWX leave the other elements as they were (the ISA
leaves them undefined). The Xenon's (and Cell's) LVLX, LVRX, STVLX, STVRX
and their LRU forms load and store the parts of a quadword left and right
of the address; the 970 has none of them, so they wait for a console too.

### VMX128
The Xenon's extension of VMX to 128 vector registers, in primary opcodes 4
(loads, stores and VSLDOI128), 5 and 6. The decoder reads its encodings as
Xenia's opcode table has them, with the register numbers split across the
word (vD in bits 6-10 and 28-29, vA in 11-15, 26 and 21, vB in 16-20 and
30-31), and lifts most of it as the VMX operations they are: the loads
and stores (the LVLX family among them), the logical, shift, merge, pack
and unpack operations (VUPKHSB128 and VUPKLSB128 with vA 0x60 are the
halfword unpacks), the compares, conversions, roundings and estimates, and
the multiply-adds, which take an operand from vD (VMADDFP128: vA * vB + vD;
VMADDCFP128: vA * vD + vB; VNMSUBFP128: -(vA * vB - vD); VSEL128's mask
is vD; VPERM128's vC is one of v0-v7). Its own (src/ppc/vmx.h, vmx.cpp):
- VMULFP128, rounded once; VRLIMI128 (rotate by words, insert the words a
  mask selects into vD) and VPERMWI128 (words chosen by an immediate);
- VMSUM3FP128 and VMSUM4FP128, the dot products: as Xenia's x64 back end
  computes them, which it measured against a console (right but for the
  last bit at times): exact products in double, summed as (x + z) + y
  (+ (y + w)), rounded once to single, the default NaN when that
  overflows; denormals flushed under NJ;
- VPKD3D128 and VUPKD3D128, the D3D formats (D3DCOLOR, NORMSHORT2/4,
  10:10:10:2, 20:20:20:4, FLOAT16_2/4): the integer formats as biased
  floats (3.0 + v * 2^-22), clamped as floats when packed (a NaN to the
  bottom), the most negative field unpacking to the default NaN; the
  Xenos's halves (no infinities or NaNs, no denormals); then merged into
  vD as a word or a doubleword at the place the immediate says. As
  Xenia's x64 back end has them, but for one choice: FLOAT16_2 packs
  round to nearest even like FLOAT16_4 (Xenia truncates the first; its
  tests have a tie for the second only).
What Xenia has not measured waits for a console: the NaN a dot product
returns (here the first of the products' operands, lane by lane, A's
before B's), the multiply-adds' NaN order, the halves' rounding, a
packed 10:10:10:2 in all four words of the intermediate, and the
encodings the decoder rejects (a VPKD3D128 size of 0, type 7, VRLIMI128's
mask bit 4, an unused immediate).

## Lifter
`recomp_lift_ppc --image <title.xex> --out <dir>` (tools/lifter_ppc):
- Function starts: the entry point, `.pdata` (each entry's start and
  length), code exports, import thunks, and every `bl` and out-of-range
  branch target. A function is the code reachable from its start within
  its range (`.pdata`'s length, or up to the next start), so a call into
  the middle of a run of code (the register save and restore helpers)
  gets its own overlapping function, and falling through into the next
  start is a tail call.
- Jump tables, ending in `mtctr` and `bcctr` with the index bounded by a
  `cmplwi` and a `bgt` or `bge`: absolute (`lwzx` from a table of
  addresses) and relative (`lbzx`, `lhzx` or `lwzx` of an offset, scaled,
  added to a base); constants from `lis`/`li` with `addi`/`ori`, and the
  `ori r, r, 0` padding skipped. Other `bcctr` are indirect jumps
  (dispatched).
- Calls: `bl` and `bcl` call the target's function, `blr` returns,
  `bctrl` and `blrl` dispatch; registers are spilled to the context
  around anything that can observe them.
- Import thunks become `Import(c, module, ordinal, va)`, defined by the
  platform. `sc` calls `Sc()`, traps that fire `Trap()`, `mftb` (and
  `mfspr` of TBL or TBU, its later spelling) `TimeBase()`.

## XEX2
`src/xbox360/xex.{h,cpp}` reads the headers, the file format info, the
image (stored, "basic" runs of data and zeros, or "normal": a chain of
blocks of LZX chunks), the PE headers inside the image (sections,
`.pdata`), the import libraries (a record's word in the image says thunk
or variable and the ordinal) and the exports by ordinal of system modules
(the table the security info points at; the PE `.edata` of system modules
is stale). LZX (`src/xbox360/lzx.{h,cpp}`) is written from the format's
specification; it decodes every system module and title checked so far
byte for byte as libmspack does. Encrypted files are refused with a
pointer to `xbox360_xex_decrypt` (tools/xex_decrypt.cpp, OpenSSL's AES
linked dynamically): the security info's AES key decrypted with the
owner's key (ECB) is the session key, the data is CBC under it with a
zero IV.

## Runtime
The runtime (`src/xbox360`, library `recomp_xbox360`) holds what lifted
code calls (`Import`, `Sc`, `Trap`, `TimeBase`) and the kernel's state:
guest memory, the HLE registry, threads, dispatcher objects, handles,
the title's module, the console's settings, the GPU and time. The system modules'
exports are HLE modules on top of it, one shared library per system
module (`recomp_xbox360_xboxkrnl` in `src/xbox360/xboxkrnl`,
`recomp_xbox360_xam` in `src/xbox360/xam`); where one module's export is
another's (xam's XGetVideoMode is the kernel's VdQueryVideoMode), both
handlers call the same runtime code. Behaviour comes from the system's own code where
it shows it (xam, which the console's update ships unencrypted, and what
titles read), otherwise from Xenia and ReXGlue, and is written down in
docs/xboxkrnl.md with what still waits for a console.

### Programs and libraries
A program is a directory of files, each of which can be replaced alone:
- A title's lifted code is a shared library of its own, a lifted module
  (`recomp_add_lifted_ppc(<target> <dir> LINK recomp_xbox360)`). Its
  symbols are hidden: calls between lifted functions are direct, and the
  dynamic symbol table does not carry a title's 10^5 functions. It hands
  its function table to the dispatcher when it loads; the dispatcher
  holds the tables of every loaded module, and the loader refuses a XEX
  whose code hash matches none of them.
- The runtime, the HLE modules and recomp_ppc are shared libraries as on
  the x86 path; nothing refers to an HLE module's or a lifted module's
  symbols, so programs link them --no-as-needed.
- The game project's executable holds `main()` and little else: it fills
  a `GameConfig` (xbox360/process.h) and calls `RunProcess`.
- `recomp_bundle()` (top-level CMakeLists.txt) puts executables and
  libraries in one directory (`RECOMP_BUNDLE_DIR`, `bin/` of the build)
  with an rpath relative to themselves (`$ORIGIN`, `@loader_path`). A
  title update is a new lifted module, a kernel fix a new
  `librecomp_xbox360_xboxkrnl`, and neither relinks the rest.

### Memory
One 4 GiB arena, guest address a at `mem + a`, laid out as the kernel
does (the ranges are Xenia's, which match the addresses titles use):

| Range | Pages | What |
|---|---|---|
| 0x00000000-0x3FFFFFFF | 4 KiB | NtAllocateVirtualMemory; the first 64 KiB never mapped |
| 0x40000000-0x7EFFFFFF | 64 KiB | the same with MEM_LARGE_PAGES; thread stacks from the top |
| 0x7F000000-0x7FFFFFFF | | the GPU's registers (milestone 5) |
| 0x80000000-0x8FFFFFFF | 64 KiB | XEX images |
| 0x90000000-0x9FFFFFFF | 4 KiB | XEX images; the kernel's own objects from the top |
| 0xA0000000-0xBFFFFFFF | 64 KiB | physical memory |
| 0xC0000000-0xDFFFFFFF | 16 MiB | physical memory |
| 0xE0000000-0xFFFFFFFF | 4 KiB | physical memory from 0x1000 |

The 512 MiB of physical memory is one shared memory object mapped three
times, so a write through one view is read through the others, and an
allocation in any view claims its physical pages, so no two allocations
share any. MmGetPhysicalAddress answers for the physical views only:
virtual memory has no physical pages behind it here, and a title that
asks for one is stopped (fatal) rather than handed a wrong one.
Allocation state and protection are kept per 4 KiB page; the host's
protection is never changed for them (only the first 64 KiB are
inaccessible, so a null pointer faults), and decommitted pages are zeroed
so that a commit always hands out zeros.

The 0xE0000000 view starts one 4 KiB page into physical memory (Xenia's
measurement; titles get physical addresses from MmGetPhysicalAddress and
hand them to the GPU). mmap can alias that only with 4 KiB host pages:
on hosts with larger ones (macOS on Apple Silicon) the runtime refuses to
start for now. The way there is the one Xenia's x64 back end takes on
Windows, whose mappings are 64 KiB grained: lifted code adds 0x1000 to
addresses at and above 0xE0000000, which needs the address computation
of lifted.h to know about it.

### HLE
Handlers are registered by module and ordinal with
`HLE("xboxkrnl.exe", 300, RtlInitAnsiString) { ... }`. A handler reads
its arguments with `Arg(c, i)`: r3-r10, then the caller's parameter area,
an 8-byte slot per argument from r1 + 0x50 (the low word of a 32-bit one
at r1 + 0x54 + 8 * (i - 8)); its result goes to r3. An import without a
handler is fatal when it is called, naming the export. The loader writes
every import slot: a function's gets its thunk's address, so a call
through the slot reaches the same handler, a variable's the address of
guest memory the HLE module defines for it (`HLE_VARIABLE`), and a module
importing a variable nobody defines is refused at load, naming each.
Import() finds the handler by the thunk's address in a per-thread table,
without a lock.

### Threads and objects
- A guest thread runs on its own host thread (16 MiB of host stack, for
  the lifted calls). Its guest stack comes from the top of the 64 KiB
  virtual range; its KTHREAD, KPCR and TLS from the kernel's objects.
  r13 points at its own KPCR (the console has one per hardware thread);
  the fields guest code reads are set (docs/xboxkrnl.md).
- Dispatcher objects (events, semaphores, mutants, threads) live in guest
  memory and their headers are their state: guest code initialises
  events itself. Every signal and every check of a wait is made under one
  dispatcher lock, and any change wakes every waiter to look again.
  Timers, queues and APCs are not there yet; alertable waits are not
  woken.
- Handles name objects by guest address and hold references; objects the
  kernel created (NtCreateEvent, threads) go with their last reference.
- Critical sections follow the kernel's algorithm on the guest structure;
  spin locks hold their owner's KPCR; IRQL is the KPCR's byte, and no
  interrupt or DPC ever runs.
- The title ends when its entry point returns (with what it returned),
  when its last thread ends, or at HalReturnToFirmware. A thread that is
  running cannot be suspended (fatal; a host thread cannot be stopped
  where it is), one created suspended can.
- The kernel's own threads (the GPU's command processor and vertical
  blank) are host loops with guest structures, which call guest code (the
  graphics interrupt callback) as DPCs: at DISPATCH_LEVEL, the KPCR's DPC
  flag set. They do not keep the process alive.
- The time base runs at 49.875 MHz from the host's steady clock, system
  time is the host's, KeTimeStampBundle is kept current every
  millisecond.

### The title's module
The loader keeps the title's XEX headers in kernel memory and describes
the module with a loader entry (LDR_DATA_TABLE_ENTRY), which
XexExecutableModuleHandle points at and which is the module's handle:
titles find their headers through it and read them with
RtlImageXexHeaderField, their resources with XexGetModuleSection, their
privileges with XexCheckExecutablePrivilege. The title is the only
module with an entry: asking for a system module's handle (xam asks for
its own) is fatal until those are modelled, and XexLoadImage (a title's
own DLLs) is not there yet.

### Settings
The console's settings (`src/xbox360/config.{h,cpp}`) are the XConfig
entries titles and xam read with ExGetXConfigSetting, and the display
they describe: the AV pack (HDMI) and the video mode (1280x720,
progressive, at the AV region's rate). Only the settings listed there
exist; reading another is fatal and names it, so each one is added with
its evidence (docs/xboxkrnl.md, "Settings and the display"). The values
are a console set up in English for an HD display, the time zone the
host's.

### The GPU
The GPU (`src/xbox360/gpu.{h,cpp}`) is what the kernel's display driver
and titles see of the Xenos, without drawing yet (docs/xenos.md):
- Its registers are guest memory at 0x7FC80000, where the console maps
  them, so guest code reads and writes them with plain loads and stores;
  nothing traps an access. The values titles read are set at power-on.
- The command processor is a kernel thread that polls CP_RB_WPTR and
  executes the ring buffer's PM4 packets up to it: register writes,
  memory writes, waits, fences, indirect buffers, interrupts, the swap
  packet VdSwap writes. Draws and shader loads are consumed; an opcode it
  does not know is fatal. It writes the read pointer back as the driver
  asked.
- The vertical blank is another kernel thread, at the video mode's rate.
  Both deliver the graphics interrupt callback as a DPC.
Drawing (milestone 5) adds a renderer behind the same command processor,
through gfx::Device.

### Audio
The audio render driver (`src/xbox360/audio.{h,cpp}`, docs/xboxkrnl.md
"Audio") is a kernel thread that calls each registered client's
callback once per frame of 256 samples of 5.1 at 48 kHz, paced by the
host clock and held back by the host's queue. Submitted frames go to
SDL3 (the host layer), one stream per client on the default playback
device, which mixes them and downmixes to the device; the floats are
handed over unchanged. With no device the frames are dropped and the
callbacks keep their pace. XMA decoding (the XMA* exports) is not there
yet.

### Files
The file namespace (`src/xbox360/fs.{h,cpp}`) is the kernel's: devices,
each a host directory, and symbolic links to them.
- `\Device\CdRom0` is the title's disc, read-only: the directory of its
  files (`GameConfig::game_dir`, by default the XEX's). GAME: (which xam
  makes for a title it launches) and D: name it.
- `\Device\Harddisk0\Partition1` is the hard disk's data partition,
  writable: `Partition1` in the per-user data directory
  (`GameConfig::data_dir`, by default $XDG_DATA_HOME/<id>).
- ObCreateSymbolicLink adds links (`\??\name:`), which may name another.
  A path is resolved link by link, then on its device component by
  component, case-insensitively as the console's file systems compare
  names; "." and ".." never leave the device.
- A file handle names a kernel object (a notification event, signalled
  as each I/O on the file completes) with the host file behind it. I/O
  is carried out at once: an asynchronous request completes before it
  returns, its event set and its status block written. APC completion
  routines are not supported yet (fatal).
- NtQueryDirectoryFile returns one entry per call, the mask applying
  from the first query, in name order. FileFsAttributeInformation names
  the disc's file system GDFX and the hard disk's FATX; their allocation
  units are 2 KiB and 16 KiB.
- Information classes without a handler are fatal and name the class.

## Tests and the oracle
The oracle is QEMU's PowerPC 970 bare metal (`qemu-system-ppc64 -M none
-cpu 970fx`), switched to 32-bit mode by the test programs' start-up
(tests/oracle/crt0.S): a 64-bit CPU running in 32-bit mode as a Xenon runs
a title. `tests/oracle/qemu_run.py` loads a program and the interrupt
vectors, waits for the closing `sc` and dumps what the program left in
memory; any other interrupt fails the run.
- The fixture (tests/fixture): hand-written forms of every decoded
  instruction (ops.S), the compiler's two jump table shapes (tables.S) and
  integer C, built with clang and LLD for big-endian PowerPC64 and
  turned into a XEX by `xbox360_elf2xex`. Run bare metal on the oracle
  and lifted (`xbox360_lift_fixture`), it must print the same.
- The form test: `recomp_lift_ppc --forms` writes each distinct
  instruction form as a lifted function and as the original instruction
  in a stub; the oracle runs every stub from random states (edge values,
  base registers into a buffer with random high words) and the host test
  replays them through the lifted forms, which must end bit for bit in
  the same registers, CR, XER, LR, CTR and memory. Builds with planted
  bugs in ppc/lifted.h (RECOMP_PPC_MUTATION) must fail it.
- Where QEMU departs from the ISA the test does not compare (and the
  model follows the ISA): FR (QEMU's is not the ISA's, as spot checks with exact
  arithmetic show), FX (QEMU sets it on every exception, not on a
  0 to 1 change), FI and FPRF's C bit on compares (the ISA alters only
  FPCC), FRES's FPRF and its estimate of 1/±0 (QEMU: ±0.5), and VSCR's
  reserved bits (QEMU keeps what MTVSCR wrote).
- VMX128 runs on the oracle as VMX: the form writer re-encodes each
  VMX128 form as the VMX instruction with the same operation and
  operands, or for VMULFP128, VRLIMI128 and VPERMWI128 as a short VMX
  sequence through scratch registers the stub reloads; the fixture keeps
  those forms' registers below 30. Since the twin comes from the
  decoder's reading, xbox360_vmx128_test checks the decoding itself
  (high registers, operand roles) against each operation's definition,
  and the dot products and D3D formats, which have no twin, against
  Xenia's test vectors; it has its own planted bugs.
- The runtime test title (tests/title, `xbox360_runtime`): C and assembly
  built like the fixture, which calls the kernel through imports
  (`xbox360_elf2xex --import` writes its import library, `--header` and
  `__xex_resource_` symbols its optional headers), is lifted into a
  lifted module and run on the runtime. It checks what the kernel gives
  it (memory and the physical views, threads, TLS, r13, waits and
  objects, critical sections, formatting, variables, time, files on its
  disc and on the hard disk, its module and XEX headers, the settings and
  the video mode, the GPU's registers, interrupts and a ring buffer's
  packets, string conversions, an audio client) and exits with the number
  of failed checks. It runs with SDL's dummy audio driver.
  The test writes the disc's files and empties the data directory before
  each run.
- Not covered by QEMU: what the 970 does differently from the Xenon
  (DCBZ's size, the undefined cases above, the estimates, the LVLX
  family) and VMX128 beyond its VMX twins. Those wait for a console.

## Roadmap
1. Integer core, XEX reader, oracle (done).
2. Floating point (exact, FPSCR) and VMX, on the same oracle (done).
3. VMX128, with Xenia's implementation as the reference until a console
   can run test programs (done).
4. Runtime: the arena with the physical memory views, process and
   thread bring-up (r13 and the thread's kernel structures), the kernel
   and xam HLE, written against the system's own behaviour. In progress:
   the libraries and their layout, memory, threads, synchronisation,
   files, the title's module, the settings, the display driver with a
   command processor that executes the ring buffer without drawing, and
   the audio render driver, and the first kernel and xam exports are
   done; next xam (users, content, notifications, messages), XMA
   decoding, timers and APCs.
5. Xenos: drawing, shaders and presentation through gfx::Device, behind
   the command processor (which runs the ring buffer already).
