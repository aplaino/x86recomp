#pragma once
// High-level emulation of the system modules' exports (ARCHITECTURE.md,
// "Runtime: HLE"): host functions registered by module and ordinal, which
// the lifted import thunks call through Import(). A handler reads its
// arguments with Arg(c, i) (r3-r10, then the caller's parameter area) and
// returns the value for r3. An import without a handler is fatal when it
// is called and names the export; there are no default stubs. Variable
// exports are guest memory the runtime allocates and fills, and every one
// a module imports must be defined when it is loaded.

#include <cstdint>
#include <cstring>
#include <string>

#include "ppc/ctx.h"
#include "ppc/lifted.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {

struct XexModule;

using HleFn = uint64_t (*)(ppc::Ctx& c);

struct Registrar {
  Registrar(const char* module, uint32_t ordinal, const char* name, HleFn fn);
};
// A variable export: `size` bytes of guest memory, zero or set by `init`,
// allocated when a module first imports it.
struct VariableRegistrar {
  VariableRegistrar(const char* module, uint32_t ordinal, const char* name, uint32_t size,
                    void (*init)(uint32_t va));
};

// Writes the import slots of a module being loaded: a function import's
// slot gets its thunk's address (so a call through the slot reaches the
// same handler), a variable import's the variable's. Fatal, naming each
// one, if a variable import has no definition.
void BindImports(const XexModule& m);
// The guest address of variable export module!ordinal (allocated on first
// use), 0 if none is registered.
uint32_t VariableAddress(const char* module, uint32_t ordinal);
// "module!Name" or "module!ordinal" for diagnostics.
std::string ExportName(const char* module, uint32_t ordinal);

// ---- guest memory, big-endian -------------------------------------------
inline uint8_t Rd8(uint32_t va) { return Mem()[va]; }
inline uint16_t Rd16(uint32_t va) { return ppc::lift::Ld<uint16_t>(Mem(), va); }
inline uint32_t Rd32(uint32_t va) { return ppc::lift::Ld<uint32_t>(Mem(), va); }
inline uint64_t Rd64(uint32_t va) { return ppc::lift::Ld<uint64_t>(Mem(), va); }
inline void Wr8(uint32_t va, uint8_t v) { Mem()[va] = v; }
inline void Wr16(uint32_t va, uint16_t v) { ppc::lift::St<uint16_t>(Mem(), va, v); }
inline void Wr32(uint32_t va, uint32_t v) { ppc::lift::St<uint32_t>(Mem(), va, v); }
inline void Wr64(uint32_t va, uint64_t v) { ppc::lift::St<uint64_t>(Mem(), va, v); }
// A NUL-terminated guest string (of bytes; of big-endian UTF-16 units).
std::string ReadString(uint32_t va);
std::u16string ReadString16(uint32_t va);
// The characters of an ANSI_STRING (Length, MaximumLength, Buffer).
std::string ReadAnsiString(uint32_t ansi_string);

// Argument i of a call into a handler: r3-r10, then the caller's parameter
// area, 8 bytes per argument from r1 + 0x50 (32-bit values in their low
// word, at r1 + 0x54 + 8 * (i - 8)).
inline uint64_t Arg64(const ppc::Ctx& c, int i) {
  if (i < 8) return c.r[3 + i];
  return Rd64(uint32_t(c.r[1]) + 0x50 + 8 * uint32_t(i - 8));
}
inline uint32_t Arg(const ppc::Ctx& c, int i) { return uint32_t(Arg64(c, i)); }

}  // namespace recomp::xbox360

#define RECOMP_XHLE_CAT2(a, b) a##b
#define RECOMP_XHLE_CAT(a, b) RECOMP_XHLE_CAT2(a, b)

// HLE("xboxkrnl.exe", 300, RtlInitAnsiString) { ... return r3; }
#define HLE(module, ordinal, name)                                                     \
  static uint64_t RECOMP_XHLE_CAT(hle_, name)(::recomp::ppc::Ctx & c);                 \
  static const ::recomp::xbox360::Registrar RECOMP_XHLE_CAT(reg_, name){              \
      module, ordinal, #name, &RECOMP_XHLE_CAT(hle_, name)};                           \
  static uint64_t RECOMP_XHLE_CAT(hle_, name)([[maybe_unused]] ::recomp::ppc::Ctx & c)

// HLE_VARIABLE("xboxkrnl.exe", 344, XboxKrnlVersion, 8, init): init(va) or nullptr.
#define HLE_VARIABLE(module, ordinal, name, size, init)                                \
  static const ::recomp::xbox360::VariableRegistrar RECOMP_XHLE_CAT(var_, name){      \
      module, ordinal, #name, size, init}
