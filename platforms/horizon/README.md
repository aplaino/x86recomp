# The Switch (Horizon OS) platform module

Static recompilation of Nintendo Switch games into native 64-bit
programs, on x86recomp's shared core: a game's AArch64 machine code is
lifted ahead of time to C++ (`recomp_lift_a64`, the AArch64 core in
`tools/lifter_a64` and `src/a64`) and linked against a runtime that
reimplements the parts of the console's SDK (nnSdk), its graphics API
(NVN) and its services the game uses. Built only with
`RECOMP_PLATFORM_HORIZON`; the x86 path (`RECOMP_X86`) can be off.

Status: milestone 1 of [ARCHITECTURE.md](ARCHITECTURE.md). The lifter and
an integer-only runtime lift and run a homebrew NRO fixture, checked
against Unicorn (and against the CPU on arm64 hosts). The rest of
ARCHITECTURE.md is the design being built.

## Using it from a game project

```cmake
set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../x86recomp" CACHE PATH "")
set(RECOMP_X86 OFF CACHE BOOL "")
set(RECOMP_PLATFORM_HORIZON ON CACHE BOOL "")
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)
enable_testing()
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

recomp_add_lifted_a64(mygame_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")
add_executable(mygame src/main.cpp $<TARGET_OBJECTS:mygame_lifted>)
target_link_libraries(mygame PRIVATE recomp_horizon)
```

`main()` fills a `recomp::horizon::GameConfig` and calls
`recomp::horizon::RunProcess`. The lifter runs as a separate step over
the user's own extracted modules:

```sh
recomp_lift_a64 --image module.nro --out generated
```

## Requirements

Linux x86-64, or macOS on Apple Silicon. Clang, LLD and CMake/Ninja, and
the host layer every guest shares: SDL3, ffmpeg, LLVM (its demangler),
glslang (and SPIRV-Cross on macOS). For the tests: Python 3 with `unicorn` in the repository's `.venv/`
(gitignored); Unicorn is used by tests only, never by a product. An arm64
host build can be checked on x86-64 with crossdev's
`aarch64-unknown-linux-gnu` toolchain and `qemu-aarch64` (the
`horizon-aarch64-qemu` preset, CPU-only: the lifter and runtime without
the host layer). The fixtures the tests need are checked
in as assembly and C built with clang's integrated assembler and LLD.

No game code, assets, keys or firmware are in this repository or in any
game project: a game project builds from the owner's own extracted
`exefs` and `romfs`, and the lifted code lives in a gitignored
`generated/` directory.
