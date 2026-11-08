# The AArch64 core's test fixture (tests/a64) as a Switch homebrew NRO,
# with the NRO crt0 here (fixture/crt0.S): lifted through the NRO reader
# and run on this module's loader and supervisor calls, its output must
# match the Unicorn oracle's run of the core's fixture.

if(NOT COMMAND recomp_a64_fixture)
  message(STATUS "horizon: the AArch64 core's fixture is not built; the NRO test is skipped")
  return()
endif()

recomp_a64_fixture(fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture/crt0.S" _elf)
set(_nro "${CMAKE_CURRENT_BINARY_DIR}/fixture.nro")
add_custom_command(OUTPUT "${_nro}"
  COMMAND horizon_elf2nro "${_elf}" "${_nro}"
  DEPENDS horizon_elf2nro "${_elf}")

set(_gen "${CMAKE_CURRENT_BINARY_DIR}/fixture_generated")
set(_funcs)
foreach(_k 000 001 002 003)
  list(APPEND _funcs "${_gen}/funcs_${_k}.cpp")
endforeach()
add_custom_command(OUTPUT ${_funcs} "${_gen}/table.cpp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
  COMMAND recomp_lift_a64 --image "${_nro}" --out "${_gen}" --files 4
  DEPENDS recomp_lift_a64 "${_nro}"
  COMMENT "Lifting the fixture NRO")

recomp_add_lifted_a64_files(horizon_fixture_code ${_funcs} "${_gen}/table.cpp")
add_executable(horizon_fixture fixture_main.cpp $<TARGET_OBJECTS:horizon_fixture_code>)
target_link_libraries(horizon_fixture PRIVATE recomp_horizon)

if(NOT RECOMP_A64_ORACLE)
  return()
endif()
add_test(NAME horizon_fixture
  COMMAND "${RECOMP_PYTHON}" "${RECOMP_A64_ORACLE}" run --elf "${RECOMP_A64_FIXTURE_ELF}"
          --image "${_nro}" --lifted ${CMAKE_CROSSCOMPILING_EMULATOR}
          $<TARGET_FILE:horizon_fixture>)
