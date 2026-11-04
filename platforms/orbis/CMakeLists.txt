# The PS4 (Orbis OS) platform module (ARCHITECTURE.md here): built only
# with RECOMP_PLATFORM_ORBIS.

# A program is a directory of shared libraries beside a small executable
# (ARCHITECTURE.md, "Programs and libraries"): the lifted modules, the
# loader, each system library's HLE, the GPU, the x86 core and the host
# layer under them, each replaced alone. A library's change relinks
# nothing that links it.
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)
recomp_bundle(recomp_base recomp_x87 recomp_x86 recomp_log recomp_gfx recomp_hlsl
              recomp_audio recomp_ui)

# Orbis ELF mapping, NIDs and the application directory, for the lifter
# and the runtime.
add_library(recomp_orbis_image SHARED
  src/orbis/elf_image.cpp
  src/orbis/nid.cpp
  src/orbis/app.cpp)
target_include_directories(recomp_orbis_image PUBLIC src)
target_link_libraries(recomp_orbis_image PUBLIC recomp_base)

# The process loader and games' main() (orbis/process.h) and the HLE
# registration by NID (orbis/hle.h); the system libraries' HLE builds on
# it.
add_library(recomp_orbis SHARED src/orbis/process.cpp src/orbis/launcher.cpp src/orbis/host.cpp
            src/orbis/fios2.cpp)
# Fixes to middleware (lifter/middleware/*.manifest `host` lines) run as
# lifted code does: guest faults may unwind out of them.
set_source_files_properties(src/orbis/fios2.cpp PROPERTIES
                            COMPILE_OPTIONS "-fno-strict-aliasing;-fnon-call-exceptions")
target_link_libraries(recomp_orbis PUBLIC recomp_x86 recomp_orbis_image)

# System library HLE, one shared library each.
add_library(recomp_orbis_kernel SHARED src/orbis/hle/kernel.cpp src/orbis/hle/kernel_memory.cpp
            src/orbis/hle/kernel_pthread.cpp src/orbis/hle/kernel_sync.cpp
            src/orbis/hle/kernel_time.cpp src/orbis/hle/kernel_file.cpp
            src/orbis/hle/kernel_equeue.cpp)
target_link_libraries(recomp_orbis_kernel PUBLIC recomp_orbis)
add_library(recomp_orbis_libc_internal SHARED src/orbis/hle/libc_internal.cpp)
target_link_libraries(recomp_orbis_libc_internal PUBLIC recomp_orbis_kernel)
# libSceSystemService, libSceUserService, libSceSysmodule, libScePlayGo, libSceDiscMap, the dialogs.
add_library(recomp_orbis_system SHARED src/orbis/hle/system.cpp src/orbis/hle/playgo.cpp
            src/orbis/hle/discmap.cpp src/orbis/hle/dialog.cpp)
target_link_libraries(recomp_orbis_system PUBLIC recomp_orbis)
add_library(recomp_orbis_fiber SHARED src/orbis/hle/fiber.cpp)
target_link_libraries(recomp_orbis_fiber PUBLIC recomp_orbis)
# libSceAudioOut (and the audio libraries on it).
add_library(recomp_orbis_audio SHARED src/orbis/hle/audioout.cpp)
target_link_libraries(recomp_orbis_audio PUBLIC recomp_orbis recomp_audio)
# libSceNgs2: renders into the game's buffers, which it gives libSceAudioOut;
# ATRAC9 through ffmpeg.
add_library(recomp_orbis_ngs2 SHARED src/orbis/hle/ngs2.cpp src/orbis/hle/ngs2_dsp.cpp)
target_link_libraries(recomp_orbis_ngs2 PUBLIC recomp_orbis PRIVATE PkgConfig::FFMPEG)
add_library(recomp_orbis_savedata SHARED src/orbis/hle/savedata.cpp)
target_link_libraries(recomp_orbis_savedata PUBLIC recomp_orbis_kernel)
# libScePad on SDL gamepads and the keyboard.
add_library(recomp_orbis_pad SHARED src/orbis/hle/pad.cpp)
target_link_libraries(recomp_orbis_pad PUBLIC recomp_orbis)
# libSceFont and libSceFontFt on the host layer's FreeType.
add_library(recomp_orbis_font SHARED src/orbis/hle/font.cpp)
target_link_libraries(recomp_orbis_font PUBLIC recomp_orbis PRIVATE Freetype::Freetype)
# libSceAvPlayer on ffmpeg.
add_library(recomp_orbis_avplayer SHARED src/orbis/hle/avplayer.cpp)
target_link_libraries(recomp_orbis_avplayer PUBLIC recomp_orbis PRIVATE PkgConfig::FFMPEG)
# Networking (libSceNet, libSceNetCtl): no connection.
add_library(recomp_orbis_net SHARED src/orbis/hle/net.cpp)
target_link_libraries(recomp_orbis_net PUBLIC recomp_orbis_kernel)
# PSN (libSceNp*): a user not signed in.
add_library(recomp_orbis_np SHARED src/orbis/hle/np_manager.cpp src/orbis/hle/np_webapi.cpp
            src/orbis/hle/np_trophy.cpp)
target_link_libraries(recomp_orbis_np PUBLIC recomp_orbis)
add_library(recomp_orbis_videoout SHARED src/orbis/hle/videoout.cpp)
target_link_libraries(recomp_orbis_videoout PUBLIC recomp_orbis_kernel recomp_orbis_gpu recomp_ui)
# The GPU (orbis/gpu): the command processor, beside the CPU, and its
# shaders' instructions.
add_library(recomp_orbis_gpu SHARED src/orbis/gpu/cp.cpp src/orbis/gpu/gcn.cpp
            src/orbis/gpu/shader.cpp src/orbis/gpu/renderer.cpp
            src/orbis/gpu/tiling.cpp)
target_link_libraries(recomp_orbis_gpu PUBLIC recomp_orbis_kernel recomp_gfx)
# libSceGnmDriver, which submits to the GPU.
add_library(recomp_orbis_gnm SHARED src/orbis/hle/gnm_driver.cpp)
target_link_libraries(recomp_orbis_gnm PUBLIC recomp_orbis_videoout recomp_orbis_gpu)
# libSceSaveDataDialog: the system's dialog, drawn over the game's frames.
add_library(recomp_orbis_savedata_dialog SHARED src/orbis/hle/savedata_dialog.cpp)
target_link_libraries(recomp_orbis_savedata_dialog PUBLIC recomp_orbis_system
  recomp_orbis_savedata recomp_orbis_pad recomp_orbis_videoout recomp_ui)
recomp_bundle(recomp_orbis_image recomp_orbis recomp_orbis_kernel recomp_orbis_libc_internal
              recomp_orbis_system recomp_orbis_fiber recomp_orbis_audio recomp_orbis_ngs2
              recomp_orbis_savedata recomp_orbis_pad recomp_orbis_font recomp_orbis_avplayer
              recomp_orbis_net recomp_orbis_np recomp_orbis_videoout recomp_orbis_gpu
              recomp_orbis_gnm recomp_orbis_savedata_dialog)

# Every system library's HLE, for a game's executable to link: imports
# bind by name, so a game links them all.
add_library(recomp_orbis_hle INTERFACE)
target_link_libraries(recomp_orbis_hle INTERFACE recomp_orbis recomp_orbis_kernel
  recomp_orbis_libc_internal recomp_orbis_system recomp_orbis_fiber recomp_orbis_audio
  recomp_orbis_ngs2 recomp_orbis_savedata recomp_orbis_pad recomp_orbis_font
  recomp_orbis_avplayer recomp_orbis_net recomp_orbis_np recomp_orbis_videoout
  recomp_orbis_gnm recomp_orbis_savedata_dialog)

# orbis_app (tools/orbis_app.cpp): the modules a game starts with, for
# tools/lift.sh, and how the HLE covers their imports.
add_executable(orbis_app tools/orbis_app.cpp)
target_link_libraries(orbis_app PRIVATE recomp_orbis_hle)
recomp_bundle(orbis_app)

# recomp_lift reads Orbis ELFs.
target_sources(recomp_lift PRIVATE lifter/orbis_reader.cpp)
target_include_directories(recomp_lift PRIVATE ${RECOMP_ROOT}/tools/lifter_x86)
target_link_libraries(recomp_lift PRIVATE recomp_orbis_image)

add_executable(orbis_image_test tests/orbis_image_test.cpp)
target_link_libraries(orbis_image_test PRIVATE recomp_orbis_image)
recomp_bundle(orbis_image_test)
add_test(NAME orbis_image COMMAND orbis_image_test)

add_executable(orbis_gcn_test tests/orbis_gcn_test.cpp)
target_link_libraries(orbis_gcn_test PRIVATE recomp_orbis_gpu)
recomp_bundle(orbis_gcn_test)
add_test(NAME orbis_gcn COMMAND orbis_gcn_test)

add_executable(orbis_tiling_test tests/orbis_tiling_test.cpp)
target_link_libraries(orbis_tiling_test PRIVATE recomp_orbis_gpu)
recomp_bundle(orbis_tiling_test)
add_test(NAME orbis_tiling COMMAND orbis_tiling_test)

# orbis_gpu_test: dispatches on SDL_GPU; needs a Vulkan device (lavapipe
# will do), skipped without one.
add_executable(orbis_gpu_test tests/orbis_gpu_test.cpp)
target_link_libraries(orbis_gpu_test PRIVATE recomp_orbis_gpu recomp_gfx)
recomp_bundle(orbis_gpu_test)
add_test(NAME orbis_gpu COMMAND orbis_gpu_test)
set_tests_properties(orbis_gpu PROPERTIES SKIP_RETURN_CODE 77
                     ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_GPU_DRIVER=vulkan")

# orbis_process_test: tests/e2e (C for FreeBSD x86-64, linked by ld.lld
# and made SCE ELFs) lifted at build time and run through the loader.
# Needs clang and ld.lld.
find_program(RECOMP_LD_LLD ld.lld HINTS "${RECOMP_LLVM_ROOT}/bin")
find_package(Python3 COMPONENTS Interpreter)
if(RECOMP_LD_LLD AND Python3_FOUND AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(_e2e "${CMAKE_CURRENT_BINARY_DIR}/e2e")
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e")
  add_custom_command(OUTPUT "${_e2e}/eboot.elf" "${_e2e}/prx.elf" "${_e2e}/load.elf"
                            "${_e2e}/app0/eboot.bin" "${_e2e}/app0/sce_module/libTestPrx.prx"
                            "${_e2e}/app0/sce_module/libTestLoad.prx"
    COMMAND Python3::Interpreter "${_src}/build.py" "${_e2e}" "${CMAKE_C_COMPILER}"
            "${RECOMP_LD_LLD}"
    DEPENDS "${_src}/build.py" "${_src}/eboot.c" "${_src}/kernel.c" "${_src}/system.c"
            "${_src}/prx.c" "${_src}/load.c" "${_src}/stubs.c"
            "${RECOMP_ROOT}/tools/orbis_sce_elf.py"
    VERBATIM)
  set(_gen "${_e2e}/gen/gen_000.cpp" "${_e2e}/gen/gen_prx_000.cpp" "${_e2e}/gen/gen_load_000.cpp"
           "${_e2e}/gen/table.cpp")
  add_custom_command(OUTPUT ${_gen}
    COMMAND recomp_lift --exe "${_e2e}/eboot.elf" --symbols "" --manifest ""
            --module "${_e2e}/prx.elf" --symbols "" --base 800000000
            --module "${_e2e}/load.elf" --symbols "" --base 810000000 --out "${_e2e}/gen"
    COMMAND ${CMAKE_COMMAND} -E touch ${_gen}
    DEPENDS recomp_lift "${_e2e}/eboot.elf" "${_e2e}/prx.elf" "${_e2e}/load.elf"
    VERBATIM)
  recomp_add_lifted_files(orbis_process_lifted ${_gen} LINK recomp_orbis)
  add_executable(orbis_process_test tests/orbis_process_test.cpp)
  target_link_libraries(orbis_process_test PRIVATE orbis_process_lifted recomp_orbis
                        recomp_orbis_kernel recomp_orbis_fiber recomp_orbis_gnm
                        recomp_orbis_savedata recomp_orbis_audio recomp_orbis_pad recomp_orbis_np
                        recomp_orbis_ngs2 recomp_orbis_savedata_dialog)
  recomp_bundle(orbis_process_test)
  add_test(NAME orbis_process COMMAND orbis_process_test "${_e2e}/eboot.elf" "${_e2e}/prx.elf"
           "${_e2e}/app0/sce_module/libTestLoad.prx")
  add_dependencies(orbis_process_test orbis_e2e_app0)
  set_tests_properties(orbis_process PROPERTIES TIMEOUT 60)  # a hang is a failure
  # orbis_app on the same programs laid out as an app0: the PRXs found by
  # their module names, the one the eboot needs at the first PRX base, the
  # one it loads itself next; the imports bound to the first, the
  # test-only library's left missing, libkernel's handled.
  add_custom_target(orbis_e2e_app0 ALL DEPENDS "${_e2e}/app0/eboot.bin"
                    "${_e2e}/app0/sce_module/libTestPrx.prx"
                    "${_e2e}/app0/sce_module/libTestLoad.prx")
  add_test(NAME orbis_app_modules COMMAND orbis_app modules "${_e2e}/app0")
  set_tests_properties(orbis_app_modules PROPERTIES PASS_REGULAR_EXPRESSION
    "^0\teboot\t[^\n]*/eboot.bin\tstart\n800000000\tlibTestPrx\t[^\n]*/libTestPrx.prx\tstart\n810000000\tlibTestLoad\t[^\n]*/libTestLoad.prx\tload\n$")
  add_test(NAME orbis_app_imports COMMAND orbis_app imports "${_e2e}/app0")
  set_tests_properties(orbis_app_imports PROPERTIES PASS_REGULAR_EXPRESSION
    "\nlibTest +[0-9]+ +0 +0 +[1-9][0-9]*\nlibTestPrx +[0-9]+ +[1-9][0-9]* +0 +0\n(.*\n)?libkernel +[0-9]+ +0 +[1-9][0-9]* +0\n")
endif()
