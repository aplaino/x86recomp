# Dev-only tests of the x86 core; nothing here ships. The EXE-derived
# oracle tests are added by the game project (recomp_add_lift_tests).

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

# sse_rsqrt_test: RSQRTPS is the correctly rounded 1/sqrt(x), checked
# exhaustively with exact arithmetic (the host oracle cannot: CPUs differ);
# portable.
add_executable(sse_rsqrt_test sse_rsqrt_test.cpp)
target_link_libraries(sse_rsqrt_test PRIVATE recomp_x87)
target_compile_options(sse_rsqrt_test PRIVATE -O2)
add_test(NAME sse_rsqrt COMMAND sse_rsqrt_test)
set_tests_properties(sse_rsqrt PROPERTIES TIMEOUT 300)

# hle_abi_test: HLE argument order under the Win64 and System V ABIs.
add_executable(hle_abi_test hle_abi_test.cpp)
target_link_libraries(hle_abi_test PRIVATE recomp_x86)
add_test(NAME hle_abi COMMAND hle_abi_test)

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
