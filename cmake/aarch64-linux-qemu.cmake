# Cross build for aarch64 Linux, run under qemu-aarch64 (user mode): the
# arm64-host build of the runtime and lifted code, and the forms test's
# native oracle, on an x86-64 machine. The sysroot is crossdev's
# (crossdev -t aarch64-unknown-linux-gnu). QEMU's CPU is emulated, so this
# checks portability and the test harness; only real arm64 hardware is an
# oracle independent of Unicorn.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(RECOMP_AARCH64_TRIPLE aarch64-unknown-linux-gnu)
set(RECOMP_AARCH64_SYSROOT "/usr/${RECOMP_AARCH64_TRIPLE}" CACHE PATH "aarch64 Linux sysroot")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${RECOMP_AARCH64_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${RECOMP_AARCH64_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${RECOMP_AARCH64_TRIPLE})
set(CMAKE_SYSROOT "${RECOMP_AARCH64_SYSROOT}")
# crossdev installs the cross GCC (crt files, libstdc++) under the host's
# /usr/lib/gcc, not in the sysroot.
set(CMAKE_C_FLAGS_INIT "--gcc-toolchain=/usr")
set(CMAKE_CXX_FLAGS_INIT "--gcc-toolchain=/usr")
foreach(_kind EXE SHARED MODULE)
  set(CMAKE_${_kind}_LINKER_FLAGS_INIT "-fuse-ld=lld --gcc-toolchain=/usr")
endforeach()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)  # host tools (ld.lld, python)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

file(GLOB _gcc_lib LIST_DIRECTORIES true "/usr/lib/gcc/${RECOMP_AARCH64_TRIPLE}/*")
list(SORT _gcc_lib COMPARE NATURAL ORDER DESCENDING)
list(GET _gcc_lib 0 _gcc_lib)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64 -L "${RECOMP_AARCH64_SYSROOT}"
    -E "LD_LIBRARY_PATH=${_gcc_lib}")
