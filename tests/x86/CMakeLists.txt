# Dev-only tests of the x86 core; nothing here ships. The lifting oracle
# tests run on the fixture below; the game projects add them on their EXEs
# too (recomp_add_lift_tests).

# x87_test runs the host CPU's own x87 (inline asm) next to the model from
# the same state and compares the results, so it can only exist on x86-64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(x87_test x87_test.cpp)
  target_link_libraries(x87_test PRIVATE recomp_x87)
  add_test(NAME x87 COMMAND x87_test)
  set_tests_properties(x87 PROPERTIES TIMEOUT 300)
endif()

# x87_fast_test checks the model's integer fast paths against its own
# SoftFloat paths; portable.
add_executable(x87_fast_test x87_fast_test.cpp)
target_link_libraries(x87_fast_test PRIVATE recomp_x87)
target_compile_options(x87_fast_test PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
add_test(NAME x87_fast COMMAND x87_fast_test)
set_tests_properties(x87_fast PROPERTIES TIMEOUT 300)
# The same built with each planted bug of the fast paths
# (RECOMP_X86_MUTATION, x86/x87.h), which it must catch (it exits 0 only
# then); each builds its own x87.cpp.
foreach(_m RANGE 1 17)
  add_executable(x87_fast_mutation_${_m} x87_fast_test.cpp "${RECOMP_ROOT}/src/x86/x87.cpp")
  target_include_directories(x87_fast_mutation_${_m} PRIVATE "${RECOMP_ROOT}/src")
  target_compile_features(x87_fast_mutation_${_m} PRIVATE cxx_std_20)
  target_compile_definitions(x87_fast_mutation_${_m} PRIVATE RECOMP_X86_MUTATION=${_m})
  target_compile_options(x87_fast_mutation_${_m} PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
  target_link_libraries(x87_fast_mutation_${_m} PRIVATE softfloat recomp_base)
  add_test(NAME x87_fast_mutation_${_m} COMMAND x87_fast_mutation_${_m} 200000)
endforeach()

# sse_rsqrt_test: RSQRTPS is the correctly rounded 1/sqrt(x), checked
# exhaustively with exact arithmetic (the host oracle cannot: CPUs differ);
# portable.
add_executable(sse_rsqrt_test sse_rsqrt_test.cpp)
target_link_libraries(sse_rsqrt_test PRIVATE recomp_x87)
target_compile_options(sse_rsqrt_test PRIVATE -O2)
add_test(NAME sse_rsqrt COMMAND sse_rsqrt_test)
set_tests_properties(sse_rsqrt PROPERTIES TIMEOUT 300)

# sse_ftz_test: FTZ and DAZ against the host (x86-64 hosts only).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(sse_ftz_test sse_ftz_test.cpp)
  target_include_directories(sse_ftz_test PRIVATE "${RECOMP_ROOT}/src")
  target_link_libraries(sse_ftz_test PRIVATE recomp_x87)
  target_compile_options(sse_ftz_test PRIVATE -O2)
  add_test(NAME sse_ftz COMMAND sse_ftz_test)
endif()

# hle_abi_test: HLE argument order under the Win64 and System V ABIs.
add_executable(hle_abi_test hle_abi_test.cpp)
target_link_libraries(hle_abi_test PRIVATE recomp_x86)
add_test(NAME hle_abi COMMAND hle_abi_test)

# hle_dup_test: a duplicate HLE registration is fatal (its message passes).
add_executable(hle_dup_test hle_dup_test.cpp)
target_link_libraries(hle_dup_test PRIVATE recomp_x86)
add_test(NAME hle_dup COMMAND hle_dup_test)
set_tests_properties(hle_dup PROPERTIES PASS_REGULAR_EXPRESSION "test.dll!Twice is registered twice")

# runtime_thread_test: guest faults, thread exits and the watchdog (Linux).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(runtime_thread_test runtime_thread_test.cpp)
  target_include_directories(runtime_thread_test PRIVATE ${RECOMP_ROOT}/src)
  target_link_libraries(runtime_thread_test PRIVATE recomp_x86)
  target_compile_options(runtime_thread_test PRIVATE -fnon-call-exceptions)
  add_test(NAME runtime_thread COMMAND runtime_thread_test)
endif()

# fxsave_test: FXSAVE/FXRSTOR of the x87 and SSE models against the host.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(fxsave_test fxsave_test.cpp)
  target_link_libraries(fxsave_test PRIVATE recomp_x87)
  add_test(NAME fxsave COMMAND fxsave_test)
endif()

# lift_semantics_test checks the integer/flag helpers in lifted.h against
# the host CPU the same way.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(lift_semantics_test lift_semantics_test.cpp)
  target_include_directories(lift_semantics_test PRIVATE ${RECOMP_ROOT}/src)
  target_compile_features(lift_semantics_test PRIVATE cxx_std_20)
  target_compile_options(lift_semantics_test PRIVATE -O1)
  add_test(NAME lift_semantics COMMAND lift_semantics_test)
  set_tests_properties(lift_semantics PROPERTIES TIMEOUT 300)
endif()

# lift_isa64_int, lift_isa64_sse: lift_x64_test.cpp on the forms in
# x64_isa/forms.txt (recomp_lift --forms): the x64 CPU model's extensions
# beyond what the x64 test images cover. They run natively too, so the
# host needs those extensions (BMI1, ABM, MOVBE, SSE4.2, AES, PCLMULQDQ,
# F16C, AVX), as every AMD family 16h or later and Intel Haswell or later
# has.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND TARGET recomp_lift)
  foreach(_kind int sse)
    set(_forms "${CMAKE_CURRENT_BINARY_DIR}/lift_isa64_${_kind}_forms.cpp")
    add_custom_command(OUTPUT "${_forms}"
      COMMAND recomp_lift --forms "${CMAKE_CURRENT_SOURCE_DIR}/x64_isa/forms.txt" --x64
              --${_kind}-test "${_forms}"
      DEPENDS recomp_lift "${CMAKE_CURRENT_SOURCE_DIR}/x64_isa/forms.txt"
      COMMENT "Lifting the x64 extension forms for lift_isa64_${_kind}_test")
    add_executable(lift_isa64_${_kind}_test lift_x64_test.cpp "${_forms}")
    target_include_directories(lift_isa64_${_kind}_test PRIVATE ${RECOMP_ROOT}/src
                               ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lift_isa64_${_kind}_test PRIVATE recomp_x87 recomp_base)
    target_compile_options(lift_isa64_${_kind}_test PRIVATE -O1 -fno-strict-aliasing
                           ${RECOMP_X87_FP_FLAGS})
    if(_kind STREQUAL "sse")
      target_compile_definitions(lift_isa64_${_kind}_test PRIVATE LIFT_X64_SSE)
    endif()
    add_test(NAME lift_isa64_${_kind} COMMAND lift_isa64_${_kind}_test)
    set_tests_properties(lift_isa64_${_kind} PROPERTIES TIMEOUT 1800)
  endforeach()
endif()

# lift_fixture_int, lift_fixture_sse, lift_fixture_x87: lift_int_test.cpp,
# lift_sse_test.cpp and lift_x87_test.cpp on the forms of fixture/forms.s,
# assembled by clang for i686 and linked by LLD to flat code, which
# recomp_lift --code takes its forms from. Without those tools
# x86_prerequisites fails, naming them, unless
# RECOMP_ALLOW_MISSING_TEST_TOOLS says this host cannot run these tests.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND TARGET recomp_lift)
  option(RECOMP_ALLOW_MISSING_TEST_TOOLS "Leave out the tests whose tools are missing instead of failing" OFF)
  find_program(RECOMP_LD_LLD ld.lld)
  set(_missing)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND _missing "clang as the C compiler")
  endif()
  if(NOT RECOMP_LD_LLD)
    list(APPEND _missing ld.lld)
  endif()
  if(_missing)
    string(JOIN ", " _missing ${_missing})
    message(STATUS "x86: missing ${_missing}; the lifter fixture's tests are left out")
    if(NOT RECOMP_ALLOW_MISSING_TEST_TOOLS)
      add_test(NAME x86_prerequisites COMMAND "${CMAKE_COMMAND}" -E echo
               "missing ${_missing} (RECOMP_ALLOW_MISSING_TEST_TOOLS=ON leaves their tests out)")
      set_tests_properties(x86_prerequisites PROPERTIES FAIL_REGULAR_EXPRESSION .)
    endif()
  else()
    set(_fix "${CMAKE_CURRENT_BINARY_DIR}/fixture")
    add_custom_command(OUTPUT "${_fix}/forms.bin"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_fix}"
      COMMAND "${CMAKE_C_COMPILER}" --target=i686-unknown-linux-gnu -c
              "${CMAKE_CURRENT_SOURCE_DIR}/fixture/forms.s" -o "${_fix}/forms.o"
      COMMAND "${RECOMP_LD_LLD}" --oformat=binary -o "${_fix}/forms.bin" "${_fix}/forms.o"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixture/forms.s"
      COMMENT "Assembling the x86 lifter fixture")
    foreach(_kind x87 int sse)
      set(_forms "${_fix}/lift_${_kind}_forms.cpp")
      add_custom_command(OUTPUT "${_forms}"
        COMMAND recomp_lift --code "${_fix}/forms.bin" --${_kind}-test "${_forms}"
        DEPENDS recomp_lift "${_fix}/forms.bin"
        COMMENT "Lifting the fixture's ${_kind} instruction forms for lift_fixture_${_kind}_test")
      add_executable(lift_fixture_${_kind}_test lift_${_kind}_test.cpp "${_forms}")
      target_include_directories(lift_fixture_${_kind}_test PRIVATE ${RECOMP_ROOT}/src
                                 ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(lift_fixture_${_kind}_test PRIVATE recomp_x87 recomp_base)
      target_compile_options(lift_fixture_${_kind}_test PRIVATE -O1 -fno-strict-aliasing
                             ${RECOMP_X87_FP_FLAGS})
      add_test(NAME lift_fixture_${_kind} COMMAND lift_fixture_${_kind}_test)
      set_tests_properties(lift_fixture_${_kind} PROPERTIES TIMEOUT 900)
    endforeach()
  endif()
endif()
