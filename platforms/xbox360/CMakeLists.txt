# The Xbox 360 platform module (ARCHITECTURE.md, "Console platforms"): built
# only with RECOMP_PLATFORM_XBOX360, on the PowerPC lifter and CPU model
# (recomp_lift_ppc, recomp_ppc).

if(NOT RECOMP_HOST)
  message(FATAL_ERROR "RECOMP_PLATFORM_XBOX360 needs RECOMP_HOST (audio output)")
endif()

# XEX2 parsing and LZX decompression, for the lifter and the runtime.
add_library(recomp_xbox360_image SHARED src/xbox360/lzx.cpp src/xbox360/xex.cpp)
target_include_directories(recomp_xbox360_image PUBLIC src ${RECOMP_ROOT}/src)
target_compile_features(recomp_xbox360_image PUBLIC cxx_std_20)
recomp_bundle(recomp_xbox360_image)

# The runtime (ARCHITECTURE.md, "Runtime"): guest memory, the HLE registry
# and Import(), threads, dispatcher objects, handles and time, the file
# namespace, the title's module, the console's settings, the GPU, audio
# output (SDL3, from the host layer), process bring-up (xbox360/process.h). Lifted modules link it for what lifted
# code calls (recomp_add_lifted_ppc ... LINK recomp_xbox360).
find_package(Threads REQUIRED)
add_library(recomp_xbox360 SHARED
  src/xbox360/audio.cpp
  src/xbox360/config.cpp
  src/xbox360/cpu.cpp
  src/xbox360/format.cpp
  src/xbox360/fs.cpp
  src/xbox360/gpu.cpp
  src/xbox360/hle.cpp
  src/xbox360/memory.cpp
  src/xbox360/module.cpp
  src/xbox360/objects.cpp
  src/xbox360/process.cpp
  src/xbox360/thread.cpp)
target_link_libraries(recomp_xbox360 PUBLIC recomp_ppc recomp_xbox360_image Threads::Threads
                      PRIVATE SDL3::SDL3 recomp_log)
recomp_bundle(recomp_xbox360)

# The HLE modules, one shared library per system module, found by nothing
# but their registrars (which run when the library loads): programs link
# them --no-as-needed, so a linker that drops unreferenced libraries keeps
# them.
add_library(recomp_xbox360_xboxkrnl SHARED
  src/xbox360/xboxkrnl/audio.cpp
  src/xbox360/xboxkrnl/io.cpp
  src/xbox360/xboxkrnl/memory.cpp
  src/xbox360/xboxkrnl/modules.cpp
  src/xbox360/xboxkrnl/rtl.cpp
  src/xbox360/xboxkrnl/sync.cpp
  src/xbox360/xboxkrnl/threads.cpp
  src/xbox360/xboxkrnl/variables.cpp
  src/xbox360/xboxkrnl/video.cpp
  src/xbox360/xboxkrnl/xconfig.cpp)
add_library(recomp_xbox360_xam SHARED
  src/xbox360/xam/info.cpp
  src/xbox360/xam/loader.cpp
  src/xbox360/xam/memory.cpp
  src/xbox360/xam/msg.cpp
  src/xbox360/xam/net.cpp
  src/xbox360/xam/notify.cpp
  src/xbox360/xam/profile.cpp
  src/xbox360/xam/user.cpp
  src/xbox360/xam/xmp.cpp)
foreach(_m xboxkrnl xam)
  target_link_libraries(recomp_xbox360_${_m} PUBLIC recomp_xbox360)
  if(NOT APPLE)
    target_link_options(recomp_xbox360_${_m} INTERFACE "LINKER:--no-as-needed")
  endif()
  recomp_bundle(recomp_xbox360_${_m})
endforeach()

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
  recomp_bundle(xbox360_xex_decrypt)
else()
  message(STATUS "xbox360: OpenSSL not found; xbox360_xex_decrypt is not built")
endif()

# xbox360_elf2xex: builds the test fixtures' XEX from their ELF links.
add_executable(xbox360_elf2xex tools/elf2xex.cpp)
recomp_bundle(xbox360_elf2xex)

add_subdirectory(tests)
