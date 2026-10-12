#pragma once
// Maps a PE32 image (the game EXE) into the guest address space at its
// preferred base, exactly as the Windows loader would lay it out. The image
// has no relocations, so the preferred base is the only option.
//
// Imports are not bound to DLLs. Each IAT slot is pointed at a unique thunk
// address in a reserved guest region; the runtime's indirect-call dispatcher
// routes calls to those addresses to host HLE handlers.

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/guest_memory.h"

namespace recomp {

// Guest layout constants. Everything stays below 0x80000000 because the
// game is not large-address-aware and may treat pointers as signed.
inline constexpr gaddr kThunkBase = 0x7F000000;
inline constexpr uint32_t kThunkStride = 0x10;
inline constexpr uint32_t kThunkRegionSize = 0x00100000;

struct PeSection {
  std::string name;
  gaddr va;
  uint32_t vsize;
  uint32_t characteristics;
};

struct PeImport {
  std::string dll;
  std::string name;  // "#123" for ordinal imports
  gaddr iat_va;      // IAT slot the game reads
  gaddr thunk_va;    // value stored in that slot
};

struct PeTls {
  gaddr raw_start = 0, raw_end = 0;  // template for each thread's block
  gaddr index_va = 0;                // where the loader stores the TLS index
  uint32_t zero_fill = 0;
};

struct PeImage {
  gaddr image_base = 0;
  uint32_t image_size = 0;
  gaddr entry = 0;
  uint32_t stack_reserve = 0;
  std::vector<PeSection> sections;
  std::vector<PeImport> imports;
  PeTls tls;
  bool has_tls = false;

  // Reads the file and maps headers and sections into mem. Returns false
  // and fills err on failure.
  bool Load(const std::string& path, GuestMemory& mem, std::string* err);
};

}  // namespace recomp
