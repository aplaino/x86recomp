# The Xbox 360 platform module

Static recompilation of Xbox 360 titles into native 64-bit programs, on
x86recomp's shared core: a title's PowerPC machine code is lifted ahead
of time to C++ (`recomp_lift_ppc`, the PowerPC core in `tools/lifter_ppc`
and `src/ppc`) and linked against a runtime that reimplements the
console's kernel, system library and GPU. Built only with
`RECOMP_PLATFORM_XBOX360`; the x86 path (`RECOMP_X86`) can be off.

Status: milestones 1-3 of [ARCHITECTURE.md](ARCHITECTURE.md), and the
runtime (milestone 4) begun. The XEX2 reader (with its own LZX decoder)
and the integer, floating-point, VMX and VMX128 core lift system modules
and titles with every instruction decoded, and a fixture runs lifted with
the same output as on a PowerPC 970 in 32-bit mode, every instruction
form checked bit for bit against it (VMX128's as their VMX equivalents).
The runtime maps the console's address space with its physical views,
runs a title's threads with their kernel structures, and has the
kernel's memory, thread, synchronisation, file, module, settings,
display and audio driver, string and formatting exports and the first of
xam's startup ones (users with a local profile signed in, profile
settings, notifications, content packages as host directories) and its
message dispatch, timers, with a GPU command processor that executes a
title's ring buffer without drawing and audio output through SDL3, which
a test title checks; the Live apps' messages and XMA decoding follow.

## Using it

```sh
# An encrypted XEX first needs the owner's key (never part of this tree):
out/build/xbox360/bin/xbox360_xex_decrypt default.xex default_plain.xex --key-file my.key
out/build/xbox360/bin/recomp_lift_ppc --image default_plain.xex --out generated
```

```cmake
set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../x86recomp" CACHE PATH "")
set(RECOMP_X86 OFF CACHE BOOL "")
set(RECOMP_PLATFORM_XBOX360 ON CACHE BOOL "")
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

# The lifted title is a library of its own, next to the program.
recomp_add_lifted_ppc(mygame_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated" LINK recomp_xbox360)
add_executable(mygame src/main.cpp)
target_link_libraries(mygame PRIVATE recomp_xbox360 recomp_xbox360_xboxkrnl recomp_xbox360_xam mygame_lifted)
recomp_bundle(mygame)
```

```cpp
#include "xbox360/process.h"

int main() { recomp::xbox360::RunProcess({"MYGAME", "default_plain.xex"}); }
```

The build's `bin/` then holds the program and every library it loads
(the lifted title, the runtime, the HLE modules), found relative to the
program: copy the directory to install it, replace one library to update
that part.
