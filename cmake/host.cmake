# The host layer (CMakeLists.txt, RECOMP_HOST), included from the top
# level, so paths are relative to it.

include(FetchContent)

find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
# ffmpeg (LGPL, linked dynamically): MP3 for Miles streams, Bink and
# console movies.
pkg_check_modules(FFMPEG IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale)
# FreeType (FTL, linked dynamically): the font engine of consoles whose
# font libraries are built on it (the PS4's libSceFontFt).
find_package(Freetype REQUIRED)
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

# Logging (runtime/log.h).
add_library(recomp_log SHARED src/runtime/log.cpp)
target_include_directories(recomp_log PUBLIC src)
target_compile_features(recomp_log PUBLIC cxx_std_20)

# HLSL (D3DX 9's dialect) to GLSL (runtime/hlsl/hlsl.h); no HLE in it,
# also used by the recomp_hlslc dev tool.
add_library(recomp_hlsl SHARED
  src/runtime/hlsl/preprocess.cpp
  src/runtime/hlsl/parse.cpp
  src/runtime/hlsl/check.cpp
  src/runtime/hlsl/glsl.cpp
  src/runtime/hlsl/hlsl.cpp
  src/runtime/hlsl/bytecode.cpp)
target_include_directories(recomp_hlsl PUBLIC src)
target_compile_features(recomp_hlsl PUBLIC cxx_std_20)
add_subdirectory(tools/hlslc)

# gfx::Device and its backends; no HLE in it, shared by the graphics API
# modules below. Backends:
# - SDL_GPU (gfx/gpu_device.cpp): always; Vulkan, or Metal on macOS;
# - d3d9 through dxvk-native (gfx/d3d9_device.cpp): when DXVK_NATIVE_DIR
#   is set (Linux), with RECOMP_HAVE_D3D9 defined for the x86 runtime too.
add_library(recomp_gfx SHARED
  src/runtime/gfx/format.cpp
  src/runtime/gfx/device.cpp
  src/runtime/gfx/gpu_device.cpp
  src/runtime/gfx/shader_compile.cpp)
target_include_directories(recomp_gfx PUBLIC src)
target_compile_features(recomp_gfx PUBLIC cxx_std_20)
# Shaders translated at run time (Direct3D 9) are compiled then: glslang
# to SPIR-V, and SPIRV-Cross to MSL on macOS (gfx/shader_compile.cpp).
find_package(glslang CONFIG REQUIRED)
target_link_libraries(recomp_gfx PUBLIC SDL3::SDL3 recomp_hlsl
  PRIVATE recomp_log glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
if(APPLE)
  # SPIRV-Cross's per-module configs do not find their dependencies.
  find_package(spirv_cross_core CONFIG REQUIRED)
  find_package(spirv_cross_glsl CONFIG REQUIRED)
  find_package(spirv_cross_msl CONFIG REQUIRED)
  target_link_libraries(recomp_gfx PRIVATE spirv-cross-msl)
endif()
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
  # Only the d3d9 backend sees dxvk's Windows/D3D9 headers.
  set_source_files_properties(src/runtime/gfx/d3d9_device.cpp
    TARGET_DIRECTORY recomp_gfx PROPERTIES
    INCLUDE_DIRECTORIES "${DXVK_NATIVE_DIR}/include/dxvk"
    COMPILE_DEFINITIONS "DXVK_WSI_SDL3")
endif()

if(FFMPEG_FOUND)
  # The software mixer, its sources, the EAX model and the reverb; no HLE
  # in it.
  add_library(recomp_audio SHARED
    src/runtime/audio/source.cpp
    src/runtime/audio/mixer.cpp
    src/runtime/audio/reverb.cpp
    src/runtime/audio/digital_reverb.cpp
    src/runtime/audio/miles65.cpp
    src/runtime/audio/eax.cpp)
  target_include_directories(recomp_audio PUBLIC src)
  target_compile_features(recomp_audio PUBLIC cxx_std_20)
  target_link_libraries(recomp_audio PUBLIC SDL3::SDL3 PkgConfig::FFMPEG recomp_log)
  # Per-sample host paths: optimised in every build type.
  set_source_files_properties(src/runtime/audio/mixer.cpp
    src/runtime/audio/source.cpp TARGET_DIRECTORY recomp_audio
    PROPERTIES COMPILE_OPTIONS "-O2")
  # The digital reverb and Miles 6.5's levels round like the original
  # only without contraction into FMA.
  set_source_files_properties(src/runtime/audio/digital_reverb.cpp
    src/runtime/audio/miles65.cpp TARGET_DIRECTORY recomp_audio
    PROPERTIES COMPILE_OPTIONS "-O2;-ffp-contract=off")
endif()

# LLVM's demanglers (llvm/Demangle/Demangle.h), static.
find_library(RECOMP_LLVM_DEMANGLE libLLVMDemangle.a
  PATHS "${RECOMP_LLVM_ROOT}/lib64" "${RECOMP_LLVM_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
add_library(recomp_demangle INTERFACE)
target_link_libraries(recomp_demangle INTERFACE "${RECOMP_LLVM_DEMANGLE}")
target_include_directories(recomp_demangle INTERFACE "${RECOMP_LLVM_ROOT}/include")
