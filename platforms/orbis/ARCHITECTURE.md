# The PS4 platform module

The PS4 (Orbis OS) is an x86-64 guest: its CPU is AMD family 16h, the
one the x86 core models, so the module (platforms/orbis, option
RECOMP_PLATFORM_ORBIS) is a platform on the x86 core like Win32, and
builds and runs without it (the `orbis` preset turns Win32 off). The
repository's ARCHITECTURE.md ("Layout: base, CPU cores, platform
modules") has the layers and the core's interfaces this builds on; the
system libraries' HLE choices and the values not measured on hardware
are in docs/hle.md.

## Images
The lifter reads the PS4's decrypted ELFs through an ImageReader
(lifter/orbis_reader.cpp, on tools/lifter_x86/program.h): SCE dynamic
tables and NID imports, function starts from .eh_frame, and .text
bounded by them and the PLT, since the executable segment also holds
.rodata and .eh_frame. Ghidra symbols are optional for it.

## Address space
Executables are mapped at 0x400000. The PRXs a game starts with
(orbis/app.h) go from 0x800000000 up in initialisation order, each at the
next 256 MiB boundary past the one before; tools/lift.sh lifts them at
the same bases (recomp_lift's --base), since both take them from there. The guest allocator hands out the range above 4 GiB, below the
top 64 MiB of the arena, where the core's thunk region is.

## Process bring-up
- A game's executable is main() calling orbis::Main (orbis/process.h)
  with its GameConfig: it takes the app0 directory (--game-dir), finds
  the modules as the system's loader does (orbis/app.h: the eboot, then
  each PRX in app0/sce_module that a module needs through
  DT_SCE_NEEDED_MODULE, found by its module name, after the modules it
  needs itself; needed modules the game does not ship are system
  libraries, the HLE) and reads param.sfo (app0/sce_sys/, or
  --param-sfo; its TITLE_ID is the game's own title for save data).
- The loader (src/orbis/process.h) maps the eboot and those PRXs at
  their bases, gives each module with PT_TLS a slot
  in load order (FreeBSD's variant II: the eboot's block right below the
  thread pointer), then binds every import: to another module's export
  of that library and NID, else to the HLE handler registered under the
  name with that NID (orbis/hle.h, ORBIS_HLE), else to an unimplemented
  thunk (a data import nothing defines is fatal at load). Each thread's
  TCB (Process::thread_init, found through fs) holds its own address and
  the DTV, which __tls_get_addr reads. Before the eboot's
  _start(argument block, cleanup), each PRX's _init(0, 0, 0) runs its
  constructors and its module_start.
- The process is a System V one (Process::sysv: HLE arguments and
  CallGuest), new threads start with FreeBSD's x87 control word
  (Process::fpu_control 0x037F), and the guest has no UI thread of its
  own: its main thread runs beside the host loop (RunMainThreadBeside).
- orbis_process_test runs C built for FreeBSD x86-64 (clang, ld.lld),
  made SCE ELFs by tools/orbis_sce_elf.py and lifted at build time,
  through the loader and libkernel's HLE.

## Host thread, GPU and system libraries
- The host thread (orbis/host.h): the process main thread brings up SDL
  video, owns the window and its events and runs what the HLE posts to
  it (presentation); guest threads never touch SDL.
- The GPU (orbis/gpu/cp.h, the library recomp_orbis_gpu): the command
  processor runs the PM4 buffers libSceGnmDriver submits on host
  threads, one per queue (graphics, each mapped compute ring), beside
  the CPU: the packets that write memory the CPU sees, the waits and the
  end-of-pipe interrupts (as event queue events) are exact, register
  writes are kept. Draws and dispatches go to gfx::Device's programs
  (orbis/gpu/renderer.h): a stage's scalar instructions run on the CPU
  with its user data and guest memory to find the descriptors (buffers,
  textures, samplers, the fetch shader it calls) its memory instructions
  use, then the stage translates to GLSL (orbis/gpu/shader.h, decoding
  in orbis/gpu/gcn.h) once per code and formats: one lane per
  invocation, branches as a switch over blocks, memory by guest address
  on the ranges each draw binds. Ranges that are read over and over
  (large ones, index ranges) are kept in device GPU buffers, uploaded
  again only after a write (write watches, the repository's
  ARCHITECTURE.md). The device's GPU buffers are places in one heap
  buffer bound beside each flush's arena, so draws read them and take
  indices from them in place: SDL_GPU wraps every buffer copy in
  pipeline barriers, and copying them into the arena each flush kept the
  GPU mostly stalled. Targets are device images keyed by guest address,
  so render-to-texture and the display need no copies through guest
  memory; VideoOut shows the target at a flipped buffer's address.
  libSceGnmDriver's HLE writes the same packets into the game's command
  buffers as the 11.00 driver does.
- System libraries' HLE (src/orbis/hle, one shared library per library
  or family: recomp_orbis_kernel, ..._system, ...) registers by name. A
  dev aid names imports without a handler: <ID>_NID_NAMES is a local
  file of symbol names (one per line) that the loader hashes, so an
  unimplemented import's error shows its name.
- Host services come only through the host layer: the GPU front end
  through gfx::Device, audio through the software mixer, windows and
  input through SDL3, movies through ffmpeg, fonts through FreeType;
  never a host graphics or audio API of its own and no host library
  outside the layer.

## Programs and libraries
A game's lifted code is a lifted module, a shared library of its own
with hidden symbols (`recomp_add_lifted(<target> <dir> LINK
recomp_orbis)`), lifted by tools/lift.sh (the eboot and its start PRXs,
each with the game's manifest named after its module, if any, and the
platform's fixes for its middleware build: lifter/middleware/<SHA-256 of
the file>.manifest, whose `host` lines name functions in src/orbis such
as the libSceFios2 scheduler kicks in fios2.cpp); the
executable holds `main()` and little else and links recomp_orbis_hle
(every system library's HLE: imports bind by name), and
recomp_bundle() puts both beside the loader, each HLE library, the GPU,
the x86 core and the host layer under them in one directory. The
libraries link with CMAKE_LINK_DEPENDS_NO_SHARED, as the game projects
do, so a GPU change rebuilds and relinks recomp_orbis_gpu alone, an
audio fix recomp_orbis_audio or recomp_orbis_ngs2 alone, and a re-lift
the lifted module alone.

The lifter's reader (lifter/orbis_reader.cpp) marks what a game would
otherwise list in its manifest: imports that never return, setjmp (the
PLT stub of an imported one and the export of a shipped libc, whose
callers longjmp can resume) and longjmp (its rets resume a setjmp
caller), all by NID.

orbis_app (tools/orbis_app.cpp) shows what the platform makes of an
app0 without running it: the start modules and their bases (`modules`,
what tools/lift.sh lifts), and every import of them with where it binds
(`imports`: to a module, to an HLE handler, or to nothing yet), by
library; it names the shipped PRXs that start only when the game loads
them itself.
