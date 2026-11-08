# recomp_lift_a64: AArch64 -> C++ lifter (build-time only, never linked into
# a game). It reads static-PIE ELF itself (elf_reader.cpp, a64/image.h);
# each AArch64 platform module adds the image reader of its own formats
# (program.h, ImageReader) to it.
add_executable(recomp_lift_a64
  a64.cpp
  elf_reader.cpp
  emit.cpp
  main.cpp
  program.cpp)
target_include_directories(recomp_lift_a64 PRIVATE ${RECOMP_ROOT}/src)
target_compile_features(recomp_lift_a64 PRIVATE cxx_std_20)
target_link_libraries(recomp_lift_a64 PRIVATE recomp_a64_image)
