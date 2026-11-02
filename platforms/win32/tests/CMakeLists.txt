# Dev-only tests of the Win32 platform; nothing here ships.

# win32_text_test: UTF-8 <-> UTF-16 conversion (win32/win32.h);
# portable.
add_executable(win32_text_test win32_text_test.cpp)
target_include_directories(win32_text_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(win32_text_test PRIVATE cxx_std_20)
add_test(NAME win32_text COMMAND win32_text_test)
