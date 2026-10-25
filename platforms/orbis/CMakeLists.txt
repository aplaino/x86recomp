# The PS4 (Orbis OS) platform module (ARCHITECTURE.md, "Console
# platforms"): built only with RECOMP_PLATFORM_ORBIS.

# Orbis ELF mapping and NIDs, for the lifter and the runtime.
add_library(recomp_orbis_image SHARED
  src/orbis/elf_image.cpp
  src/orbis/nid.cpp)
target_include_directories(recomp_orbis_image PUBLIC src)
target_link_libraries(recomp_orbis_image PUBLIC recomp_image)

# The process loader (orbis/process.h) and the HLE registration by NID
# (orbis/hle.h); the system libraries' HLE builds on it.
add_library(recomp_orbis SHARED src/orbis/process.cpp)
target_link_libraries(recomp_orbis PUBLIC recomp_runtime recomp_orbis_image)

# System library HLE, one shared library each.
add_library(recomp_orbis_kernel SHARED src/orbis/hle/kernel.cpp src/orbis/hle/kernel_memory.cpp
            src/orbis/hle/kernel_pthread.cpp src/orbis/hle/kernel_sync.cpp
            src/orbis/hle/kernel_time.cpp src/orbis/hle/kernel_file.cpp)
target_link_libraries(recomp_orbis_kernel PUBLIC recomp_orbis)
add_library(recomp_orbis_libc_internal SHARED src/orbis/hle/libc_internal.cpp)
target_link_libraries(recomp_orbis_libc_internal PUBLIC recomp_orbis)
# libSceSystemService, libSceUserService, libSceSysmodule.
add_library(recomp_orbis_system SHARED src/orbis/hle/system.cpp)
target_link_libraries(recomp_orbis_system PUBLIC recomp_orbis)

# recomp_lift reads Orbis ELFs.
target_sources(recomp_lift PRIVATE lifter/orbis_reader.cpp)
target_include_directories(recomp_lift PRIVATE ${RECOMP_ROOT}/tools/lifter)
target_link_libraries(recomp_lift PRIVATE recomp_orbis_image)

add_executable(orbis_image_test tests/orbis_image_test.cpp)
target_link_libraries(orbis_image_test PRIVATE recomp_orbis_image)
add_test(NAME orbis_image COMMAND orbis_image_test)

# orbis_process_test: tests/e2e (C for FreeBSD x86-64, linked by ld.lld
# and made SCE ELFs) lifted at build time and run through the loader.
# Needs clang and ld.lld.
find_program(RECOMP_LD_LLD ld.lld HINTS "${RECOMP_LLVM_ROOT}/bin")
find_package(Python3 COMPONENTS Interpreter)
if(RECOMP_LD_LLD AND Python3_FOUND AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(_e2e "${CMAKE_CURRENT_BINARY_DIR}/e2e")
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e")
  add_custom_command(OUTPUT "${_e2e}/eboot.elf" "${_e2e}/prx.elf"
    COMMAND Python3::Interpreter "${_src}/build.py" "${_e2e}" "${CMAKE_C_COMPILER}"
            "${RECOMP_LD_LLD}"
    DEPENDS "${_src}/build.py" "${_src}/eboot.c" "${_src}/kernel.c" "${_src}/prx.c"
            "${_src}/stubs.c"
            "${RECOMP_ROOT}/tools/orbis_sce_elf.py"
    VERBATIM)
  set(_gen "${_e2e}/gen/gen_000.cpp" "${_e2e}/gen/gen_prx_000.cpp" "${_e2e}/gen/table.cpp")
  add_custom_command(OUTPUT ${_gen}
    COMMAND recomp_lift --exe "${_e2e}/eboot.elf" --symbols "" --manifest ""
            --module "${_e2e}/prx.elf" --symbols "" --base 800000000 --out "${_e2e}/gen"
    COMMAND ${CMAKE_COMMAND} -E touch ${_gen}
    DEPENDS recomp_lift "${_e2e}/eboot.elf" "${_e2e}/prx.elf"
    VERBATIM)
  set_source_files_properties(${_gen} PROPERTIES COMPILE_OPTIONS
    "-O2;-g0;-fno-strict-aliasing;-fnon-call-exceptions;${RECOMP_X87_FP_FLAGS};${RECOMP_FAULT_FRAME_FLAGS}")
  add_executable(orbis_process_test tests/orbis_process_test.cpp ${_gen}
                 "${RECOMP_ROOT}/src/runtime/lifted_table.cpp")
  target_link_libraries(orbis_process_test PRIVATE recomp_orbis recomp_orbis_kernel)
  add_test(NAME orbis_process COMMAND orbis_process_test "${_e2e}/eboot.elf" "${_e2e}/prx.elf")
  set_tests_properties(orbis_process PROPERTIES TIMEOUT 60)  # a hang is a failure
endif()
