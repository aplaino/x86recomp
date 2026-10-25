# recomp_lift_a64: AArch64 -> C++ lifter (build-time only, never linked into
# a game). It has no executable format of its own: each AArch64 platform
# module adds its image reader (program.h, ImageReader) to it.
add_executable(recomp_lift_a64
  a64.cpp
  emit.cpp
  main.cpp
  program.cpp)
target_include_directories(recomp_lift_a64 PRIVATE ${RECOMP_ROOT}/src)
target_compile_features(recomp_lift_a64 PRIVATE cxx_std_20)
