# SwARc

SwARc, the Swiss Army Recompiler: static recompilation of games into
native 64-bit programs, the scaffold that gets a game running natively
while its code is replaced, piece by piece, with hand-written C++. A
game's machine code is translated ahead of time into C++ and linked
against a runtime that reimplements the parts of its operating system
and middleware the game uses: Windows for x86 and x86-64 PC games here,
consoles as platform modules on the core of their CPU. There is no
emulator, interpreter or Wine in the result, and no game code or data in
this repository.

The game ports built on it and their status are on the
[project site](https://jtm-rootstorm.github.io/SwARc.site/).

## What is here

Three layers ([ARCHITECTURE.md](ARCHITECTURE.md), "Layout"): the base
every guest shares (`src/runtime`), a core per guest CPU (x86:
`src/x86`, `tools/lifter_x86`; PowerPC: `src/ppc`, `tools/lifter_ppc`;
AArch64: `src/a64`, `tools/lifter_a64`), and platform modules on a core
(`platforms/<name>`: Win32 for PC games, and the consoles). All of them
are on `main`, each core and module behind its own CMake option; a game
builds its own core and platform and nothing else.

### The x86 core and Win32

- **Lifter** (`tools/lifter_x86`, `recomp_lift`): decodes an image with
  Zydis (PE through the Win32 module, the PS4's ELF through its own) and
  emits C++ for every function, one basic block at a time, with exact
  x86 integer and flag semantics. Straight-line x87 code becomes guarded
  "runs" that keep the FPU register stack in locals. 32-bit and x64
  images both lift; x64 code runs on the Win64 or System V ABI against a
  fixed CPU model (AMD family 16h: SSE3 to SSE4.2, POPCNT, LZCNT,
  CMPXCHG16B, BMI1, MOVBE, AES and PCLMUL claimed, AVX lifting).
- **x87 model** (`src/x86/x87.cpp`): exact 80-bit x87 (precision and
  rounding control, status-word flags) on Berkeley SoftFloat with
  integer fast paths, tested bit for bit against a real x87.
- **SSE and AVX** (`src/x86/sse.cpp`): the XMM/YMM registers and MXCSR,
  with SoftFloat lanes and x86 NaN rules, tested bit for bit against the
  CPU like the x87 model.
- **Runtime**: the guest address space (4 GiB for 32-bit guests),
  write watches and coroutines in the base (`src/runtime`); dispatch,
  threads, calls into lifted code, the HLE registry, crash and hang
  reports in the x86 core (`src/x86`); PE loading, SEH and x64
  unwinding in the Win32 platform.
- **HLE** (`platforms/win32`): Win32 (kernel32, user32, gdi32, advapi32,
  shfolder, ...) on POSIX and SDL3; optional modules for Miles Sound
  System (with EAX 1-3 on our own reverb, modelled on Creative
  hardware), Bink, DirectDraw 7/Direct3D 7 and OpenGL 1.x (on a
  fixed-function device: SDL_GPU on Vulkan or native Metal, or
  dxvk-native's Direct3D 9 as a reference), Direct3D 9 and D3DX 9 (in
  progress), DirectSound 8, DirectInput 8, XInput and dbghelp; for x64
  guests the UCRT and MSVCP140 (in progress). Statically linked runtime
  libraries (the 32-bit MSVC CRT so far, `recomp_msvc_crt`) are written
  once and bound to each game's copy (`native` in a lifter manifest).
- **Shader translators** (`src/runtime/hlsl`, `recomp_hlslc`): D3DX 9's
  HLSL dialect and compiled Direct3D 9 shaders (shader models 1-3) to
  GLSL, for the programmable pipeline on SDL_GPU.
- **Tools**: Ghidra export scripts (`tools/ghidra`), a PE symbol dumper
  (`tools/pe_symbols.py`), dev-time reference checks
  (`platforms/win32/tools/layout`, `platforms/win32/tools/undname`), and
  EAX reference measurement (`tools/eax_capture`: a Windows 98 recorder
  for Creative hardware, the same cases rendered by the runtime, and
  their comparison).

### Consoles

A console whose CPU a core already models is a platform module on that
core rather than a fork; a console with another CPU brings that CPU's
core with it. The console modules are in progress and off by default:

- **PlayStation 4** ([platforms/orbis](platforms/orbis),
  `RECOMP_PLATFORM_ORBIS`), on the x86 core: SCE ELF and PRX images with
  imports by NID, a process loader for any game's `app0`, libkernel and
  the system libraries (HLE), the GPU's PM4 command buffers and GCN
  shaders on the same graphics device as the PC games, AJM audio and
  video decoding.
- **Xbox 360** ([platforms/xbox360](platforms/xbox360),
  `RECOMP_PLATFORM_XBOX360`), on the PowerPC core (64-bit PowerPC in
  32-bit mode, exact floating point, VMX and the Xenon's VMX128, checked
  bit for bit against QEMU's PowerPC 970): XEX2 images with their own
  LZX decoder, the kernel's and xam's exports, the GPU's command
  processor, resolves and the front buffer in a window (not drawing
  yet), XAudio and XMA.
- **Nintendo Switch** ([platforms/horizon](platforms/horizon),
  `RECOMP_PLATFORM_HORIZON`), on the AArch64 core (integer, flags and
  the exclusive monitor so far, checked against Unicorn and, on arm64
  hosts, the CPU): NRO/NSO images, the address layout, process bring-up
  and the first supervisor calls.

The Xbox 360 and the Switch build with the x86 core and Win32 switched
off.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the design (each core and
module has its own beside its code); each platform's `docs/` the
observed behaviour of what it reimplements
([platforms/win32/docs/miles.md](platforms/win32/docs/miles.md)).

## Using it from a game project

```cmake
set(SWARC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SwARc" CACHE PATH "")
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)  # no relink when only a library changes
enable_testing()
add_subdirectory("${SWARC_DIR}" swarc)

recomp_add_lifted(mygame_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")
add_executable(mygame src/main.cpp $<TARGET_OBJECTS:mygame_lifted>)
target_link_libraries(mygame PRIVATE recomp_win32 recomp_mss32)
```

`main()` fills a `recomp::win32::GameConfig` (id, guest install
directory, EXE name, registry seed, optional hooks) and calls
`recomp::RunProcess` (`win32/process.h`).
The lifter runs as a separate step over the user's own EXE:

```sh
recomp_lift --exe game.exe --symbols symbols --out generated \
  --manifest lifter.manifest
```

DLLs the game loads itself are lifted in the same run (`--module
game.dll --symbols dir [--manifest file]`, repeated) and listed in
`GameConfig::lifted_dlls`.

A console game turns on its platform's option instead (and usually the
x86 core and Win32 off); each console module's README or ARCHITECTURE.md
shows its use.

## Building and testing

CMake presets build each core and module alone or together, and run
their tests (`ctest --test-dir out/build/<preset>`): `debug` and
`release` (the x86 core with Win32), `x86` (the x86 core alone), `ppc`,
`a64` and `a64-aarch64-qemu` (the PowerPC and AArch64 cores alone; the
last cross-built for arm64 and run under qemu-aarch64), and `orbis`,
`xbox360`, `horizon` (each console module with its core). The cores'
oracle tests need clang and ld.lld for their fixtures, qemu-system-ppc64
(PowerPC) and a `.venv/` Python with unicorn (AArch64).

Changes land on `main` from short-lived topic branches through
`tools/land.py`, which builds and tests the presets a change touches;
[CLAUDE.md](CLAUDE.md) has the project's rules.

## Requirements

Linux x86-64, or macOS on Apple Silicon. Clang and LLVM (the static
`libLLVMDemangle.a` for dbghelp), CMake 3.25+, Ninja, SDL3 (3.4+: SDL_GPU
on Vulkan 1.1); ffmpeg for Miles/Bink and XMA; FreeType for fonts, and for
the PS4 platform fontconfig too (the system dialogs the runtime draws);
glslang (`glslangValidator`) for the
shaders, and on macOS SPIRV-Cross (`spirv-cross`) too. Graphics (D3D7
and OpenGL) use SDL_GPU: Vulkan on Linux, native Metal on macOS. On
Linux a dxvk-native build (Direct3D 9 only, SDL3 window system) is
optional: it adds the d3d9 backend (`<ID>_GFX=d3d9`), the reference
image for SDL_GPU.

On macOS with Homebrew
(`brew install cmake ninja sdl3 ffmpeg freetype fontconfig llvm@22 glslang spirv-cross`):

```sh
cmake --preset debug -DRECOMP_LLVM_ROOT=/opt/homebrew/opt/llvm@22
```

## Licence

MIT (see [LICENSE](LICENSE)). Dependencies keep their own licences:
Berkeley SoftFloat 3e (BSD-3-Clause), Zydis (MIT), CORE-MATH (MIT) and,
for the PS4 module, LibAtrac9 (MIT) are fetched at build time; SDL3
(zlib), DXVK (zlib), ffmpeg (LGPL, linked dynamically), FreeType (FTL),
fontconfig (MIT), LLVM's demangler (Apache-2.0 with LLVM exception)
and, for the Xbox 360's XEX decryption tool, OpenSSL (Apache-2.0) come
from your system. Code taken from other projects keeps its notice (see
[NOTICE](NOTICE)).
