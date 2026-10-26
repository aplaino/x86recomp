# PowerPC lifter tests on the fixture (fixture/): built from source for
# big-endian PowerPC64 with clang's integrated assembler and LLD, lifted
# by recomp_lift_ppc at build time, and checked against the oracle: the
# same code bare metal on qemu-system-ppc64, a PowerPC 970 in 32-bit mode
# (oracle/qemu_run.py).

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "ppc: the PowerPC lifter tests build a PowerPC fixture with clang; skipped")
  return()
endif()
find_program(RECOMP_LD_LLD ld.lld)
if(NOT RECOMP_LD_LLD)
  message(STATUS "ppc: ld.lld not found; the PowerPC lifter tests are skipped")
  return()
endif()

set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin "${CMAKE_CURRENT_BINARY_DIR}/ppc")
set(RECOMP_PPC_TEST_FLAGS --target=powerpc64-unknown-linux-gnu -mabi=elfv2 -mcpu=970 -maltivec
    -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-jump-tables -fno-math-errno -O2
    -Wall -Wextra -Werror -I "${_src}/oracle" CACHE INTERNAL "")

# Compiles PowerPC sources into objects in the calling directory's ppc/:
# ppc_objects(<var> <file>...). Platform modules' tests use it too.
function(ppc_objects var)
  set(_objs)
  foreach(_f ${ARGN})
    get_filename_component(_name "${_f}" NAME)
    set(_o "${CMAKE_CURRENT_BINARY_DIR}/ppc/${_name}.o")
    add_custom_command(OUTPUT "${_o}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/ppc"
      COMMAND "${CMAKE_C_COMPILER}" ${RECOMP_PPC_TEST_FLAGS} -c "${_f}" -o "${_o}"
      DEPENDS "${_f}" "${RECOMP_ROOT}/tests/ppc/oracle/oracle.h"
      COMMENT "Compiling the PowerPC test program's ${_name}")
    list(APPEND _objs "${_o}")
  endforeach()
  set(${var} ${_objs} PARENT_SCOPE)
endfunction()

# --- the interrupt vectors of the oracle runs ---------------------------------
ppc_objects(_vec_objs "${_src}/oracle/vectors.S")
add_custom_command(OUTPUT "${_bin}/vectors.elf"
  COMMAND "${RECOMP_LD_LLD}" -static -T "${_src}/oracle/vectors.ld" -o "${_bin}/vectors.elf" ${_vec_objs}
  DEPENDS ${_vec_objs} "${_src}/oracle/vectors.ld")

# --- the fixture -----------------------------------------------------------------
ppc_objects(_fix_objs "${_src}/oracle/crt0.S" "${_src}/fixture/ops.S" "${_src}/fixture/tables.S"
            "${_src}/fixture/main.c")
add_custom_command(OUTPUT "${_bin}/fixture.elf"
  COMMAND "${RECOMP_LD_LLD}" -static -T "${_src}/fixture/fixture.ld" -o "${_bin}/fixture.elf" ${_fix_objs}
  DEPENDS ${_fix_objs} "${_src}/fixture/fixture.ld"
  COMMENT "Linking the PowerPC fixture")

# --- lifting it --------------------------------------------------------------
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/fixture_generated")
set(_funcs)
foreach(_k 000 001 002 003)
  list(APPEND _funcs "${_gen}/funcs_${_k}.cpp")
endforeach()
add_custom_command(OUTPUT ${_funcs} "${_gen}/table.cpp" "${_gen}/forms.cpp" "${_gen}/forms_native.S"
         "${_gen}/forms_native.c"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
  COMMAND recomp_lift_ppc --image "${_bin}/fixture.elf" --out "${_gen}" --files 4 --forms "${_gen}"
  DEPENDS recomp_lift_ppc "${_bin}/fixture.elf"
  COMMENT "Lifting the PowerPC fixture")

add_library(ppc_fixture_platform SHARED fixture_platform.cpp)
target_include_directories(ppc_fixture_platform PRIVATE "${_src}/oracle")
target_link_libraries(ppc_fixture_platform PUBLIC recomp_ppc)
recomp_bundle(ppc_fixture_platform)
recomp_add_lifted_ppc_files(ppc_fixture_code ${_funcs} "${_gen}/table.cpp" LINK ppc_fixture_platform)
add_executable(ppc_fixture fixture_main.cpp "${RECOMP_ROOT}/tools/lifter_ppc/elf.cpp")
target_include_directories(ppc_fixture PRIVATE "${RECOMP_ROOT}/tools/lifter_ppc")
target_link_libraries(ppc_fixture PRIVATE recomp_ppc ppc_fixture_platform ppc_fixture_code)
recomp_bundle(ppc_fixture)

# --- the form oracle: every form's native stub, bare metal -------------------
ppc_objects(_forms_objs "${_src}/oracle/crt0.S" "${_src}/oracle/forms_main.c" "${_gen}/forms_native.S"
            "${_gen}/forms_native.c")
add_custom_command(OUTPUT "${_bin}/forms_oracle.elf"
  COMMAND "${RECOMP_LD_LLD}" -static -T "${_src}/oracle/forms.ld" -o "${_bin}/forms_oracle.elf" ${_forms_objs}
  DEPENDS ${_forms_objs} "${_src}/oracle/forms.ld"
  COMMENT "Linking the PowerPC form oracle")
add_custom_target(ppc_oracle_programs ALL
  DEPENDS "${_bin}/vectors.elf" "${_bin}/fixture.elf" "${_bin}/forms_oracle.elf")

# ppc_lift_forms_test, and the same built with each planted bug of
# ppc/lifted.h, ppc/vmx.h, ppc/fp.cpp and ppc/vmx.cpp (RECOMP_PPC_MUTATION),
# which the test must catch. Each builds its own fp.cpp and vmx.cpp, which
# take precedence over recomp_ppc's.
set(_mutations 1 2 3 4 5 6 7 8 9 10)
foreach(_m 0 ${_mutations})
  set(_t ppc_lift_forms_test)
  if(_m)
    set(_t ppc_lift_forms_mutation_${_m})
  endif()
  add_executable(${_t} lift_forms_test.cpp "${_gen}/forms.cpp" "${RECOMP_ROOT}/src/ppc/fp.cpp"
                 "${RECOMP_ROOT}/src/ppc/vmx.cpp")
  target_include_directories(${_t} PRIVATE "${_src}" "${_src}/oracle")
  target_compile_definitions(${_t} PRIVATE RECOMP_PPC_MUTATION=${_m})
  target_compile_options(${_t} PRIVATE -O1 -fno-strict-aliasing)
  target_link_libraries(${_t} PRIVATE recomp_ppc softfloat)
endforeach()

# ppc_vmx128_test: VMX128's decoding and its operations without a VMX
# twin (the oracle has none of VMX128), with its own planted bugs.
set(_vmx128_mutations 11 12)
foreach(_m 0 ${_vmx128_mutations})
  set(_t ppc_vmx128_test)
  if(_m)
    set(_t ppc_vmx128_mutation_${_m})
  endif()
  add_executable(${_t} vmx128_test.cpp "${RECOMP_ROOT}/tools/lifter_ppc/ppc.cpp" "${RECOMP_ROOT}/src/ppc/vmx.cpp")
  target_include_directories(${_t} PRIVATE "${RECOMP_ROOT}/tools/lifter_ppc")
  target_compile_definitions(${_t} PRIVATE RECOMP_PPC_MUTATION=${_m})
  target_compile_features(${_t} PRIVATE cxx_std_20)
  target_link_libraries(${_t} PRIVATE recomp_ppc softfloat)
  add_test(NAME ${_t} COMMAND ${_t})
  if(_m)
    set_tests_properties(${_t} PROPERTIES WILL_FAIL TRUE)
  endif()
endforeach()

# --- tests against the oracle ----------------------------------------------
find_program(RECOMP_QEMU_PPC64 qemu-system-ppc64)
if(NOT RECOMP_QEMU_PPC64)
  message(STATUS "ppc: qemu-system-ppc64 not found; the PowerPC oracle tests are skipped")
  return()
endif()
set(RECOMP_PYTHON "${RECOMP_ROOT}/.venv/bin/python" CACHE FILEPATH
    "Python for the oracle tests' scripts")
if(NOT EXISTS "${RECOMP_PYTHON}")
  message(STATUS "ppc: ${RECOMP_PYTHON} missing; the PowerPC oracle tests are skipped")
  return()
endif()
set(_run "${RECOMP_PYTHON}" "${_src}/oracle/qemu_run.py" --qemu "${RECOMP_QEMU_PPC64}"
    --vectors "${_bin}/vectors.elf")
set(_fixture_out "${CMAKE_CURRENT_BINARY_DIR}/fixture_oracle.bin")
set(_records "${CMAKE_CURRENT_BINARY_DIR}/forms_records.bin")

add_test(NAME ppc_oracle_fixture COMMAND ${_run} --program "${_bin}/fixture.elf" --out "${_fixture_out}")
set_tests_properties(ppc_oracle_fixture PROPERTIES FIXTURES_SETUP ppc_fixture_out)
add_test(NAME ppc_lift_fixture COMMAND ppc_fixture "${_bin}/fixture.elf" "${_fixture_out}")
set_tests_properties(ppc_lift_fixture PROPERTIES FIXTURES_REQUIRED ppc_fixture_out)
# Each of tables.S's jump tables recognised: an unrecognised one still runs
# (dispatched, its cases lifted through the table's pointers), so only the
# lifter's count shows it.
set(_relift "${CMAKE_CURRENT_BINARY_DIR}/fixture_relift")
file(MAKE_DIRECTORY "${_relift}")
add_test(NAME ppc_lift_fixture_tables COMMAND recomp_lift_ppc --image "${_bin}/fixture.elf" --out "${_relift}" --files 1)
set_tests_properties(ppc_lift_fixture_tables PROPERTIES PASS_REGULAR_EXPRESSION " 5 jump tables,")

add_test(NAME ppc_oracle_forms COMMAND ${_run} --program "${_bin}/forms_oracle.elf" --out "${_records}")
set_tests_properties(ppc_oracle_forms PROPERTIES FIXTURES_SETUP ppc_forms_records)
add_test(NAME ppc_lift_forms COMMAND ppc_lift_forms_test "${_records}")
set_tests_properties(ppc_lift_forms PROPERTIES FIXTURES_REQUIRED ppc_forms_records)
foreach(_m ${_mutations})
  add_test(NAME ppc_lift_forms_mutation_${_m} COMMAND ppc_lift_forms_mutation_${_m} "${_records}")
  set_tests_properties(ppc_lift_forms_mutation_${_m} PROPERTIES
    FIXTURES_REQUIRED ppc_forms_records WILL_FAIL TRUE)
endforeach()
