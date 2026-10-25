# The Nintendo Switch (Horizon OS) platform module (ARCHITECTURE.md,
# "Console platforms"): built only with RECOMP_PLATFORM_HORIZON, on the
# AArch64 lifter and CPU model (recomp_lift_a64, recomp_a64).

# NRO mapping and relocation, for the lifter and the runtime.
add_library(recomp_horizon_image SHARED src/horizon/nso_image.cpp)
target_include_directories(recomp_horizon_image PUBLIC src ${RECOMP_ROOT}/src)
target_compile_features(recomp_horizon_image PUBLIC cxx_std_20)

# The process loader (horizon/process.h) and the kernel's supervisor calls.
add_library(recomp_horizon SHARED src/horizon/process.cpp src/horizon/svc.cpp)
target_link_libraries(recomp_horizon PUBLIC recomp_a64 recomp_horizon_image)

# recomp_lift_a64 reads NROs.
target_sources(recomp_lift_a64 PRIVATE lifter/nro_reader.cpp)
target_include_directories(recomp_lift_a64 PRIVATE ${RECOMP_ROOT}/tools/lifter_a64)
target_link_libraries(recomp_lift_a64 PRIVATE recomp_horizon_image)

# horizon_elf2nro: builds the test fixtures' NROs from their ELF links.
add_executable(horizon_elf2nro tools/elf2nro.cpp)

add_subdirectory(tests)
