# recomp_lift_ppc: PowerPC -> C++ lifter (build-time only, never linked into
# a game). It has no executable format of its own: each PowerPC platform
# module adds its image reader (program.h, ImageReader) to it.
add_executable(recomp_lift_ppc
  emit.cpp
  main.cpp
  ppc.cpp
  program.cpp)
target_include_directories(recomp_lift_ppc PRIVATE ${RECOMP_ROOT}/src)
target_compile_features(recomp_lift_ppc PRIVATE cxx_std_20)
recomp_bundle(recomp_lift_ppc)
