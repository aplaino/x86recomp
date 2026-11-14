# Dev-only tests of the base; nothing here ships. The x86 core's are in
# x86/, a platform's in platforms/<name>/tests.

# digital_reverb_test: the Miles-compatible digital reverb against a hash
# of its own output recorded when it matched the original
# (recomp_add_digital_reverb_test compares with the original itself);
# portable.
add_executable(digital_reverb_test digital_reverb_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/audio/digital_reverb.cpp)
target_include_directories(digital_reverb_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(digital_reverb_test PRIVATE cxx_std_20)
target_compile_options(digital_reverb_test PRIVATE -O2 -ffp-contract=off)
add_test(NAME digital_reverb COMMAND digital_reverb_test)

# fast2d_test: Miles 6.5's sample levels and Fast 2D against a hash of
# their own output recorded when they matched the original
# (recomp_add_fast2d_test compares with the original itself); portable.
add_executable(fast2d_test fast2d_test.cpp ${PROJECT_SOURCE_DIR}/src/runtime/audio/miles65.cpp)
target_include_directories(fast2d_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(fast2d_test PRIVATE cxx_std_20)
target_compile_options(fast2d_test PRIVATE -O2 -ffp-contract=off)
add_test(NAME fast2d COMMAND fast2d_test)

# reverb_test: the EAX environment reverb's network stays stable at its
# parameters' limits; portable.
add_executable(reverb_test reverb_test.cpp ${PROJECT_SOURCE_DIR}/src/runtime/audio/reverb.cpp)
target_include_directories(reverb_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(reverb_test PRIVATE cxx_std_20)
target_compile_options(reverb_test PRIVATE -O2)
add_test(NAME reverb COMMAND reverb_test)

# gl_draw_test: the GL context's vertex arrays and immediate mode against
# a reference model of GL's conversions, through a recording device;
# portable.
add_executable(gl_draw_test gl_draw_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/gl/context.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/gfx/format.cpp)
target_include_directories(gl_draw_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(gl_draw_test PRIVATE cxx_std_20)
target_compile_options(gl_draw_test PRIVATE -O2)
add_test(NAME gl_draw COMMAND gl_draw_test)

# pacing_test: FramePacer's rate and that it waits without spinning;
# portable. Timing-sensitive, so it runs alone.
add_executable(pacing_test pacing_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/pacing.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/platform.cpp)
target_include_directories(pacing_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(pacing_test PRIVATE cxx_std_20)
add_test(NAME pacing COMMAND pacing_test)
set_tests_properties(pacing PROPERTIES RUN_SERIAL TRUE)

# platform_fault_test: the platform layer's fault contexts and
# RedirectToCall on real faults, built as code that faults on guest memory
# is on hosts that need RedirectToCall; portable.
add_executable(platform_fault_test platform_fault_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/platform.cpp)
target_include_directories(platform_fault_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(platform_fault_test PRIVATE cxx_std_20)
set_source_files_properties(platform_fault_test.cpp PROPERTIES COMPILE_OPTIONS
  "-fnon-call-exceptions;-fno-omit-frame-pointer;-mno-omit-leaf-frame-pointer;-mno-red-zone")
add_test(NAME platform_fault COMMAND platform_fault_test)

# lazy_mem_test: lazily read-back images (DirectDraw Lock of GPU
# surfaces) through real page faults; portable.
add_executable(lazy_mem_test lazy_mem_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/lazy_mem.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_memory.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/write_watch.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_alloc.cpp)
target_include_directories(lazy_mem_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(lazy_mem_test PRIVATE cxx_std_20)
target_link_libraries(lazy_mem_test PRIVATE Threads::Threads)
add_test(NAME lazy_mem COMMAND lazy_mem_test)

# guest_alloc_test: reservations, decommits and heap sizes at their
# edges; portable.
add_executable(guest_alloc_test guest_alloc_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_alloc.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_memory.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/write_watch.cpp)
target_include_directories(guest_alloc_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(guest_alloc_test PRIVATE cxx_std_20)
target_link_libraries(guest_alloc_test PRIVATE Threads::Threads)
add_test(NAME guest_alloc COMMAND guest_alloc_test)

# audio_source_test: raw sample decoding; needs the audio library.
if(TARGET recomp_audio)
  add_executable(audio_source_test audio_source_test.cpp)
  target_link_libraries(audio_source_test PRIVATE recomp_audio)
  add_test(NAME audio_source COMMAND audio_source_test)
endif()

# write_watch_test: write watches through real page faults; portable.
add_executable(write_watch_test write_watch_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/write_watch.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_memory.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_alloc.cpp)
target_include_directories(write_watch_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(write_watch_test PRIVATE cxx_std_20)
target_link_libraries(write_watch_test PRIVATE Threads::Threads)
add_test(NAME write_watch COMMAND write_watch_test)

# HLSL translator: each shader in hlsl/ through recomp_hlslc, then
# glslangValidator (the GLSL must compile for Vulkan), with the reflection
# (constant table, inputs, varyings, samplers) compared with its
# <name>.reflect; error.hlsl must fail.
function(recomp_hlsl_test name src stage)  # ARGN: recomp_hlslc arguments
  string(REPLACE ";" "|" _args "${ARGN}")
  add_test(NAME hlsl_${name} COMMAND "${CMAKE_COMMAND}"
           -DHLSLC=$<TARGET_FILE:recomp_hlslc> -DGLSLANG=${RECOMP_GLSLANG}
           -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/hlsl/${src}.hlsl -DSTAGE=${stage}
           -DARGS=${_args} -DPRELUDE=${CMAKE_CURRENT_SOURCE_DIR}/hlsl/prelude.glsl
           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/hlsl_${name}.${stage}
           -DEXPECT=${CMAKE_CURRENT_SOURCE_DIR}/hlsl/${name}.reflect
           -P ${CMAKE_CURRENT_SOURCE_DIR}/hlsl/run.cmake)
endfunction()
recomp_hlsl_test(vs_features vs_features vert -T vs_3_0 -E main)
recomp_hlsl_test(ps_features ps_features frag -T ps_3_0 -E main)
recomp_hlsl_test(preprocess preprocess frag -T ps_2_0 -E main -D FROM_COMMAND_LINE=0.5)
recomp_hlsl_test(sm2_vs sm2 vert -T vs_2_0 -E vs)
recomp_hlsl_test(sm2_ps sm2 frag -T ps_2_0 -E ps)
add_test(NAME hlsl_error COMMAND recomp_hlslc ${CMAKE_CURRENT_SOURCE_DIR}/hlsl/error.hlsl
         -T ps_3_0 -E main)
set_tests_properties(hlsl_error PROPERTIES WILL_FAIL TRUE)

# The front end on sources it must translate or refuse (without a crash).
add_executable(hlsl_frontend_test hlsl_frontend_test.cpp)
target_link_libraries(hlsl_frontend_test PRIVATE recomp_hlsl)
add_test(NAME hlsl_frontend COMMAND hlsl_frontend_test)

# The bytecode translator on hand-assembled shaders (shader models
# 1.1-3.0); each translation must compile with glslangValidator.
add_executable(hlsl_bytecode_test hlsl_bytecode_test.cpp)
target_link_libraries(hlsl_bytecode_test PRIVATE recomp_hlsl)
add_test(NAME hlsl_bytecode COMMAND hlsl_bytecode_test ${RECOMP_GLSLANG}
         ${CMAKE_CURRENT_SOURCE_DIR}/hlsl/prelude.glsl ${CMAKE_CURRENT_BINARY_DIR})

# gfx_program_test: gfx::Device's programs (the console GPUs' path) on
# SDL_GPU; needs a Vulkan device (lavapipe will do), skipped without one.
if(RECOMP_HOST)
  add_executable(gfx_program_test gfx_program_test.cpp)
  target_link_libraries(gfx_program_test PRIVATE recomp_gfx recomp_log)
  add_test(NAME gfx_program COMMAND gfx_program_test)
  set_tests_properties(gfx_program PROPERTIES SKIP_RETURN_CODE 77
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_GPU_DRIVER=vulkan")

# gfx_ff_test: explicit fixed-function draws (DrawFf) against the
# stateful ones on SDL_GPU; skipped without a Vulkan device.
  add_executable(gfx_ff_test gfx_ff_test.cpp)
  target_link_libraries(gfx_ff_test PRIVATE recomp_gfx recomp_log)
  add_test(NAME gfx_ff COMMAND gfx_ff_test)
  set_tests_properties(gfx_ff PROPERTIES SKIP_RETURN_CODE 77
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_GPU_DRIVER=vulkan")
endif()
