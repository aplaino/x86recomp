cmake_minimum_required(VERSION 3.25)
project(x86recomp LANGUAGES C CXX)

# Static recompilation of 32-bit x86 Windows games to native programs: the
# lifter (build-time tool), the x87 model, and the runtime with its Win32/
# DirectX/Miles HLE. A game project pulls this in with add_subdirectory()
# and links the parts its EXE imports; see README.md.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
# ffmpeg (LGPL, linked dynamically): MP3 for Miles streams, Bink movies.
pkg_check_modules(FFMPEG IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale)
if(APPLE)
  set(_llvm_default "/opt/homebrew/opt/llvm@22")  # Homebrew
else()
  set(_llvm_default "/usr/lib/llvm/22")
endif()
set(RECOMP_LLVM_ROOT "${_llvm_default}" CACHE PATH
    "LLVM install with include/llvm/Demangle and libLLVMDemangle.a in lib64 or lib")
# dxvk-native (d3d9 only, SDL3 WSI) for the D3D7 HLE's d3d9 backend. The
# game project sets the default; recomp_d3d7 exists only when it is set.
set(DXVK_NATIVE_DIR "" CACHE PATH "dxvk-native install prefix")

# Where the unwinder cannot leave a signal frame (macOS), a guest exception
# raised by a fault is rethrown as if the faulting instruction had made a
# call (runtime/platform.h); that needs a frame record in every function
# that can fault on guest memory, leaf functions included.
if(APPLE)
  set(RECOMP_FAULT_FRAME_FLAGS -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
      CACHE INTERNAL "")
else()
  set(RECOMP_FAULT_FRAME_FLAGS "" CACHE INTERNAL "")
endif()

# For the helper functions below, which run in the game project's scope.
set(RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# Guest address space + PE mapping. Shared by the runtime and the lifter.
add_library(recomp_image STATIC
  src/runtime/guest_memory.cpp
  src/runtime/pe_image.cpp)
target_include_directories(recomp_image PUBLIC src)
target_compile_features(recomp_image PUBLIC cxx_std_20)

# --- x87 and SSE models (SoftFloat 3e) ---------------------------------
include(cmake/softfloat.cmake)  # defines target `softfloat`
add_library(recomp_x87 STATIC src/runtime/x87.cpp src/runtime/sse.cpp)
target_include_directories(recomp_x87 PUBLIC src)
target_compile_features(recomp_x87 PUBLIC cxx_std_20)
target_link_libraries(recomp_x87 PRIVATE softfloat)
# Every x87 instruction of the game runs through these: always optimise
# (an -O0 debug build made float-heavy scenes several times slower).
# No stack protector: its canary check sat in every hot x87 helper.
# Code that runs the x87 model's host-double paths (x87.h) is built with
# RECOMP_X87_FP_FLAGS: binary64 as written, never contracted into FMA.
set(RECOMP_X87_FP_FLAGS -ffp-contract=off -fno-fast-math CACHE INTERNAL "")
target_compile_options(recomp_x87 PRIVATE -O2 -fno-stack-protector
                       ${RECOMP_X87_FP_FLAGS} ${RECOMP_FAULT_FRAME_FLAGS})
target_compile_options(softfloat PRIVATE -O2)
# ----------------------------------------------------------------------

# Lifter (build-time tool): x86-32 machine code -> C++.
add_subdirectory(tools/lifter)

# Runtime core and the Win32 HLE. OBJECT libraries throughout: handlers
# register themselves from static initialisers, which a static archive
# would drop.
add_library(recomp_runtime OBJECT
  src/runtime/dispatch.cpp
  src/runtime/pacing.cpp
  src/runtime/platform.cpp
  src/runtime/process.cpp
  src/runtime/watchdog.cpp
  src/runtime/guest_alloc.cpp
  src/runtime/lazy_mem.cpp
  src/runtime/hle/hle.cpp
  src/runtime/hle/kernel32.cpp
  src/runtime/hle/kernel32_file.cpp
  src/runtime/hle/kernel32_sync.cpp
  src/runtime/hle/kernel32_seh.cpp
  src/runtime/hle/kernel32_misc.cpp
  src/runtime/hle/advapi32.cpp
  src/runtime/hle/user32_input.cpp
  src/runtime/hle/user32_window.cpp
  src/runtime/hle/gdi32.cpp
  src/runtime/hle/shell_net.cpp
  src/runtime/hle/com.cpp
  src/runtime/hle/present.cpp)
target_link_libraries(recomp_runtime PUBLIC recomp_image recomp_x87
                      SDL3::SDL3 Threads::Threads)

if(FFMPEG_FOUND)
  # The software mixer, its sources, the EAX model and the reverb; no HLE
  # in it (it logs through the runtime's WarnF).
  add_library(recomp_audio STATIC
    src/runtime/audio/source.cpp
    src/runtime/audio/mixer.cpp
    src/runtime/audio/reverb.cpp
    src/runtime/audio/digital_reverb.cpp
    src/runtime/audio/miles65.cpp
    src/runtime/audio/eax.cpp)
  target_include_directories(recomp_audio PUBLIC src)
  target_compile_features(recomp_audio PUBLIC cxx_std_20)
  target_link_libraries(recomp_audio PUBLIC SDL3::SDL3 PkgConfig::FFMPEG)
  # Per-sample host paths: optimised in every build type.
  set_source_files_properties(src/runtime/audio/mixer.cpp
    src/runtime/audio/source.cpp TARGET_DIRECTORY recomp_audio
    PROPERTIES COMPILE_OPTIONS "-O2")
  # The digital reverb and Miles 6.5's levels round like the original
  # only without contraction into FMA.
  set_source_files_properties(src/runtime/audio/digital_reverb.cpp
    src/runtime/audio/miles65.cpp TARGET_DIRECTORY recomp_audio
    PROPERTIES COMPILE_OPTIONS "-O2;-ffp-contract=off")

  # Miles Sound System (mss32) on the software mixer.
  add_library(recomp_mss32 OBJECT src/runtime/hle/mss32.cpp)
  target_link_libraries(recomp_mss32 PUBLIC recomp_runtime recomp_audio)

  # Bink video (binkw32) on ffmpeg; audio through the Miles mixer.
  add_library(recomp_bink OBJECT src/runtime/hle/binkw32.cpp)
  target_link_libraries(recomp_bink PUBLIC recomp_mss32)
endif()

# gfx::Device and its backends; no HLE in it, so a plain static library
# shared by the graphics API modules below. Backends:
# - SDL_GPU (gfx/gpu_device.cpp): always; Vulkan, or Metal on macOS;
# - d3d9 through dxvk-native (gfx/d3d9_device.cpp): when DXVK_NATIVE_DIR
#   is set (Linux), with RECOMP_HAVE_D3D9 defined for the runtime too.
add_library(recomp_gfx STATIC
  src/runtime/gfx/format.cpp
  src/runtime/gfx/device.cpp
  src/runtime/gfx/gpu_device.cpp)
target_include_directories(recomp_gfx PUBLIC src)
target_compile_features(recomp_gfx PUBLIC cxx_std_20)
target_link_libraries(recomp_gfx PUBLIC SDL3::SDL3)
# SDL_GPU's shaders, one GLSL source per stage (gfx/shaders): glslang
# compiles it to SPIR-V for Vulkan; for Metal, SPIRV-Cross turns a build
# with Metal's binding order (METAL defined) into MSL source. Either way
# the result is embedded in a header (ff_<stage>.h, array kFf<Stage>).
find_program(RECOMP_GLSLANG glslangValidator REQUIRED)
if(APPLE)
  find_program(RECOMP_SPIRV_CROSS spirv-cross REQUIRED)
endif()
set(_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/gfx_shaders")
set(_shader_name_vert kFfVert)
set(_shader_name_frag kFfFrag)
foreach(_stage vert frag)
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/gfx/shaders/ff.${_stage}")
  set(_out "${_shader_dir}/ff_${_stage}.h")
  if(APPLE)
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
      COMMAND "${RECOMP_GLSLANG}" -V --quiet -DMETAL
              -o "${_shader_dir}/ff_${_stage}.spv" "${_src}"
      COMMAND "${RECOMP_SPIRV_CROSS}" --msl --msl-version 20100
              --msl-decoration-binding --msl-no-clip-distance-user-varying
              "${_shader_dir}/ff_${_stage}.spv" --output "${_shader_dir}/ff_${_stage}.metal"
      COMMAND "${CMAKE_COMMAND}" -DIN=${_shader_dir}/ff_${_stage}.metal -DOUT=${_out}
              -DNAME=${_shader_name_${_stage}}
              -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake"
      DEPENDS "${_src}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake" VERBATIM)
  else()
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
      COMMAND "${RECOMP_GLSLANG}" -V --target-env vulkan1.0 --quiet
              --vn ${_shader_name_${_stage}} -o "${_out}" "${_src}"
      DEPENDS "${_src}" VERBATIM)
  endif()
  target_sources(recomp_gfx PRIVATE "${_out}")
endforeach()
target_include_directories(recomp_gfx PRIVATE "${_shader_dir}")
# Per-pixel host paths: optimised in every build type.
set_source_files_properties(src/runtime/gfx/format.cpp
  TARGET_DIRECTORY recomp_gfx PROPERTIES COMPILE_OPTIONS "-O2")
if(DXVK_NATIVE_DIR)
  find_library(RECOMP_DXVK_D3D9 dxvk_d3d9
    PATHS "${DXVK_NATIVE_DIR}/lib64" "${DXVK_NATIVE_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
  target_sources(recomp_gfx PRIVATE src/runtime/gfx/d3d9_device.cpp)
  target_link_libraries(recomp_gfx PUBLIC "${RECOMP_DXVK_D3D9}")
  target_compile_definitions(recomp_gfx PUBLIC RECOMP_HAVE_D3D9)
  target_compile_definitions(recomp_runtime PUBLIC RECOMP_HAVE_D3D9)
  # Only the d3d9 backend sees dxvk's Windows/D3D9 headers.
  set_source_files_properties(src/runtime/gfx/d3d9_device.cpp
    TARGET_DIRECTORY recomp_gfx PROPERTIES
    INCLUDE_DIRECTORIES "${DXVK_NATIVE_DIR}/include/dxvk"
    COMPILE_DEFINITIONS "DXVK_WSI_SDL3")
endif()

# DirectDraw 7 / Direct3D 7 over gfx::Device.
add_library(recomp_d3d7 OBJECT
  src/runtime/hle/ddraw.cpp
  src/runtime/hle/d3d7.cpp)
target_link_libraries(recomp_d3d7 PUBLIC recomp_runtime recomp_gfx)

# DirectInput 8 (keyboard and mouse) on the event pump's raw input.
add_library(recomp_dinput8 OBJECT src/runtime/hle/dinput8.cpp)
target_link_libraries(recomp_dinput8 PUBLIC recomp_runtime)

# Direct3D 9 and D3DX 9 over gfx::Device.
add_library(recomp_d3d9 OBJECT src/runtime/hle/d3d9.cpp
  src/runtime/hle/d3d9_resource.cpp src/runtime/hle/d3dx9.cpp)
target_link_libraries(recomp_d3d9 PUBLIC recomp_runtime recomp_gfx)

# OpenGL 1.x (opengl32, wgl, gdi32 pixel formats) over gfx::Device.
add_library(recomp_opengl OBJECT
  src/runtime/gl/context.cpp
  src/runtime/hle/opengl32.cpp)
target_link_libraries(recomp_opengl PUBLIC recomp_runtime recomp_gfx)

# dbghelp (UnDecorateSymbolName) on LLVM's Microsoft demangler.
add_library(recomp_dbghelp OBJECT src/runtime/hle/dbghelp.cpp)
find_library(RECOMP_LLVM_DEMANGLE libLLVMDemangle.a
  PATHS "${RECOMP_LLVM_ROOT}/lib64" "${RECOMP_LLVM_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
target_link_libraries(recomp_dbghelp PUBLIC recomp_runtime "${RECOMP_LLVM_DEMANGLE}")
target_include_directories(recomp_dbghelp PRIVATE "${RECOMP_LLVM_ROOT}/include")

# recomp_add_lifted(<target> <dir>): an OBJECT library of the lifter's
# output in <dir> (globbed, so re-run cmake after lifting). Defines nothing
# when <dir> has no lifted code yet.
function(recomp_add_lifted target dir)
  file(GLOB _gen CONFIGURE_DEPENDS "${dir}/*.cpp")
  if(NOT _gen)
    return()
  endif()
  # Lifted code is always optimised: at -O0 it would be unusably slow and
  # debug info for 100+ MB of generated source is not useful.
  add_library(${target} OBJECT ${_gen})
  target_include_directories(${target} PRIVATE "${RECOMP_ROOT}/src")
  target_compile_features(${target} PRIVATE cxx_std_20)
  # -fnon-call-exceptions: guest access violations are raised from the
  # SIGSEGV handler and may unwind (GuestResume) out of the faulting frame.
  target_compile_options(${target} PRIVATE -O2 -g0 -fno-strict-aliasing
                         -fnon-call-exceptions ${RECOMP_X87_FP_FLAGS}
                         ${RECOMP_FAULT_FRAME_FLAGS})
endfunction()

# recomp_add_lift_tests(EXE <exe> SYMBOLS <dir> MANIFEST <file>): oracle
# tests that run every x87, integer and SSE instruction form found in the
# EXE lifted and natively (x86-64 hosts only).
function(recomp_add_lift_tests)
  cmake_parse_arguments(A "" "EXE;SYMBOLS;MANIFEST" "" ${ARGN})
  if(NOT EXISTS "${A_EXE}" OR
     NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    return()
  endif()
  set(_tests "${RECOMP_ROOT}/tests")
  foreach(_kind x87 int sse)
    set(_forms "${CMAKE_CURRENT_BINARY_DIR}/lift_${_kind}_forms.cpp")
    add_custom_command(OUTPUT "${_forms}"
      COMMAND recomp_lift --exe "${A_EXE}" --symbols "${A_SYMBOLS}"
              --manifest "${A_MANIFEST}" --${_kind}-test "${_forms}"
      DEPENDS recomp_lift "${A_MANIFEST}"
      COMMENT "Lifting the EXE's ${_kind} instruction forms for lift_${_kind}_test")
    add_executable(lift_${_kind}_test "${_tests}/lift_${_kind}_test.cpp" "${_forms}")
    target_include_directories(lift_${_kind}_test PRIVATE "${RECOMP_ROOT}/src" "${_tests}")
    target_link_libraries(lift_${_kind}_test PRIVATE recomp_x87 recomp_image)
    target_compile_options(lift_${_kind}_test PRIVATE -O1 -fno-strict-aliasing
                           ${RECOMP_X87_FP_FLAGS})
    add_test(NAME lift_${_kind} COMMAND lift_${_kind}_test)
  endforeach()
  set_tests_properties(lift_x87 PROPERTIES TIMEOUT 600)
  set_tests_properties(lift_int PROPERTIES TIMEOUT 900)
  set_tests_properties(lift_sse PROPERTIES TIMEOUT 900)
endfunction()

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
  target_link_libraries(digital_reverb_oracle_test PRIVATE recomp_x87 recomp_image)
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
  target_link_libraries(fast2d_oracle_test PRIVATE recomp_x87 recomp_image)
  target_compile_options(fast2d_oracle_test PRIVATE -O2 -ffp-contract=off
                         -fno-strict-aliasing)
  add_test(NAME fast2d_oracle COMMAND fast2d_oracle_test "${A_MSS32}" "${A_FAST2D}")
  set_tests_properties(fast2d_oracle PROPERTIES TIMEOUT 600)
endfunction()

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
endif()
add_subdirectory(tests)
# Dev tool: renders the EAX capture cases with recomp_audio (eax_render).
if(PROJECT_IS_TOP_LEVEL AND FFMPEG_FOUND)
  add_subdirectory(tools/eax_capture)
endif()
