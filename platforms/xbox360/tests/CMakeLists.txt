# The runtime test title (title/): C and assembly built like the PowerPC
# core's test programs (tests/ppc, whose ppc_objects compiles them) and
# turned into a XEX by xbox360_elf2xex.
if(NOT COMMAND ppc_objects)
  return()  # the core's tests say why its test programs are not built
endif()

set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin "${CMAKE_CURRENT_BINARY_DIR}/ppc")

# --- the runtime test title ------------------------------------------------------
# A title that calls the kernel through imports (title/), lifted into a
# lifted module and run on the runtime and the kernel's HLE module: it
# checks what the kernel gives it and exits with the number of failed
# checks.
ppc_objects(_title_objs "${_src}/title/start.S" "${_src}/title/imports.S" "${_src}/title/title.c")
add_custom_command(OUTPUT "${_bin}/title.elf"
  COMMAND "${RECOMP_LD_LLD}" -static -T "${RECOMP_ROOT}/tests/ppc/fixture/fixture.ld" -o "${_bin}/title.elf" ${_title_objs}
  DEPENDS ${_title_objs} "${RECOMP_ROOT}/tests/ppc/fixture/fixture.ld"
  COMMENT "Linking the PowerPC runtime test title")
add_custom_command(OUTPUT "${_bin}/title.xex"
  COMMAND xbox360_elf2xex "${_bin}/title.elf" "${_bin}/title.xex" --import xboxkrnl.exe
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
target_link_libraries(xbox360_title PRIVATE recomp_xbox360 recomp_xbox360_xboxkrnl xbox360_title_code)
recomp_bundle(xbox360_title)
add_test(NAME xbox360_runtime COMMAND xbox360_title "${_bin}/title.xex")
set_tests_properties(xbox360_runtime PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|fatal" TIMEOUT 120)
