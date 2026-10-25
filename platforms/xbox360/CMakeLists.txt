# The Xbox 360 platform module (ARCHITECTURE.md, "Console platforms"): built
# only with RECOMP_PLATFORM_XBOX360, on the PowerPC lifter and CPU model
# (recomp_lift_ppc, recomp_ppc).

# XEX2 parsing and LZX decompression, for the lifter and the runtime.
add_library(recomp_xbox360_image SHARED src/xbox360/lzx.cpp src/xbox360/xex.cpp)
target_include_directories(recomp_xbox360_image PUBLIC src ${RECOMP_ROOT}/src)
target_compile_features(recomp_xbox360_image PUBLIC cxx_std_20)

# recomp_lift_ppc reads XEX2.
target_sources(recomp_lift_ppc PRIVATE lifter/xex_reader.cpp)
target_include_directories(recomp_lift_ppc PRIVATE ${RECOMP_ROOT}/tools/lifter_ppc)
target_link_libraries(recomp_lift_ppc PRIVATE recomp_xbox360_image)

# xbox360_xex_decrypt: an encrypted XEX's unencrypted copy, with the owner's
# key (tools/xex_decrypt.cpp). OpenSSL is a host library, linked dynamically.
find_package(OpenSSL COMPONENTS Crypto)
if(OpenSSL_FOUND)
  add_executable(xbox360_xex_decrypt tools/xex_decrypt.cpp)
  target_link_libraries(xbox360_xex_decrypt PRIVATE OpenSSL::Crypto)
else()
  message(STATUS "xbox360: OpenSSL not found; xbox360_xex_decrypt is not built")
endif()

# xbox360_elf2xex: builds the test fixtures' XEX from their ELF links.
add_executable(xbox360_elf2xex tools/elf2xex.cpp)

add_subdirectory(tests)
