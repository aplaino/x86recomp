# The Win32 platform module (ARCHITECTURE.md, "Layout"): Windows PC games
# on the x86 core. Built with RECOMP_PLATFORM_WIN32 (the default); its
# rules are in CLAUDE.md here.

# CORE-MATH, behind the x64 CRT's math imports (recomp_ucrt).
include(${RECOMP_ROOT}/cmake/coremath.cmake)  # defines target `coremath`

# The Win32 process (PE image, PEB, TEBs, TLS, SEH) and the core of the
# Win32 HLE: kernel32, user32, gdi32, advapi32, winmm, ws2_32, shell and
# COM, and the window and event pump every graphics API presents through.
# HLE handlers register themselves from static initialisers, which run
# when their library is loaded.
add_library(recomp_win32 SHARED
  src/win32/process.cpp
  src/win32/hle.cpp
  src/win32/kernel32.cpp
  src/win32/winmm_timer.cpp
  src/win32/kernel32_file.cpp
  src/win32/kernel32_sync.cpp
  src/win32/kernel32_seh.cpp
  src/win32/seh64.cpp
  src/win32/kernel32_misc.cpp
  src/win32/advapi32.cpp
  src/win32/user32_input.cpp
  src/win32/user32_window.cpp
  src/win32/gdi32.cpp
  src/win32/shell_net.cpp
  src/win32/ws2_32.cpp
  src/win32/com.cpp)
target_include_directories(recomp_win32 PUBLIC src)
target_link_libraries(recomp_win32 PUBLIC recomp_x86 recomp_pe SDL3::SDL3
                      PRIVATE recomp_gfx)  # user32: gfx::FitRect, x86/present.h
# Nothing in a game refers to an HLE module's symbols: keep the linker
# from dropping them where it links --as-needed (ld64 keeps every dylib).
if(NOT APPLE)
  target_link_options(recomp_win32 INTERFACE "LINKER:--no-as-needed")
endif()

if(FFMPEG_FOUND)
  # Miles Sound System (mss32) on the software mixer.
  add_library(recomp_mss32 SHARED src/win32/mss32.cpp)
  target_link_libraries(recomp_mss32 PUBLIC recomp_win32 recomp_audio)

  # DirectSound 8 (dsound) on the same software mixer.
  add_library(recomp_dsound SHARED src/win32/dsound.cpp)
  target_link_libraries(recomp_dsound PUBLIC recomp_win32 recomp_audio)

  # Bink video (binkw32) on ffmpeg; audio through the Miles mixer.
  add_library(recomp_bink SHARED src/win32/binkw32.cpp)
  target_link_libraries(recomp_bink PUBLIC recomp_mss32)
endif()



# DirectDraw 7 / Direct3D 7 over gfx::Device.
add_library(recomp_d3d7 SHARED
  src/win32/ddraw.cpp
  src/win32/d3d7.cpp)
target_link_libraries(recomp_d3d7 PUBLIC recomp_win32 recomp_gfx)

# XInput 1.3 on SDL gamepads, and the WMI that tells games which
# DirectInput game controllers are XInput pads.
add_library(recomp_xinput SHARED src/win32/xinput.cpp
            src/win32/wbem.cpp)
target_link_libraries(recomp_xinput PUBLIC recomp_win32)

# DirectInput 8 (keyboard and mouse) on the event pump's raw input; it
# lists the XInput pads as game controllers.
add_library(recomp_dinput8 SHARED src/win32/dinput8.cpp)
target_link_libraries(recomp_dinput8 PUBLIC recomp_win32 recomp_xinput)

# Direct3D 9 and D3DX 9 over gfx::Device.
add_library(recomp_d3d9 SHARED src/win32/d3d9.cpp
  src/win32/d3d9_draw.cpp src/win32/d3d9_resource.cpp
  src/win32/d3dx9.cpp)
target_link_libraries(recomp_d3d9 PUBLIC recomp_win32 recomp_gfx recomp_hlsl)
# D3DX's maths rounds like the original only without contraction into FMA.
set_source_files_properties(src/win32/d3dx9.cpp TARGET_DIRECTORY recomp_d3d9
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# OpenGL 1.x (opengl32, wgl, gdi32 pixel formats) over gfx::Device.
add_library(recomp_opengl SHARED
  ${RECOMP_ROOT}/src/runtime/gl/context.cpp
  src/win32/opengl32.cpp)
target_link_libraries(recomp_opengl PUBLIC recomp_win32 recomp_gfx)

# dbghelp (UnDecorateSymbolName) on LLVM's Microsoft demangler.
# The x64 C and C++ runtime DLLs (UCRT, VCRUNTIME140, MSVCP140): x64 guests
# import them, 32-bit ones link their CRT statically.
add_library(recomp_ucrt SHARED src/win32/ucrt.cpp
            src/win32/ucrt_stdio.cpp src/win32/ucrt_time.cpp
            src/win32/vcruntime.cpp src/win32/vcruntime_eh.cpp
            src/win32/msvcp140.cpp)
target_link_libraries(recomp_ucrt PUBLIC recomp_win32 PRIVATE coremath)

# Store and online service SDKs (GOG Galaxy, ...), answering as offline.
add_library(recomp_online SHARED src/win32/galaxy64.cpp)
target_link_libraries(recomp_online PUBLIC recomp_win32)

# SDL2.dll for x64 guests that link it themselves, on the host's SDL3.
add_library(recomp_sdl2 SHARED src/win32/sdl2.cpp
            src/win32/sdl2_video.cpp)
target_link_libraries(recomp_sdl2 PUBLIC recomp_win32 SDL3::SDL3)

add_library(recomp_dbghelp SHARED src/win32/dbghelp.cpp)
target_link_libraries(recomp_dbghelp PUBLIC recomp_win32 PRIVATE recomp_demangle)

# recomp_add_digital_reverb_test(DLL <mss32.dll>): checks the Miles-
# compatible digital reverb against the original routines of the user's
# installed mss32.dll (Miles 6.5e), lifted at build time with --only into
# the build tree and run on the x87 model. Nothing from the DLL is kept in
# the source tree; without the DLL there is no test. The routines' VAs are
# those of one build, so any other DLL is skipped (by its SHA-256).
function(recomp_add_digital_reverb_test)
  cmake_parse_arguments(A "" "DLL" "" ${ARGN})
  if(NOT EXISTS "${A_DLL}")
    return()
  endif()
  # mss32.dll 6.5e (docs/miles.md).
  set(_known 7855b8fbae917cb8449f2d4361ab61b5ecec4df0a11130d797cb0aa99b4260ea)
  file(SHA256 "${A_DLL}" _hash)
  if(NOT _hash STREQUAL _known)
    message(STATUS "digital_reverb_oracle: ${A_DLL} is not the mss32.dll 6.5e build "
                   "its addresses come from (SHA-256 ${_hash}); test skipped")
    return()
  endif()
  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/digital_reverb_oracle")
  foreach(_tsv functions exports jumptables)
    file(WRITE "${_dir}/symbols/${_tsv}.tsv" "header\n")
  endforeach()
  file(WRITE "${_dir}/empty.manifest" "")
  set(_gen "${_dir}/gen/gen_000.cpp" "${_dir}/gen/table.cpp")
  add_custom_command(OUTPUT ${_gen}
    COMMAND recomp_lift --exe "${A_DLL}" --symbols "${_dir}/symbols"
            --manifest "${_dir}/empty.manifest"
            --only 2112AA10,2112AA70,2112ACE0,2112B040,2112B0F0,2112BCC0
            --out "${_dir}/gen"
    DEPENDS recomp_lift "${A_DLL}"
    COMMENT "Lifting mss32's reverb routines for digital_reverb_oracle_test")
  add_executable(digital_reverb_oracle_test "${RECOMP_ROOT}/tests/digital_reverb_test.cpp"
                 "${RECOMP_ROOT}/src/runtime/audio/digital_reverb.cpp" ${_gen})
  target_compile_definitions(digital_reverb_oracle_test PRIVATE DIGITAL_REVERB_ORACLE)
  target_include_directories(digital_reverb_oracle_test PRIVATE "${RECOMP_ROOT}/src")
  target_link_libraries(digital_reverb_oracle_test PRIVATE recomp_x87 recomp_pe)
  target_compile_options(digital_reverb_oracle_test PRIVATE -O2 -ffp-contract=off
                         -fno-strict-aliasing)
  add_test(NAME digital_reverb_oracle COMMAND digital_reverb_oracle_test "${A_DLL}")
  set_tests_properties(digital_reverb_oracle PROPERTIES TIMEOUT 600)
endfunction()

# recomp_add_fast2d_test(MSS32 <mss32.dll> FAST2D <msssoft.m3d>): checks
# Miles 6.5's sample levels and Fast 2D (runtime/audio/miles65) bit for bit
# against the original routines, lifted from the user's installed Miles
# 6.5e at build time (tests/fast2d_test.cpp). Nothing from the DLLs is
# stored in the source tree; skipped unless both files are given and are
# the builds the addresses come from.
function(recomp_add_fast2d_test)
  cmake_parse_arguments(A "" "MSS32;FAST2D" "" ${ARGN})
  if(NOT EXISTS "${A_MSS32}" OR NOT EXISTS "${A_FAST2D}")
    return()
  endif()
  # mss32.dll 6.5e and miles/msssoft.m3d (docs/miles.md).
  set(_known_mss32 7855b8fbae917cb8449f2d4361ab61b5ecec4df0a11130d797cb0aa99b4260ea)
  set(_known_fast2d 62233bebccb6aeb97370a8499c577e2982f46924e3a1bc55c53fa1f8a2404252)
  foreach(_f MSS32 FAST2D)
    file(SHA256 "${A_${_f}}" _hash)
    string(TOLOWER "${_f}" _name)
    if(NOT _hash STREQUAL _known_${_name})
      message(STATUS "fast2d_oracle: ${A_${_f}} is not the Miles 6.5e build "
                     "its addresses come from (SHA-256 ${_hash}); test skipped")
      return()
    endif()
  endforeach()
  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/fast2d_oracle")
  foreach(_tsv functions exports jumptables)
    file(WRITE "${_dir}/symbols/${_tsv}.tsv" "header\n")
  endforeach()
  file(WRITE "${_dir}/empty.manifest" "")
  set(_gen "${_dir}/gen/gen_000.cpp" "${_dir}/gen/gen_msssoft_000.cpp" "${_dir}/gen/table.cpp")
  add_custom_command(OUTPUT ${_gen}
    COMMAND recomp_lift --exe "${A_MSS32}" --symbols "${_dir}/symbols"
            --manifest "${_dir}/empty.manifest"
            --module "${A_FAST2D}" --symbols "${_dir}/symbols"
            --manifest "${_dir}/empty.manifest"
            --only 2112A810,2112A930,21136D30,22401000,22401870,224019B0
            --out "${_dir}/gen"
    DEPENDS recomp_lift "${A_MSS32}" "${A_FAST2D}"
    COMMENT "Lifting Miles 6.5e's level and Fast 2D routines for fast2d_oracle_test")
  add_executable(fast2d_oracle_test "${RECOMP_ROOT}/tests/fast2d_test.cpp"
                 "${RECOMP_ROOT}/src/runtime/audio/miles65.cpp" ${_gen})
  target_compile_definitions(fast2d_oracle_test PRIVATE FAST2D_ORACLE)
  target_include_directories(fast2d_oracle_test PRIVATE "${RECOMP_ROOT}/src")
  target_compile_features(fast2d_oracle_test PRIVATE cxx_std_20)
  target_link_libraries(fast2d_oracle_test PRIVATE recomp_x87 recomp_pe)
  target_compile_options(fast2d_oracle_test PRIVATE -O2 -ffp-contract=off
                         -fno-strict-aliasing)
  add_test(NAME fast2d_oracle COMMAND fast2d_oracle_test "${A_MSS32}" "${A_FAST2D}")
  set_tests_properties(fast2d_oracle PROPERTIES TIMEOUT 600)
endfunction()

add_subdirectory(tests)
