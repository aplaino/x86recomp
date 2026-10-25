cmake_minimum_required(VERSION 3.25)
project(x86recomp LANGUAGES C CXX)

# Static recompilation of 32-bit x86 Windows games to native programs: the
# lifter (build-time tool), the x87 model, and the runtime with its Win32/
# DirectX/Miles HLE. A game project pulls this in with add_subdirectory()
# and links the parts its EXE imports; see README.md. Console platforms
# are modules on top, each behind its own option (ARCHITECTURE.md,
# "Console platforms").

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# For the helper functions below, which run in the game project's scope.
set(RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# The runtime, the HLE modules and their helpers are shared libraries
# (ARCHITECTURE.md "Game projects"): one copy of their state, and a change
# to one rebuilds only that library. Their thread-locals use the
# initial-exec model: the fault handler reads them, and a general-dynamic
# access can allocate. --no-undefined holds Linux to what macOS requires
# of a dylib: everything it uses comes from the libraries it links.
add_compile_options(-ftls-model=initial-exec)
if(NOT APPLE)
  string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--no-undefined")
endif()

# --- guests and host -------------------------------------------------------
# Guests: RECOMP_X86 is everything at the end of this file, the x86 lifter
# and CPU model with the Win32 HLE (PC games); each console platform is a
# module with its own option. A game for another CPU turns RECOMP_X86 off.
option(RECOMP_X86 "x86 lifter, CPU model and Win32 HLE (PC games)" ON)
option(RECOMP_PLATFORM_HORIZON "Nintendo Switch (Horizon OS) platform module" OFF)
option(RECOMP_PLATFORM_XBOX360 "Xbox 360 platform module" OFF)

# The host layer (cmake/host.cmake): what a native program needs on its
# host, whatever its guest CPU. SDL3 (windows, input, audio output),
# ffmpeg (movies, compressed audio), LLVM's demanglers (Microsoft and
# Itanium names), gfx::Device on Vulkan or Metal with the shader
# compilers, the software mixer and logging; the guests' libraries and
# platform modules link what they use. Off leaves a CPU-only build (the
# AArch64 lifter tests cross built for an arm64 host whose sysroot has
# none of those libraries); no product builds without it.
option(RECOMP_HOST "Host layer: SDL3, ffmpeg, LLVM demangling, gfx::Device, audio" ON)
if(RECOMP_X86 AND NOT RECOMP_HOST)
  message(FATAL_ERROR "RECOMP_X86 needs RECOMP_HOST")
endif()
if(RECOMP_HOST)
  include(cmake/host.cmake)
endif()

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
endif()

# The AArch64 lifter and CPU model, for the platforms with an AArch64 CPU
# (the Switch), then those platforms.
if(RECOMP_PLATFORM_HORIZON)
  add_library(recomp_a64 SHARED src/a64/dispatch.cpp src/a64/fatal.cpp src/a64/platform.cpp)
  target_include_directories(recomp_a64 PUBLIC src)
  target_compile_features(recomp_a64 PUBLIC cxx_std_20)
  add_subdirectory(tools/lifter_a64)

  # recomp_add_lifted_a64(<target> <dir>): an object library of
  # recomp_lift_a64's output in <dir> plus the registration of its function
  # table. Lifted code is always optimised: at -O0 it is unusably slow,
  # and debug info for generated source is not useful.
  function(recomp_add_lifted_a64 target dir)
    file(GLOB _gen CONFIGURE_DEPENDS "${dir}/*.cpp")
    recomp_add_lifted_a64_files(${target} ${_gen})
  endfunction()
  # The same over a known list of generated files (a build-time lift).
  function(recomp_add_lifted_a64_files target)
    add_library(${target} OBJECT ${ARGN} "${RECOMP_ROOT}/src/a64/lifted_table.cpp")
    target_include_directories(${target} PRIVATE "${RECOMP_ROOT}/src")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -O2 -g0 -fno-strict-aliasing)
  endfunction()

  add_subdirectory(platforms/horizon)
endif()

# The PowerPC lifter and CPU model, for the platforms with a PowerPC CPU
# (the Xbox 360), then those platforms.
if(RECOMP_PLATFORM_XBOX360)
  add_library(recomp_ppc SHARED src/ppc/dispatch.cpp src/ppc/fatal.cpp)
  target_include_directories(recomp_ppc PUBLIC src)
  target_compile_features(recomp_ppc PUBLIC cxx_std_20)
  add_subdirectory(tools/lifter_ppc)

  # recomp_add_lifted_ppc(<target> <dir>): an object library of
  # recomp_lift_ppc's output in <dir> plus the registration of its function
  # table, optimised like recomp_add_lifted_a64's.
  function(recomp_add_lifted_ppc target dir)
    file(GLOB _gen CONFIGURE_DEPENDS "${dir}/*.cpp")
    recomp_add_lifted_ppc_files(${target} ${_gen})
  endfunction()
  # The same over a known list of generated files (a build-time lift).
  function(recomp_add_lifted_ppc_files target)
    add_library(${target} OBJECT ${ARGN} "${RECOMP_ROOT}/src/ppc/lifted_table.cpp")
    target_include_directories(${target} PRIVATE "${RECOMP_ROOT}/src")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -O2 -g0 -fno-strict-aliasing)
  endfunction()
endif()

if(NOT RECOMP_X86)
  return()
endif()

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

# Guest address space + PE mapping. Shared by the runtime, the x87 model
# and the lifter.
add_library(recomp_image SHARED
  src/runtime/guest_memory.cpp
  src/runtime/pe_image.cpp
  src/runtime/unwind.cpp)
target_include_directories(recomp_image PUBLIC src)
target_compile_features(recomp_image PUBLIC cxx_std_20)
target_link_libraries(recomp_image PUBLIC recomp_log)

# --- x87 and SSE models (SoftFloat 3e) ---------------------------------
include(cmake/softfloat.cmake)  # defines target `softfloat`
include(cmake/coremath.cmake)  # defines target `coremath`
add_library(recomp_x87 SHARED src/runtime/x87.cpp src/runtime/sse.cpp)
target_include_directories(recomp_x87 PUBLIC src)
target_compile_features(recomp_x87 PUBLIC cxx_std_20)
target_link_libraries(recomp_x87 PRIVATE softfloat recomp_image)
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

# Runtime core and the Win32 HLE. HLE handlers register themselves from
# static initialisers, which run when their library is loaded.
add_library(recomp_runtime SHARED
  src/runtime/dispatch.cpp
  src/runtime/pacing.cpp
  src/runtime/platform.cpp
  src/runtime/process.cpp
  src/runtime/watchdog.cpp
  src/runtime/guest_alloc.cpp
  src/runtime/lazy_mem.cpp
  src/runtime/hle/hle.cpp
  src/runtime/hle/kernel32.cpp
  src/runtime/hle/winmm_timer.cpp
  src/runtime/hle/kernel32_file.cpp
  src/runtime/hle/kernel32_sync.cpp
  src/runtime/hle/kernel32_seh.cpp
  src/runtime/hle/seh64.cpp
  src/runtime/hle/kernel32_misc.cpp
  src/runtime/hle/advapi32.cpp
  src/runtime/hle/user32_input.cpp
  src/runtime/hle/user32_window.cpp
  src/runtime/hle/gdi32.cpp
  src/runtime/hle/shell_net.cpp
  src/runtime/hle/ws2_32.cpp
  src/runtime/hle/com.cpp
  src/runtime/hle/present.cpp)
target_link_libraries(recomp_runtime PUBLIC recomp_image recomp_x87
                      SDL3::SDL3 Threads::Threads
                      PRIVATE recomp_gfx)  # presentation (hle/present.cpp)
# Nothing in a game refers to an HLE module's symbols: keep the linker
# from dropping them where it links --as-needed (ld64 keeps every dylib).
if(NOT APPLE)
  target_link_options(recomp_runtime INTERFACE "LINKER:--no-as-needed")
endif()

if(FFMPEG_FOUND)
  # Miles Sound System (mss32) on the software mixer.
  add_library(recomp_mss32 SHARED src/runtime/hle/mss32.cpp)
  target_link_libraries(recomp_mss32 PUBLIC recomp_runtime recomp_audio)

  # DirectSound 8 (dsound) on the same software mixer.
  add_library(recomp_dsound SHARED src/runtime/hle/dsound.cpp)
  target_link_libraries(recomp_dsound PUBLIC recomp_runtime recomp_audio)

  # Bink video (binkw32) on ffmpeg; audio through the Miles mixer.
  add_library(recomp_bink SHARED src/runtime/hle/binkw32.cpp)
  target_link_libraries(recomp_bink PUBLIC recomp_mss32)
endif()



if(DXVK_NATIVE_DIR)
  target_compile_definitions(recomp_runtime PUBLIC RECOMP_HAVE_D3D9)
endif()

# DirectDraw 7 / Direct3D 7 over gfx::Device.
add_library(recomp_d3d7 SHARED
  src/runtime/hle/ddraw.cpp
  src/runtime/hle/d3d7.cpp)
target_link_libraries(recomp_d3d7 PUBLIC recomp_runtime recomp_gfx)

# XInput 1.3 on SDL gamepads, and the WMI that tells games which
# DirectInput game controllers are XInput pads.
add_library(recomp_xinput SHARED src/runtime/hle/xinput.cpp src/runtime/hle/wbem.cpp)
target_link_libraries(recomp_xinput PUBLIC recomp_runtime)

# DirectInput 8 (keyboard and mouse) on the event pump's raw input; it
# lists the XInput pads as game controllers.
add_library(recomp_dinput8 SHARED src/runtime/hle/dinput8.cpp)
target_link_libraries(recomp_dinput8 PUBLIC recomp_runtime recomp_xinput)

# Direct3D 9 and D3DX 9 over gfx::Device.
add_library(recomp_d3d9 SHARED src/runtime/hle/d3d9.cpp src/runtime/hle/d3d9_draw.cpp
  src/runtime/hle/d3d9_resource.cpp src/runtime/hle/d3dx9.cpp)
target_link_libraries(recomp_d3d9 PUBLIC recomp_runtime recomp_gfx recomp_hlsl)
# D3DX's maths rounds like the original only without contraction into FMA.
set_source_files_properties(src/runtime/hle/d3dx9.cpp TARGET_DIRECTORY recomp_d3d9
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# OpenGL 1.x (opengl32, wgl, gdi32 pixel formats) over gfx::Device.
add_library(recomp_opengl SHARED
  src/runtime/gl/context.cpp
  src/runtime/hle/opengl32.cpp)
target_link_libraries(recomp_opengl PUBLIC recomp_runtime recomp_gfx)

# dbghelp (UnDecorateSymbolName) on LLVM's Microsoft demangler.
# The x64 C and C++ runtime DLLs (UCRT, VCRUNTIME140, MSVCP140): x64 guests
# import them, 32-bit ones link their CRT statically.
add_library(recomp_ucrt SHARED src/runtime/hle/ucrt.cpp src/runtime/hle/ucrt_stdio.cpp
            src/runtime/hle/ucrt_time.cpp
            src/runtime/hle/vcruntime.cpp src/runtime/hle/vcruntime_eh.cpp
            src/runtime/hle/msvcp140.cpp)
target_link_libraries(recomp_ucrt PUBLIC recomp_runtime PRIVATE coremath)

# Store and online service SDKs (GOG Galaxy, ...), answering as offline.
add_library(recomp_online SHARED src/runtime/hle/galaxy64.cpp)
target_link_libraries(recomp_online PUBLIC recomp_runtime)

# SDL2.dll for x64 guests that link it themselves, on the host's SDL3.
add_library(recomp_sdl2 SHARED src/runtime/hle/sdl2.cpp src/runtime/hle/sdl2_video.cpp)
target_link_libraries(recomp_sdl2 PUBLIC recomp_runtime SDL3::SDL3)

add_library(recomp_dbghelp SHARED src/runtime/hle/dbghelp.cpp)
target_link_libraries(recomp_dbghelp PUBLIC recomp_runtime PRIVATE recomp_demangle)

# recomp_add_lifted(<target> <dir>): an OBJECT library of the lifter's
# output in <dir> (globbed, so re-run cmake after lifting), with the
# registration of its function table (runtime/lifted_table.cpp). Defines
# nothing when <dir> has no lifted code yet.
function(recomp_add_lifted target dir)
  file(GLOB _gen CONFIGURE_DEPENDS "${dir}/*.cpp")
  if(NOT _gen)
    return()
  endif()
  # Lifted code is always optimised: at -O0 it would be unusably slow and
  # debug info for 100+ MB of generated source is not useful.
  add_library(${target} OBJECT ${_gen} "${RECOMP_ROOT}/src/runtime/lifted_table.cpp")
  target_include_directories(${target} PRIVATE "${RECOMP_ROOT}/src")
  target_compile_features(${target} PRIVATE cxx_std_20)
  # -fnon-call-exceptions: guest access violations are raised from the
  # SIGSEGV handler and may unwind (GuestResume) out of the faulting frame.
  target_compile_options(${target} PRIVATE -O2 -g0 -fno-strict-aliasing
                         -fnon-call-exceptions ${RECOMP_X87_FP_FLAGS}
                         ${RECOMP_FAULT_FRAME_FLAGS})
endfunction()

# recomp_add_lift_tests(EXE <exe> SYMBOLS <dir> MANIFEST <file> [X64]):
# oracle tests that run every x87, integer and SSE instruction form found
# in the EXE lifted and natively (x86-64 hosts only). X64: the EXE is an
# x64 image; its integer and SSE/AVX forms go to tests/lift_x64_test.cpp
# (lift_int64, lift_sse64; the latter needs an AVX host).
function(recomp_add_lift_tests)
  cmake_parse_arguments(A "X64" "EXE;SYMBOLS;MANIFEST" "" ${ARGN})
  if(NOT EXISTS "${A_EXE}" OR
     NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    return()
  endif()
  set(_tests "${RECOMP_ROOT}/tests")
  if(A_X64)
    foreach(_kind int sse)
      set(_forms "${CMAKE_CURRENT_BINARY_DIR}/lift_${_kind}64_forms.cpp")
      add_custom_command(OUTPUT "${_forms}"
        COMMAND recomp_lift --exe "${A_EXE}" --symbols "${A_SYMBOLS}"
                --manifest "${A_MANIFEST}" --${_kind}-test "${_forms}"
        DEPENDS recomp_lift "${A_MANIFEST}"
        COMMENT "Lifting the EXE's ${_kind} instruction forms for lift_${_kind}64_test")
      add_executable(lift_${_kind}64_test "${_tests}/lift_x64_test.cpp" "${_forms}")
      target_include_directories(lift_${_kind}64_test PRIVATE "${RECOMP_ROOT}/src" "${_tests}")
      target_link_libraries(lift_${_kind}64_test PRIVATE recomp_x87 recomp_image)
      target_compile_options(lift_${_kind}64_test PRIVATE -O1 -fno-strict-aliasing
                             ${RECOMP_X87_FP_FLAGS})
      if(_kind STREQUAL "sse")
        target_compile_definitions(lift_${_kind}64_test PRIVATE LIFT_X64_SSE)
      endif()
      add_test(NAME lift_${_kind}64 COMMAND lift_${_kind}64_test)
      set_tests_properties(lift_${_kind}64 PROPERTIES TIMEOUT 1800)
    endforeach()
    return()
  endif()
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

add_subdirectory(tests)
# Dev tool: renders the EAX capture cases with recomp_audio (eax_render).
if(PROJECT_IS_TOP_LEVEL AND FFMPEG_FOUND)
  add_subdirectory(tools/eax_capture)
endif()
