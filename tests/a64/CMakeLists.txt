# AArch64 lifter tests on the fixture (fixture/): built from source for
# aarch64-none-elf with clang's integrated assembler and LLD, lifted by
# recomp_lift_a64 at build time, and checked against the Unicorn oracle
# (oracle/oracle.py, run from the repository's .venv). No platform is
# needed: the fixture is a static-PIE ELF, which the lifter reads itself,
# and its two supervisor calls are the test platform's
# (fixture_platform.cpp).

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "a64: the AArch64 lifter tests build an AArch64 fixture with clang; skipped")
  return()
endif()
find_program(RECOMP_LD_LLD ld.lld)
if(NOT RECOMP_LD_LLD)
  message(STATUS "a64: ld.lld not found; the AArch64 lifter tests are skipped")
  return()
endif()

# --- the fixture -----------------------------------------------------------
# recomp_a64_fixture(<name> <crt0> <elf variable>): the fixture's C and
# assembly linked with <crt0> into <name>.elf in the current binary
# directory, for this core's tests or a platform that runs the same
# program in its own format with a crt0 of its own.
set(_a64_fixture_src "${CMAKE_CURRENT_SOURCE_DIR}/fixture" CACHE INTERNAL "")
function(recomp_a64_fixture name crt0 out)
  set(_src "${_a64_fixture_src}")
  set(_bin "${CMAKE_CURRENT_BINARY_DIR}/${name}")
  set(_flags --target=aarch64-none-elf -mcpu=cortex-a57 -mgeneral-regs-only
      -mno-outline-atomics -fPIE -ffreestanding -fno-stack-protector
      -fasynchronous-unwind-tables -O2 -Wall -Wextra -Werror)
  set(_objs)
  foreach(_f "${crt0}" "${_src}/ops.S" "${_src}/main.c")
    get_filename_component(_n "${_f}" NAME)
    add_custom_command(OUTPUT "${_bin}/${_n}.o"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bin}"
      COMMAND "${CMAKE_C_COMPILER}" ${_flags} -c "${_f}" -o "${_bin}/${_n}.o"
      DEPENDS "${_f}"
      COMMENT "Compiling the AArch64 fixture's ${_n} (${name})")
    list(APPEND _objs "${_bin}/${_n}.o")
  endforeach()
  add_custom_command(OUTPUT "${_bin}.elf"
    COMMAND "${RECOMP_LD_LLD}" -pie -z norelro -z text --eh-frame-hdr --export-dynamic
            --hash-style=sysv --no-dynamic-linker -T "${_src}/fixture.ld"
            -o "${_bin}.elf" ${_objs}
    DEPENDS ${_objs} "${_src}/fixture.ld"
    COMMENT "Linking the AArch64 fixture (${name})")
  set(${out} "${_bin}.elf" PARENT_SCOPE)
endfunction()
recomp_a64_fixture(fixture "${_a64_fixture_src}/crt0.S" _elf)
set(RECOMP_A64_FIXTURE_ELF "${_elf}" CACHE INTERNAL "")

# --- lifting it ----------------------------------------------------------
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/fixture_generated")
set(_funcs)
foreach(_k 000 001 002 003)
  list(APPEND _funcs "${_gen}/funcs_${_k}.cpp")
endforeach()
add_custom_command(OUTPUT ${_funcs} "${_gen}/table.cpp" "${_gen}/forms.cpp" "${_gen}/forms.txt"
         "${_gen}/forms_native.S"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
  COMMAND recomp_lift_a64 --image "${_elf}" --out "${_gen}" --files 4 --forms "${_gen}"
  DEPENDS recomp_lift_a64 "${_elf}"
  COMMENT "Lifting the fixture")

recomp_add_lifted_a64_files(a64_fixture_code ${_funcs} "${_gen}/table.cpp")
add_executable(a64_fixture fixture_main.cpp fixture_platform.cpp
               $<TARGET_OBJECTS:a64_fixture_code>)
target_link_libraries(a64_fixture PRIVATE recomp_a64 recomp_a64_image)

# a64_lift_forms_test, and the same built with each planted bug of
# a64/lifted.h (RECOMP_A64_MUTATION), which the test must catch. On arm64 hosts it also
# runs the original instructions on the CPU (forms_native.S).
set(_native)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  enable_language(ASM)
  set(_native "${_gen}/forms_native.S")
endif()
set(_mutations 1 2 3 4)
foreach(_m 0 ${_mutations})
  set(_t a64_lift_forms_test)
  if(_m)
    set(_t a64_lift_forms_mutation_${_m})
  endif()
  add_executable(${_t} lift_forms_test.cpp "${_gen}/forms.cpp" ${_native})
  target_include_directories(${_t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(${_t} PRIVATE RECOMP_A64_MUTATION=${_m})
  target_compile_options(${_t} PRIVATE -O1 -fno-strict-aliasing)
  target_link_libraries(${_t} PRIVATE recomp_a64_image recomp_a64)
endforeach()

# --- tests against the oracle ----------------------------------------------
set(RECOMP_PYTHON "${RECOMP_ROOT}/.venv/bin/python" CACHE FILEPATH
    "Python with unicorn for the AArch64 oracle tests")
if(NOT EXISTS "${RECOMP_PYTHON}")
  message(STATUS "a64: ${RECOMP_PYTHON} missing; the AArch64 oracle tests are skipped")
  return()
endif()
set(RECOMP_A64_ORACLE "${CMAKE_CURRENT_SOURCE_DIR}/oracle/oracle.py" CACHE INTERNAL "")
set(_vectors "${CMAKE_CURRENT_BINARY_DIR}/forms_vectors.bin")

add_test(NAME a64_lift_fixture
  COMMAND "${RECOMP_PYTHON}" "${RECOMP_A64_ORACLE}" run --elf "${_elf}"
          --lifted ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:a64_fixture>)
add_test(NAME a64_lift_forms_vectors
  COMMAND "${RECOMP_PYTHON}" "${RECOMP_A64_ORACLE}" forms --elf "${_elf}"
          --forms "${_gen}/forms.txt" --out "${_vectors}")
set_tests_properties(a64_lift_forms_vectors PROPERTIES FIXTURES_SETUP a64_forms_vectors)
add_test(NAME a64_lift_forms COMMAND a64_lift_forms_test "${_elf}" "${_vectors}")
set_tests_properties(a64_lift_forms PROPERTIES FIXTURES_REQUIRED a64_forms_vectors)
foreach(_m ${_mutations})
  add_test(NAME a64_lift_forms_mutation_${_m}
    COMMAND a64_lift_forms_mutation_${_m} "${_elf}" "${_vectors}")
  set_tests_properties(a64_lift_forms_mutation_${_m} PROPERTIES
    FIXTURES_REQUIRED a64_forms_vectors WILL_FAIL TRUE)
endforeach()
