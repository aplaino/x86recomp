# AArch64 lifter tests on the fixture NRO (fixture/): built from source for
# aarch64-none-elf with clang's integrated assembler and LLD, lifted by
# recomp_lift_a64 at build time, and checked against the Unicorn oracle
# (oracle/oracle.py, run from the repository's .venv).

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "horizon: the AArch64 lifter tests build an AArch64 fixture with clang; skipped")
  return()
endif()
find_program(RECOMP_LD_LLD ld.lld)
if(NOT RECOMP_LD_LLD)
  message(STATUS "horizon: ld.lld not found; the AArch64 lifter tests are skipped")
  return()
endif()

# --- the fixture NRO ---------------------------------------------------
set(_src "${CMAKE_CURRENT_SOURCE_DIR}/fixture")
set(_bin "${CMAKE_CURRENT_BINARY_DIR}/fixture")
set(_flags --target=aarch64-none-elf -mcpu=cortex-a57 -mgeneral-regs-only
    -mno-outline-atomics -fPIE -ffreestanding -fno-stack-protector
    -fasynchronous-unwind-tables -O2 -Wall -Wextra -Werror)
set(_objs)
foreach(_f crt0.S ops.S main.c)
  add_custom_command(OUTPUT "${_bin}/${_f}.o"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bin}"
    COMMAND "${CMAKE_C_COMPILER}" ${_flags} -c "${_src}/${_f}" -o "${_bin}/${_f}.o"
    DEPENDS "${_src}/${_f}"
    COMMENT "Compiling the AArch64 fixture's ${_f}")
  list(APPEND _objs "${_bin}/${_f}.o")
endforeach()
add_custom_command(OUTPUT "${_bin}/fixture.elf"
  COMMAND "${RECOMP_LD_LLD}" -pie -z norelro -z text --eh-frame-hdr --export-dynamic
          --hash-style=sysv --no-dynamic-linker -T "${_src}/fixture.ld"
          -o "${_bin}/fixture.elf" ${_objs}
  DEPENDS ${_objs} "${_src}/fixture.ld"
  COMMENT "Linking the AArch64 fixture")
set(_nro "${_bin}/fixture.nro")
add_custom_command(OUTPUT "${_nro}"
  COMMAND horizon_elf2nro "${_bin}/fixture.elf" "${_nro}"
  DEPENDS horizon_elf2nro "${_bin}/fixture.elf")

# --- lifting it ----------------------------------------------------------
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/fixture_generated")
set(_funcs)
foreach(_k 000 001 002 003)
  list(APPEND _funcs "${_gen}/funcs_${_k}.cpp")
endforeach()
add_custom_command(OUTPUT ${_funcs} "${_gen}/table.cpp" "${_gen}/forms.cpp" "${_gen}/forms.txt"
         "${_gen}/forms_native.S"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
  COMMAND recomp_lift_a64 --image "${_nro}" --out "${_gen}" --files 4 --forms "${_gen}"
  DEPENDS recomp_lift_a64 "${_nro}"
  COMMENT "Lifting the fixture")

recomp_add_lifted_a64_files(horizon_fixture_code ${_funcs} "${_gen}/table.cpp")
add_executable(horizon_fixture fixture_main.cpp $<TARGET_OBJECTS:horizon_fixture_code>)
target_link_libraries(horizon_fixture PRIVATE recomp_horizon)

# horizon_lift_forms_test, and the same built with each planted bug of
# a64/lifted.h (RECOMP_A64_MUTATION), which the test must catch. On arm64 hosts it also
# runs the original instructions on the CPU (forms_native.S).
set(_native)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  enable_language(ASM)
  set(_native "${_gen}/forms_native.S")
endif()
set(_mutations 1 2 3 4)
foreach(_m 0 ${_mutations})
  set(_t horizon_lift_forms_test)
  if(_m)
    set(_t horizon_lift_forms_mutation_${_m})
  endif()
  add_executable(${_t} lift_forms_test.cpp "${_gen}/forms.cpp" ${_native})
  target_include_directories(${_t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(${_t} PRIVATE RECOMP_A64_MUTATION=${_m})
  target_compile_options(${_t} PRIVATE -O1 -fno-strict-aliasing)
  target_link_libraries(${_t} PRIVATE recomp_horizon_image recomp_a64)
endforeach()

# --- tests against the oracle ----------------------------------------------
set(RECOMP_PYTHON "${RECOMP_ROOT}/.venv/bin/python" CACHE FILEPATH
    "Python with unicorn for the AArch64 oracle tests")
if(NOT EXISTS "${RECOMP_PYTHON}")
  message(STATUS "horizon: ${RECOMP_PYTHON} missing; the AArch64 oracle tests are skipped")
  return()
endif()
set(_oracle "${CMAKE_CURRENT_SOURCE_DIR}/oracle/oracle.py")
set(_vectors "${CMAKE_CURRENT_BINARY_DIR}/forms_vectors.bin")

add_test(NAME horizon_lift_fixture
  COMMAND "${RECOMP_PYTHON}" "${_oracle}" run --nro "${_nro}"
          --lifted ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:horizon_fixture>)
add_test(NAME horizon_lift_forms_vectors
  COMMAND "${RECOMP_PYTHON}" "${_oracle}" forms --nro "${_nro}"
          --forms "${_gen}/forms.txt" --out "${_vectors}")
set_tests_properties(horizon_lift_forms_vectors PROPERTIES FIXTURES_SETUP horizon_forms_vectors)
add_test(NAME horizon_lift_forms COMMAND horizon_lift_forms_test "${_nro}" "${_vectors}")
set_tests_properties(horizon_lift_forms PROPERTIES FIXTURES_REQUIRED horizon_forms_vectors)
foreach(_m ${_mutations})
  add_test(NAME horizon_lift_forms_mutation_${_m}
    COMMAND horizon_lift_forms_mutation_${_m} "${_nro}" "${_vectors}")
  set_tests_properties(horizon_lift_forms_mutation_${_m} PROPERTIES
    FIXTURES_REQUIRED horizon_forms_vectors WILL_FAIL TRUE)
endforeach()
