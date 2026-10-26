# PowerPC lifter tests on the fixture (fixture/): built from source for
# big-endian PowerPC64 with clang's integrated assembler and LLD, turned
# into a XEX by xbox360_elf2xex, lifted by recomp_lift_ppc at build time,
# and checked against the oracle: the same code bare metal on
# qemu-system-ppc64, a PowerPC 970 in 32-bit mode (oracle/qemu_run.py).

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "xbox360: the PowerPC lifter tests build a PowerPC fixture with clang; skipped")
  return()
endif()
find_program(RECOMP_LD_LLD ld.lld)
if(NOT RECOMP_LD_LLD)
  message(STATUS "xbox360: ld.lld not found; the PowerPC lifter tests are skipped")
  return()
endif()

set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin "${CMAKE_CURRENT_BINARY_DIR}/ppc")
set(_flags --target=powerpc64-unknown-linux-gnu -mabi=elfv2 -mcpu=970 -maltivec
    -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-jump-tables -fno-math-errno -O2
    -Wall -Wextra -Werror -I "${_src}/oracle")

# Compiles PowerPC sources into objects: ppc_objects(<var> <file>...).
function(ppc_objects var)
  set(_objs)
  foreach(_f ${ARGN})
    get_filename_component(_name "${_f}" NAME)
    set(_o "${_bin}/${_name}.o")
    add_custom_command(OUTPUT "${_o}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bin}"
      COMMAND "${CMAKE_C_COMPILER}" ${_flags} -c "${_f}" -o "${_o}"
      DEPENDS "${_f}" "${_src}/oracle/oracle.h"
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
set(_xex "${_bin}/fixture.xex")
add_custom_command(OUTPUT "${_xex}"
  COMMAND xbox360_elf2xex "${_bin}/fixture.elf" "${_xex}"
  DEPENDS xbox360_elf2xex "${_bin}/fixture.elf")

# --- lifting it --------------------------------------------------------------
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/fixture_generated")
set(_funcs)
foreach(_k 000 001 002 003)
  list(APPEND _funcs "${_gen}/funcs_${_k}.cpp")
endforeach()
add_custom_command(OUTPUT ${_funcs} "${_gen}/table.cpp" "${_gen}/forms.cpp" "${_gen}/forms_native.S"
         "${_gen}/forms_native.c"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
  COMMAND recomp_lift_ppc --image "${_xex}" --out "${_gen}" --files 4 --forms "${_gen}"
  DEPENDS recomp_lift_ppc "${_xex}"
  COMMENT "Lifting the PowerPC fixture")

add_library(xbox360_fixture_platform SHARED fixture_platform.cpp)
target_include_directories(xbox360_fixture_platform PRIVATE "${_src}/oracle")
target_link_libraries(xbox360_fixture_platform PUBLIC recomp_ppc)
recomp_bundle(xbox360_fixture_platform)
recomp_add_lifted_ppc_files(xbox360_fixture_code ${_funcs} "${_gen}/table.cpp" LINK xbox360_fixture_platform)
add_executable(xbox360_fixture fixture_main.cpp)
target_link_libraries(xbox360_fixture PRIVATE recomp_ppc recomp_xbox360_image xbox360_fixture_platform
                      xbox360_fixture_code)
recomp_bundle(xbox360_fixture)

# --- the runtime test title ------------------------------------------------------
# A title that calls the kernel through imports (title/), lifted into a
# lifted module and run on the runtime and the kernel's HLE module: it
# checks what the kernel gives it and exits with the number of failed
# checks.
ppc_objects(_title_objs "${_src}/title/start.S" "${_src}/title/imports.S" "${_src}/title/title.c")
add_custom_command(OUTPUT "${_bin}/title.elf"
  COMMAND "${RECOMP_LD_LLD}" -static -T "${_src}/fixture/fixture.ld" -o "${_bin}/title.elf" ${_title_objs}
  DEPENDS ${_title_objs} "${_src}/fixture/fixture.ld"
  COMMENT "Linking the PowerPC runtime test title")
add_custom_command(OUTPUT "${_bin}/title.xex"
  COMMAND xbox360_elf2xex "${_bin}/title.elf" "${_bin}/title.xex" --import xboxkrnl.exe --import xam.xex
          --header 0x00030000=0x400 --header 0x00020401=0x123000
  DEPENDS xbox360_elf2xex "${_bin}/title.elf")
set(_title_gen "${CMAKE_CURRENT_BINARY_DIR}/title_generated")
add_custom_command(OUTPUT "${_title_gen}/funcs_000.cpp" "${_title_gen}/table.cpp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_title_gen}"
  COMMAND recomp_lift_ppc --image "${_bin}/title.xex" --out "${_title_gen}" --files 1
  DEPENDS recomp_lift_ppc "${_bin}/title.xex"
  COMMENT "Lifting the PowerPC runtime test title")
recomp_add_lifted_ppc_files(xbox360_title_code "${_title_gen}/funcs_000.cpp" "${_title_gen}/table.cpp"
                            LINK recomp_xbox360)
add_executable(xbox360_title title_main.cpp)
target_link_libraries(xbox360_title PRIVATE recomp_xbox360 recomp_xbox360_xboxkrnl recomp_xbox360_xam
                      xbox360_title_code)
recomp_bundle(xbox360_title)
# Its disc (the files title.c reads) and a data directory, emptied before
# each run.
set(_title_game "${CMAKE_CURRENT_BINARY_DIR}/title_game")
set(_title_data "${CMAKE_CURRENT_BINARY_DIR}/title_data")
file(WRITE "${_title_game}/data.bin" "0123456789ABCDEFGHIJKLMNOPQRSTUV")
file(WRITE "${_title_game}/Sub/Name.TXT" "hello")
file(WRITE "${_title_game}/Sub/other.dat" "x")
add_test(NAME xbox360_runtime_data COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_title_data}")
set_tests_properties(xbox360_runtime_data PROPERTIES FIXTURES_SETUP xbox360_runtime_data)
add_test(NAME xbox360_runtime COMMAND xbox360_title "${_bin}/title.xex" "${_title_game}" "${_title_data}")
set_tests_properties(xbox360_runtime PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|fatal" TIMEOUT 120
                     ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SDL_ASSERT=abort"
                     FIXTURES_REQUIRED xbox360_runtime_data)
# The title settings it wrote, saved in the profile's directory: the
# 1000-byte one, the one written again (10 bytes), the empty one.
add_test(NAME xbox360_runtime_profile COMMAND "${CMAKE_COMMAND}"
         -D "DIR=${_title_data}/profiles/E000000000000001/54540001" -P "${_src}/check_profile.cmake")
set_tests_properties(xbox360_runtime_profile PROPERTIES DEPENDS xbox360_runtime
                     FIXTURES_REQUIRED xbox360_runtime_data)

# --- the form oracle: every form's native stub, bare metal -------------------
ppc_objects(_forms_objs "${_src}/oracle/crt0.S" "${_src}/oracle/forms_main.c" "${_gen}/forms_native.S"
            "${_gen}/forms_native.c")
add_custom_command(OUTPUT "${_bin}/forms_oracle.elf"
  COMMAND "${RECOMP_LD_LLD}" -static -T "${_src}/oracle/forms.ld" -o "${_bin}/forms_oracle.elf" ${_forms_objs}
  DEPENDS ${_forms_objs} "${_src}/oracle/forms.ld"
  COMMENT "Linking the PowerPC form oracle")
add_custom_target(xbox360_oracle_programs ALL
  DEPENDS "${_bin}/vectors.elf" "${_bin}/fixture.elf" "${_bin}/forms_oracle.elf")

# xbox360_lift_forms_test, and the same built with each planted bug of
# ppc/lifted.h, ppc/vmx.h, ppc/fp.cpp and ppc/vmx.cpp (RECOMP_PPC_MUTATION),
# which the test must catch. Each builds its own fp.cpp and vmx.cpp, which
# take precedence over recomp_ppc's.
set(_mutations 1 2 3 4 5 6 7 8 9 10)
foreach(_m 0 ${_mutations})
  set(_t xbox360_lift_forms_test)
  if(_m)
    set(_t xbox360_lift_forms_mutation_${_m})
  endif()
  add_executable(${_t} lift_forms_test.cpp "${_gen}/forms.cpp" "${RECOMP_ROOT}/src/ppc/fp.cpp"
                 "${RECOMP_ROOT}/src/ppc/vmx.cpp")
  target_include_directories(${_t} PRIVATE "${_src}" "${_src}/oracle")
  target_compile_definitions(${_t} PRIVATE RECOMP_PPC_MUTATION=${_m})
  target_compile_options(${_t} PRIVATE -O1 -fno-strict-aliasing)
  target_link_libraries(${_t} PRIVATE recomp_ppc softfloat)
endforeach()

# xbox360_vmx128_test: VMX128's decoding and its operations without a VMX
# twin (the oracle has none of VMX128), with its own planted bugs.
set(_vmx128_mutations 11 12)
foreach(_m 0 ${_vmx128_mutations})
  set(_t xbox360_vmx128_test)
  if(_m)
    set(_t xbox360_vmx128_mutation_${_m})
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
  message(STATUS "xbox360: qemu-system-ppc64 not found; the PowerPC oracle tests are skipped")
  return()
endif()
set(RECOMP_PYTHON "${RECOMP_ROOT}/.venv/bin/python" CACHE FILEPATH
    "Python for the oracle tests' scripts")
if(NOT EXISTS "${RECOMP_PYTHON}")
  message(STATUS "xbox360: ${RECOMP_PYTHON} missing; the PowerPC oracle tests are skipped")
  return()
endif()
set(_run "${RECOMP_PYTHON}" "${_src}/oracle/qemu_run.py" --qemu "${RECOMP_QEMU_PPC64}"
    --vectors "${_bin}/vectors.elf")
set(_fixture_out "${CMAKE_CURRENT_BINARY_DIR}/fixture_oracle.bin")
set(_records "${CMAKE_CURRENT_BINARY_DIR}/forms_records.bin")

add_test(NAME xbox360_oracle_fixture COMMAND ${_run} --program "${_bin}/fixture.elf" --out "${_fixture_out}")
set_tests_properties(xbox360_oracle_fixture PROPERTIES FIXTURES_SETUP xbox360_fixture_out)
add_test(NAME xbox360_lift_fixture COMMAND xbox360_fixture "${_xex}" "${_fixture_out}")
set_tests_properties(xbox360_lift_fixture PROPERTIES FIXTURES_REQUIRED xbox360_fixture_out)

add_test(NAME xbox360_oracle_forms COMMAND ${_run} --program "${_bin}/forms_oracle.elf" --out "${_records}")
set_tests_properties(xbox360_oracle_forms PROPERTIES FIXTURES_SETUP xbox360_forms_records)
add_test(NAME xbox360_lift_forms COMMAND xbox360_lift_forms_test "${_records}")
set_tests_properties(xbox360_lift_forms PROPERTIES FIXTURES_REQUIRED xbox360_forms_records)
foreach(_m ${_mutations})
  add_test(NAME xbox360_lift_forms_mutation_${_m} COMMAND xbox360_lift_forms_mutation_${_m} "${_records}")
  set_tests_properties(xbox360_lift_forms_mutation_${_m} PROPERTIES
    FIXTURES_REQUIRED xbox360_forms_records WILL_FAIL TRUE)
endforeach()
