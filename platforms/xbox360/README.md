# The Xbox 360 platform module

Static recompilation of Xbox 360 titles into native 64-bit programs, on
x86recomp's shared core: a title's PowerPC machine code is lifted ahead
of time to C++ (`recomp_lift_ppc`, the PowerPC core in `tools/lifter_ppc`
and `src/ppc`) and linked against a runtime that reimplements the
console's kernel, system library and GPU. Built only with
`RECOMP_PLATFORM_XBOX360`; the x86 path (`RECOMP_X86`) can be off.

Status: milestones 1-3 of [ARCHITECTURE.md](ARCHITECTURE.md). The XEX2
reader (with its own LZX decoder) and the integer, floating-point, VMX
and VMX128 core lift system modules and titles with every instruction
decoded, and a fixture runs lifted with the same output as on a PowerPC
970 in 32-bit mode, every instruction form checked bit for bit against it
(VMX128's as their VMX equivalents). The runtime is the design being
built.

## Using it

```sh
# An encrypted XEX first needs the owner's key (never part of this tree):
out/build/xbox360/platforms/xbox360/xbox360_xex_decrypt default.xex default_plain.xex --key-file my.key
out/build/xbox360/tools/lifter_ppc/recomp_lift_ppc --image default_plain.xex --out generated
```

```cmake
set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../x86recomp" CACHE PATH "")
set(RECOMP_X86 OFF CACHE BOOL "")
set(RECOMP_PLATFORM_XBOX360 ON CACHE BOOL "")
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

recomp_add_lifted_ppc(mygame_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")
add_executable(mygame src/main.cpp $<TARGET_OBJECTS:mygame_lifted>)
target_link_libraries(mygame PRIVATE recomp_ppc recomp_xbox360_image)
```
