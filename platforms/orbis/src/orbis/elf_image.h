#pragma once
// Maps a decrypted PS4 ELF into the guest address space as the Orbis
// loader lays it out: its loadable segments (PT_LOAD, PT_SCE_RELRO) at
// base + p_vaddr, with the SCE relocation table applied. Executables are
// ET_SCE_EXEC (linked at their address) or ET_SCE_DYNEXEC (position
// independent, mapped at kExecBase); PRXs are ET_SCE_DYNAMIC, at a base
// the caller picks. The file must be the ELF itself: signed SELF
// containers are not read.
//
// The dynamic linking data sits in PT_SCE_DYNLIBDATA, outside the mapped
// image; the DT_SCE_* tags of PT_DYNAMIC give offsets into it. Symbols
// there are named "NID#library#module": the NID is a hash of the symbol's
// name (orbis/nid.h), and library and module are ids that
// DT_SCE_IMPORT_LIB / DT_SCE_NEEDED_MODULE (imports) and
// DT_SCE_EXPORT_LIB / DT_SCE_MODULE_INFO (exports) name.
//
// Imports are bound like PE ones: without a binder every imported symbol
// gets its own address in the core's x64 thunk region, which the
// runtime's dispatcher routes to HLE handlers.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "runtime/guest_memory.h"

namespace recomp::orbis {

// Where the Orbis loader maps a position-independent executable.
inline constexpr gaddr kExecBase = 0x400000;

struct ElfSegment {
  gaddr va;
  uint64_t size;   // in memory
  uint32_t flags;  // PF_X 1, PF_W 2, PF_R 4
};

struct ElfImport {
  std::string nid, library, module;
  uint8_t type;  // STT_*: 1 object, 2 function
  gaddr value;   // the address it is bound to
  std::vector<gaddr> slots;  // words relocated to exactly that address
  gaddr plt = 0;             // its PLT stub (jmp [rip+slot]), 0 if none
};

struct ElfExport {
  std::string nid, library, module;
  uint8_t type;
  gaddr va;
};

// An .eh_frame FDE's code range. FDEs of code the linker discarded (a
// pc_begin of 0) are left out.
struct ElfFunction {
  gaddr begin, end;  // [begin, end)
};

// PT_TLS, and the module's place in the static TLS layout (FreeBSD's
// variant II: blocks below the thread pointer): its index in the DTV and
// the distance from its block's start up to the thread pointer.
struct ElfTls {
  gaddr init_va = 0;  // template for each thread's block
  uint64_t init_size = 0, mem_size = 0, align = 0;
  uint64_t module = 0, offset = 0;
};

using ElfImportBinder = std::function<gaddr(const ElfImport&)>;

struct ElfImage {
  uint16_t type = 0;      // ET_SCE_EXEC 0xFE00, ET_SCE_DYNEXEC 0xFE10, ET_SCE_DYNAMIC 0xFE18
  gaddr image_base = 0;   // what p_vaddr 0 maps to
  gaddr link_base = 0;    // the lowest p_vaddr
  gaddr entry = 0;
  gaddr init = 0, fini = 0;  // DT_INIT / DT_FINI, 0 if absent
  gaddr proc_param = 0;      // PT_SCE_PROCPARAM (an eboot's), 0 if absent
  std::vector<ElfSegment> segments;
  // The code: the executable segment up to its last .eh_frame function or
  // PLT stub (the segment also holds .rodata and .eh_frame); and the
  // read-only data, there and in read-only segments (set by Link).
  gaddr text_lo = 0, text_hi = 0;
  std::vector<std::pair<gaddr, gaddr>> rodata;
  std::vector<ElfImport> imports;
  std::vector<ElfExport> exports;
  std::vector<ElfFunction> functions;  // in .eh_frame order
  ElfTls tls;
  bool has_tls = false;

  // True if the file is a little-endian x86-64 ELF.
  static bool IsElfFile(const std::string& path);

  // Map reads the file and maps it into mem at `base` (0: the link
  // address, or kExecBase for ET_SCE_DYNEXEC; a PRX needs one), and reads
  // its imports (not yet bound) and exports. It fails if any page of the
  // range is already committed. Link then binds the imports, applies the
  // relocations and reads .eh_frame. In between, a loader reads other
  // modules' exports and gives the module its TLS slot (tls.module and
  // tls.offset; Map sets a main executable's: module 1, its block right
  // below the thread pointer). Load does both. Each returns false and
  // fills err on failure.
  bool Map(const std::string& path, GuestMemory& mem, std::string* err, gaddr base = 0);
  bool Link(GuestMemory& mem, std::string* err, const ElfImportBinder& bind = nullptr);
  bool Load(const std::string& path, GuestMemory& mem, std::string* err,
            gaddr base = 0, const ElfImportBinder& bind = nullptr);

 private:
  struct Ph {
    uint32_t type, flags;
    uint64_t off, va, filesz, memsz, align;
  };
  struct Sym {
    uint8_t info;
    uint16_t shndx;
    uint64_t value;
    int import = -1;  // index into imports
  };
  std::vector<uint8_t> file_;
  std::vector<Ph> phs_;
  std::vector<Sym> syms_;
  size_t rela_ = 0, rela_size_ = 0, jmprel_ = 0, jmprel_size_ = 0;
  gaddr map_lo_ = 0, map_hi_ = 0;
};

}  // namespace recomp::orbis
