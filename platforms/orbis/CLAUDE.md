# The PS4 (Orbis OS) platform module

Rules only, on top of the repository's CLAUDE.md. Design in the
top-level ARCHITECTURE.md ("Console platforms"); the system libraries'
HLE choices and the values not measured on hardware in docs/hle.md.

## Build and test
- `cmake --preset orbis && cmake --build --preset orbis && ctest --test-dir out/build/orbis`
  (the x86 path with RECOMP_PLATFORM_ORBIS on). orbis_process_test
  needs clang and ld.lld for its FreeBSD x86-64 test programs.
- Changes to the x86 core (lifter, lifted.h, runtime) also go through
  the PC games' validation (the repository's CLAUDE.md).

## Constraints
- Host services come from the shared host layer (cmake/host.cmake):
  the GPU front end through gfx::Device, audio through the software
  mixer, windows and input through SDL3, movies through ffmpeg, fonts
  through FreeType; never
  from an HLE file straight to a host graphics or audio API, and no
  host library outside the layer.
- System libraries are reimplemented (HLE), never lifted from firmware
  or shipped. The owner's firmware dump (decrypted system modules) is a
  behaviour reference and a local oracle only: its code, tables and
  strings are not copied; behaviour taken from it is described in the
  code and docs with the firmware version (11.00).
- GPL sources (shadPS4 and its NID lists among them) are references
  only, never copied. The NID name lists stay local files
  (<ID>_NID_NAMES), never in the repository.

## Rules
- Every import needs an explicit handler, registered by name
  (ORBIS_HLE), by mangled name (ORBIS_HLE_NAMED) or, for exports known
  only by NID, by NID (ORBIS_HLE_NID). An unhandled one is fatal by
  design; a path not implemented yet is fatal and says so, never a
  guess.
- Argument checks, error codes, structure layouts and handle numbering
  follow the firmware; where they come from a system service rather
  than the library (the service's answers), docs/hle.md says what the
  HLE models (a console offline, a user not signed in, a game installed
  in full).
- HLE source files belong to the matching recomp_orbis_* shared library
  in CMakeLists.txt (here).
- Guest objects shared with host threads (queues, condition variables
  waited on until exit) are never destroyed at exit.
