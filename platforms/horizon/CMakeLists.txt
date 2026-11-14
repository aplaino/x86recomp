# The Nintendo Switch (Horizon OS) platform module (ARCHITECTURE.md,
# "Layout"): built only with RECOMP_PLATFORM_HORIZON, on the AArch64 lifter
# and CPU model (recomp_lift_a64, recomp_a64).

# NRO mapping and relocation, for the lifter and the runtime.
add_library(recomp_horizon_image SHARED src/horizon/nso_image.cpp)
target_include_directories(recomp_horizon_image PUBLIC src)
target_link_libraries(recomp_horizon_image PUBLIC recomp_a64_image)

# The process loader (horizon/process.h) and the kernel's supervisor calls.
add_library(recomp_horizon SHARED src/horizon/process.cpp src/horizon/svc.cpp)
target_link_libraries(recomp_horizon PUBLIC recomp_a64 recomp_horizon_image)

# recomp_lift_a64 reads NROs.
target_sources(recomp_lift_a64 PRIVATE lifter/nro_reader.cpp)
target_include_directories(recomp_lift_a64 PRIVATE ${RECOMP_ROOT}/tools/lifter_a64)
target_link_libraries(recomp_lift_a64 PRIVATE recomp_horizon_image)

# horizon_elf2nro: builds the test fixture's NRO from its ELF link.
add_executable(horizon_elf2nro tools/elf2nro.cpp)

# horizon_nro_test: the NRO reader refuses headers that do not describe an
# image (tests/nro_test.cpp); it needs no fixture or oracle.
add_executable(horizon_nro_test tests/nro_test.cpp)
target_link_libraries(horizon_nro_test PRIVATE recomp_horizon_image)
add_test(NAME horizon_nro_test COMMAND horizon_nro_test "${CMAKE_CURRENT_BINARY_DIR}/nro_test.nro")

add_subdirectory(tests)
