cmake_minimum_required(VERSION 3.25)
project(SwARc LANGUAGES C CXX)

# Static recompilation of games to native programs. Three layers
# (ARCHITECTURE.md, "Layout"): the base every guest shares (the host
# layer, guest memory), a CPU core per guest CPU (the x86 core: its
# lifter, CPU model and runtime), and the platform modules on a core
# (platforms/<name>: Win32 for PC games, the consoles), each behind its own
# option. A game project pulls this in with add_subdirectory() and links
# the parts its program imports; see README.md.

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

# recomp_bundle(<target>...): programs and the shared libraries they load
# sit side by side in RECOMP_BUNDLE_DIR, found through an rpath relative
# to themselves ($ORIGIN, @loader_path on macOS), so the directory can be
# moved or installed as a whole and one library (a lifted module, the
# runtime, an HLE module) replaced alone. Used by the console platforms
# and their game projects.
set(RECOMP_BUNDLE_DIR "${CMAKE_BINARY_DIR}/bin" CACHE PATH
    "Where recomp_bundle() puts programs and their shared libraries")
if(APPLE)
  set(RECOMP_BUNDLE_RPATH "@loader_path" CACHE INTERNAL "")
else()
  set(RECOMP_BUNDLE_RPATH "$ORIGIN" CACHE INTERNAL "")
endif()
function(recomp_bundle)
  set_target_properties(${ARGN} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RECOMP_BUNDLE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RECOMP_BUNDLE_DIR}"
    BUILD_RPATH "${RECOMP_BUNDLE_RPATH}"
    INSTALL_RPATH "${RECOMP_BUNDLE_RPATH}"
    BUILD_RPATH_USE_ORIGIN ON)
endfunction()

# --- guests and host -------------------------------------------------------
# Guests: RECOMP_X86 is everything at the end of this file, the x86 core
# (lifter, CPU model, runtime); each platform is a module on a core, with
# its own option. A game for another CPU turns RECOMP_X86 off.
option(RECOMP_X86 "x86 core: lifter, CPU model and runtime" ON)
# Windows PC games, on the x86 core.
option(RECOMP_PLATFORM_WIN32 "Win32 platform module (Windows PC games)" ON)
if(RECOMP_PLATFORM_WIN32 AND NOT RECOMP_X86)
  message(FATAL_ERROR "RECOMP_PLATFORM_WIN32 needs RECOMP_X86")
endif()
# Consoles with a PowerPC CPU.
option(RECOMP_PPC "PowerPC core: lifter and CPU model (a PowerPC platform turns it on)" OFF)
# The Xbox 360, on the PowerPC core.
option(RECOMP_PLATFORM_XBOX360 "Xbox 360 platform module" OFF)
if(RECOMP_PLATFORM_XBOX360)
  set(RECOMP_PPC ON)
endif()

# Consoles with an AArch64 CPU.
option(RECOMP_A64 "AArch64 core: lifter and CPU model (an AArch64 platform turns it on)" OFF)
# The Nintendo Switch, on the AArch64 core.
option(RECOMP_PLATFORM_HORIZON "Nintendo Switch (Horizon OS) platform module" OFF)
if(RECOMP_PLATFORM_HORIZON)
  set(RECOMP_A64 ON)
endif()

# The host layer (cmake/host.cmake): what a native program needs on its
# host, whatever its guest CPU. SDL3 (windows, input, audio output),
# ffmpeg (movies, compressed audio), LLVM's demanglers (Microsoft and
# Itanium names), gfx::Device on Vulkan or Metal with the shader
# compilers, the software mixer and logging; the guests' libraries and
# platform modules link what they use. Off leaves a CPU-only build (a
# lifter's tests cross built for a host whose sysroot has none of those
# libraries); no product builds without it.
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

# --- base: exact floating point --------------------------------------------
# SoftFloat 3e (cmake/softfloat.cmake), the definition of correct for every
# core's floating-point model. Built only for the libraries that link it.
include(FetchContent)
include(cmake/softfloat.cmake)  # defines target `softfloat`
target_compile_options(softfloat PRIVATE -O2)
set_target_properties(softfloat PROPERTIES EXCLUDE_FROM_ALL ON)

# --- PowerPC core ----------------------------------------------------------
# The PowerPC lifter and CPU model, for the platforms with a PowerPC CPU
# (RECOMP_PPC, which they turn on), its tests, then those platforms.
if(RECOMP_PPC)
  # The base's SoftFloat under the exact floating-point unit
  # (src/ppc/fp.cpp), which applies the PowerPC's own NaN rules.
  add_library(recomp_ppc SHARED src/ppc/dispatch.cpp src/ppc/fatal.cpp src/ppc/fp.cpp src/ppc/unwind.cpp
              src/ppc/vmx.cpp)
  target_include_directories(recomp_ppc PUBLIC src)
  target_compile_features(recomp_ppc PUBLIC cxx_std_20)
  target_link_libraries(recomp_ppc PRIVATE softfloat)
  recomp_bundle(recomp_ppc)
  add_subdirectory(tools/lifter_ppc)

  # recomp_add_lifted_ppc(<target> <dir> LINK <platform library>): a shared
  # library (a lifted module) of recomp_lift_ppc's output in <dir> plus
  # the registration of its function table, which runs when the library
  # loads. LINK names the platform library that defines what lifted code
  # calls (Import, Sc, Trap, TimeBase). Lifted code is always optimised
  # (at -O0 it is unusably slow), and its symbols stay inside the library:
  # calls between lifted functions are direct, and the dynamic symbol table
  # holds none of them. A program links its lifted modules --no-as-needed,
  # as nothing in it refers to them.
  function(recomp_add_lifted_ppc target dir)
    file(GLOB _gen CONFIGURE_DEPENDS "${dir}/*.cpp")
    recomp_add_lifted_ppc_files(${target} ${_gen} ${ARGN})
  endfunction()
  # The same over a known list of generated files (a build-time lift):
  # recomp_add_lifted_ppc_files(<target> <file>... LINK <platform library>).
  function(recomp_add_lifted_ppc_files target)
    cmake_parse_arguments(PARSE_ARGV 1 _l "" "" "LINK")
    add_library(${target} SHARED ${_l_UNPARSED_ARGUMENTS} "${RECOMP_ROOT}/src/ppc/lifted_table.cpp")
    target_include_directories(${target} PRIVATE "${RECOMP_ROOT}/src")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -O2 -g0 -fno-strict-aliasing)
    set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_link_libraries(${target} PRIVATE recomp_ppc ${_l_LINK})
    if(NOT APPLE)
      target_link_options(${target} INTERFACE "LINKER:--no-as-needed")
    endif()
    recomp_bundle(${target})
  endfunction()

  add_subdirectory(tests/ppc)

  if(RECOMP_PLATFORM_XBOX360)
    add_subdirectory(platforms/xbox360)
  endif()
endif()

# --- AArch64 core ----------------------------------------------------------
# The AArch64 lifter and CPU model, for the platforms with an AArch64 CPU
# (RECOMP_A64, which they turn on), its tests, then those platforms.
if(RECOMP_A64)
  add_library(recomp_a64 SHARED src/a64/dispatch.cpp src/a64/fatal.cpp src/a64/platform.cpp)
  target_include_directories(recomp_a64 PUBLIC src)
  target_compile_features(recomp_a64 PUBLIC cxx_std_20)
  # Position-independent modules (a64/image.h), for the lifter and the
  # runtime loaders: static-PIE ELF here, a platform's formats on top.
  add_library(recomp_a64_image SHARED src/a64/image.cpp)
  target_include_directories(recomp_a64_image PUBLIC src)
  target_compile_features(recomp_a64_image PUBLIC cxx_std_20)
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

  add_subdirectory(tests/a64)

  if(RECOMP_PLATFORM_HORIZON)
    add_subdirectory(platforms/horizon)
  endif()
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

# --- base runtime ----------------------------------------------------------
# What any guest's runtime builds on (src/runtime): the guest address
# space (arena, allocator, lazily filled regions), the host platform layer
# (signals, threads' stacks) and frame pacing. Only the x86 core uses it
# so far.
add_library(recomp_base SHARED
  src/runtime/guest_memory.cpp
  src/runtime/guest_alloc.cpp
  src/runtime/lazy_mem.cpp
  src/runtime/platform.cpp
  src/runtime/pacing.cpp)
target_include_directories(recomp_base PUBLIC src)
target_compile_features(recomp_base PUBLIC cxx_std_20)
target_link_libraries(recomp_base PUBLIC recomp_log Threads::Threads)

# --- x86 core --------------------------------------------------------------
# x87 and SSE models (SoftFloat 3e, from the base).
add_library(recomp_x87 SHARED src/x86/x87.cpp src/x86/sse.cpp)
target_include_directories(recomp_x87 PUBLIC src)
target_compile_features(recomp_x87 PUBLIC cxx_std_20)
target_link_libraries(recomp_x87 PRIVATE softfloat recomp_base)
# Every x87 instruction of the game runs through these: always optimise
# (an -O0 debug build made float-heavy scenes several times slower).
# No stack protector: its canary check sat in every hot x87 helper.
# Code that runs the x87 model's host-double paths (x87.h) is built with
# RECOMP_X87_FP_FLAGS: binary64 as written, never contracted into FMA.
set(RECOMP_X87_FP_FLAGS -ffp-contract=off -fno-fast-math CACHE INTERNAL "")
target_compile_options(recomp_x87 PRIVATE -O2 -fno-stack-protector
                       ${RECOMP_X87_FP_FLAGS} ${RECOMP_FAULT_FRAME_FLAGS})

# Lifter (build-time tool): x86 machine code -> C++. It reads no
# executable format itself: each platform adds the reader of its own
# (ImageReader, tools/lifter_x86/program.h).
add_subdirectory(tools/lifter_x86)

# The runtime of every x86 guest: dispatch and the fault handler, the
# process and its threads, calls between the host and lifted code, the
# HLE registry and its calling conventions, presentation and the hang
# watchdog. A platform module loads the program and sets its hooks
# (x86/runtime.h).
add_library(recomp_x86 SHARED
  src/x86/dispatch.cpp
  src/x86/process.cpp
  src/x86/hle.cpp
  src/x86/present.cpp
  src/x86/watchdog.cpp)
target_link_libraries(recomp_x86 PUBLIC recomp_base recomp_x87 SDL3::SDL3
                      PRIVATE recomp_gfx)  # presentation (x86/present.cpp)
if(DXVK_NATIVE_DIR)
  target_compile_definitions(recomp_x86 PUBLIC RECOMP_HAVE_D3D9)
endif()

# recomp_add_lifted(<target> <dir>): an OBJECT library of the lifter's
# output in <dir> (globbed, so re-run cmake after lifting), with the
# registration of its function table (x86/lifted_table.cpp). Defines
# nothing when <dir> has no lifted code yet.
function(recomp_add_lifted target dir)
  file(GLOB _gen CONFIGURE_DEPENDS "${dir}/*.cpp")
  if(NOT _gen)
    return()
  endif()
  # Lifted code is always optimised: at -O0 it would be unusably slow and
  # debug info for 100+ MB of generated source is not useful.
  add_library(${target} OBJECT ${_gen} "${RECOMP_ROOT}/src/x86/lifted_table.cpp")
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
# x64 image; its integer and SSE/AVX forms go to tests/x86/lift_x64_test.cpp
# (lift_int64, lift_sse64; the latter needs an AVX host).
function(recomp_add_lift_tests)
  cmake_parse_arguments(A "X64" "EXE;SYMBOLS;MANIFEST" "" ${ARGN})
  if(NOT EXISTS "${A_EXE}" OR
     NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    return()
  endif()
  set(_tests "${RECOMP_ROOT}/tests/x86")
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
      target_link_libraries(lift_${_kind}64_test PRIVATE recomp_x87 recomp_base)
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
    target_link_libraries(lift_${_kind}_test PRIVATE recomp_x87 recomp_base)
    target_compile_options(lift_${_kind}_test PRIVATE -O1 -fno-strict-aliasing
                           ${RECOMP_X87_FP_FLAGS})
    add_test(NAME lift_${_kind} COMMAND lift_${_kind}_test)
  endforeach()
  set_tests_properties(lift_x87 PROPERTIES TIMEOUT 600)
  set_tests_properties(lift_int PROPERTIES TIMEOUT 900)
  set_tests_properties(lift_sse PROPERTIES TIMEOUT 900)
endfunction()

add_subdirectory(tests/x86)
if(RECOMP_PLATFORM_WIN32)
  add_subdirectory(platforms/win32)
endif()

# --- base tests and tools --------------------------------------------------
add_subdirectory(tests)
# Dev tool: renders the EAX capture cases with recomp_audio (eax_render).
if(PROJECT_IS_TOP_LEVEL AND FFMPEG_FOUND)
  add_subdirectory(tools/eax_capture)
endif()
