# Architecture: the Xbox 360 platform module

This module turns an Xbox 360 title into a native 64-bit program: the
title's PowerPC machine code is lifted to C++ ahead of time and linked
with a runtime that reimplements the kernel (xboxkrnl), the system
library (xam) and the GPU (Xenos) it calls. Nothing here is specific to
one game: game projects hold that (manifest, symbols, game hooks) and use
this tree as a library.

It is a console platform on SwARc's PowerPC core (the top-level
ARCHITECTURE.md, "Layout: base, CPU cores, platform modules"), built
only with `RECOMP_PLATFORM_XBOX360`:
- Core, shared by every PowerPC guest: the lifter `recomp_lift_ppc`
  (`tools/lifter_ppc`: decoder, function discovery, jump tables, the
  emitter) and the CPU model and dispatcher it targets (`src/ppc`, library
  `recomp_ppc`). The lifter reads ELF itself; the platform registers an
  image reader for its own. Its design: src/ppc/ARCHITECTURE.md.
- Platform, here: XEX2 and the lifter's reader for it, LZX, the tools
  around the format, the runtime (the guest address layout, process
  bring-up, the kernel and xam HLE, the Xenos's command processor;
  drawing to come; libraries `recomp_xbox360_*`), with their tests and
  docs.
Paths below are relative to `platforms/xbox360` unless they start with
`src/ppc` or `tools/lifter_ppc`. An Xbox 360 game project turns the
x86 core and Win32 off (`RECOMP_X86`, `RECOMP_PLATFORM_WIN32`) and keeps
the host layer every guest shares.

The module keeps the x86 core's and the Switch's shape: one C++ function
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

## CPU and lifter
The PowerPC core's: its design (src/ppc/ARCHITECTURE.md) covers the CPU
in 32-bit mode as titles run it, floating point, VMX and the Xenon's
VMX128, the lifter and the oracle tests. A XEX's code is the Xenon's
(`Cpu::kXenon`): the reader (lifter/xex_reader.cpp) gives
recomp_lift_ppc that CPU and its function starts, from the entry point,
`.pdata` (each entry's start and length), the code exports and the
import thunks, which become calls to `Import(c, module, ordinal, va)`.

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
handlers call the same runtime code. Guest code that takes a system
export's address (XexGetProcedureAddress) gets one the dispatcher runs
the handler for: each `HLE()` handler also defines a guest function, and
before the first guest thread the runtime gives each an address in its
module's area and registers it with the dispatcher (`RegisterGuestFunctions`).
Behaviour comes from the system's own code where
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
  the x86 core; nothing refers to an HLE module's or a lifted module's
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
| 0x7F000000-0x7FFFFFFF | | the GPU's registers (milestone 5), the XMA decoder's |
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
without a lock. xam's own messages to its apps (XMsg*) have a registry
of their own in the xam module, by app and message
(`XMSG(app, message, Name)`), with the same rule: a message without a
handler is fatal when it is sent. Requests complete before the call
returns.

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
  Timers are signalled by a host thread at their due time and period;
  queues and APCs are not there yet, and alertable waits are not woken.
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
callbacks keep their pace.

### The XMA decoder
The XMA decoder (`src/xbox360/xma.{h,cpp}`, docs/xboxkrnl.md "XMA") is
what the kernel's XMA* exports and titles' audio libraries see of the
hardware, without decoding yet:
- 320 contexts of 64 bytes in uncached physical memory, which
  XMACreateContext hands out and XMAReleaseContext zeroes and frees.
- Its registers are guest memory at 0x7FEA0000, little-endian, as for
  the GPU: the context array's physical address is set at power-on.
- A kernel thread polls the command registers, a bit per context, and
  takes each word written (leaving 0). A clear resets the context's
  buffers and offsets; a lock, with nothing decoding, changes nothing; a
  kick needs the decoder and is fatal.
  Polling leaves a gap between a title's write and the command, which a
  clear only narrows by running fast; the decoder settles how commands
  reach it (docs/xboxkrnl.md, "XMA").

### Files
The file namespace (`src/xbox360/fs.{h,cpp}`) is the kernel's: devices,
each a host directory, and symbolic links to them.
- `\Device\CdRom0` is the title's disc, read-only: the directory of its
  files (`GameConfig::game_dir`, by default the XEX's). GAME: (which xam
  makes for a title it launches) and D: name it.
- `\Device\Harddisk0\Partition1` is the hard disk's data partition,
  writable: `Partition1` in the per-user data directory
  (`GameConfig::data_dir`, by default $XDG_DATA_HOME/<id>).
- A content package xam opens is a device of its own, mounted while the
  title runs: a host directory under Partition1's `Content` (docs/xam.md,
  "Content"). Devices are never unmounted, as open files refer to them.
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

## Tests
The PowerPC core's tests (tests/ppc; src/ppc/ARCHITECTURE.md, "Tests
and the oracle") check the CPU and the lifter; this module's preset
builds and runs them too.
- The runtime test title (tests/title, `xbox360_runtime`): C and assembly
  built like the core's fixture, which calls the kernel through imports
  (`xbox360_elf2xex --import` writes its import library, `--header` and
  `__xex_resource_` symbols its optional headers), is lifted into a lifted
  module and run on the runtime. It checks what the kernel gives it
  (memory and the physical views, threads, TLS, r13, waits and objects,
  critical sections, formatting, variables, time, files on its disc and on
  the hard disk, its module and XEX headers, the settings and the video
  mode, the GPU's registers, interrupts and a ring buffer's packets,
  string conversions, an audio client, xam's startup exports and messages,
  users, profile settings, notifications, content packages, timers) and
  exits with the number of failed checks. It runs with SDL's dummy audio
  driver. The test writes the disc's files and empties the data directory
  before each run; it then checks the saves left on the data directory
  (title settings, content packages).

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
   the audio render driver, timers, the kernel exports and xam's startup
   ones (the loader, networking, memory, messages, the music player's,
   users, profile settings, notifications, content) are done; next the
   XGI and XLiveBase messages, XMA decoding (the contexts and their
   registers are there) and APCs.
5. Xenos: drawing, shaders and presentation through gfx::Device, behind
   the command processor (which runs the ring buffer already).
