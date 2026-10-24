# The PS4 (Orbis OS) platform module (ARCHITECTURE.md, "Console
# platforms"): built only with RECOMP_PLATFORM_ORBIS.

# Orbis ELF mapping and NIDs, for the lifter and the runtime.
add_library(recomp_orbis_image SHARED
  src/orbis/elf_image.cpp
  src/orbis/nid.cpp)
target_include_directories(recomp_orbis_image PUBLIC src)
target_link_libraries(recomp_orbis_image PUBLIC recomp_image)

# recomp_lift reads Orbis ELFs.
target_sources(recomp_lift PRIVATE lifter/orbis_reader.cpp)
target_include_directories(recomp_lift PRIVATE ${RECOMP_ROOT}/tools/lifter)
target_link_libraries(recomp_lift PRIVATE recomp_orbis_image)

add_executable(orbis_image_test tests/orbis_image_test.cpp)
target_link_libraries(orbis_image_test PRIVATE recomp_orbis_image)
add_test(NAME orbis_image COMMAND orbis_image_test)
