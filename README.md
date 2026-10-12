# x86recomp

Static recompilation of 32-bit x86 Windows games into native 64-bit
programs: the scaffold that gets a game running natively while its code
is replaced, piece by piece, with hand-written C++. A game's machine code is translated ahead of time into C++ and
linked against a runtime that reimplements the parts of Windows and its
middleware the game uses. There is no emulator, interpreter or Wine in
the result, and no game code or data in this repository.

Used by:
- [DS1-Rebuilt](https://github.com/JTM-rootstorm/DS1-Rebuilt) (Dungeon
  Siege, 2002), where it was developed;
- [CoD1-Reissued](https://github.com/JTM-rootstorm/CoD1-Reissued) (Call
  of Duty, 2003), in progress.

## What is here

- **Lifter** (`tools/lifter`, `recomp_lift`): decodes a PE with Zydis and
  emits C++ for every function, one basic block at a time, with exact
  x86 integer and flag semantics. Straight-line x87 code becomes guarded
  "runs" that keep the FPU register stack in locals.
- **x87 model** (`src/runtime/x87.cpp`): exact 80-bit x87 (precision and
  rounding control, status-word flags) on Berkeley SoftFloat with
  integer fast paths, tested bit for bit against a real x87.
- **SSE/SSE2** (`src/runtime/sse.cpp`): the XMM registers and MXCSR,
  with SoftFloat lanes and x86 NaN rules, tested bit for bit against the
  CPU like the x87 model.
- **Runtime** (`src/runtime`): the guest's 4 GiB address space, PE
  loading, threads, SEH, crash and hang reports.
- **HLE** (`src/runtime/hle`): Win32 (kernel32, user32, gdi32, advapi32,
  shfolder, ...) on POSIX and SDL3; optional modules for Miles Sound
  System, Bink, DirectDraw 7/Direct3D 7 (on dxvk-native's Direct3D 9) and
  dbghelp.
- **Tools**: Ghidra export scripts (`tools/ghidra`), a PE symbol dumper
  (`tools/pe_symbols.py`), dev-time reference checks (`tools/layout`,
  `tools/undname`).

[ARCHITECTURE.md](ARCHITECTURE.md) describes the design.

## Using it from a game project

```cmake
set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../x86recomp" CACHE PATH "")
enable_testing()
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

recomp_add_lifted(mygame_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")
add_executable(mygame src/main.cpp $<TARGET_OBJECTS:mygame_lifted>)
target_link_libraries(mygame PRIVATE recomp_runtime recomp_mss32)
```

`main()` fills a `recomp::GameConfig` (id, guest install directory, EXE
name, registry seed, optional hooks) and calls `recomp::RunProcess`.
The lifter runs as a separate step over the user's own EXE:

```sh
recomp_lift --exe game.exe --symbols symbols --out generated \
  --manifest lifter.manifest
```

DLLs the game loads itself are lifted in the same run (`--module
game.dll --symbols dir [--manifest file]`, repeated) and listed in
`GameConfig::lifted_dlls`.

## Requirements

Linux x86-64 for now (macOS arm64 with Metal is planned; the code is
kept portable). Clang and LLVM (the static `libLLVMDemangle.a` for
dbghelp), CMake 3.25+, Ninja, SDL3; ffmpeg for Miles/Bink; a
dxvk-native build (Direct3D 9 only, SDL3 window system) for D3D7.

## Licence

MIT (see [LICENSE](LICENSE)). Dependencies keep their own licences:
Berkeley SoftFloat 3e (BSD-3-Clause) and Zydis (MIT) are fetched at
build time; SDL3 (zlib), DXVK (zlib), ffmpeg (LGPL, linked dynamically)
and LLVM's demangler (Apache-2.0 with LLVM exception) come from your
system.
