// recomp_lift_ppc's reader for XEX2 (xbox360/xex.h), at the image's load
// address: function starts from the entry point, .pdata and the exports by
// ordinal; import thunks become calls to the platform's Import(). Its
// code is the Xenon's. From .pdata too: each function's prologue, and the
// functions with an exception handler, which an unwind may continue in;
// for __C_specific_handler's, their __except blocks (its scope table).

#include "program.h"
#include "xbox360/xex.h"

namespace lifter::ppc {
namespace {

using namespace recomp::xbox360;

bool Load(Image& img, const std::string& path, std::string& err) {
  XexModule m;
  if (!LoadXex(path, m, err)) return false;
  img.base = m.base;
  img.cpu = Cpu::kXenon;
  for (const XexSection& s : m.sections)
    if (s.code && s.size) img.code.emplace_back(s.va, uint64_t(s.va) + s.size);
  if (m.entry) img.AddStart(m.entry);
  for (const XexFunction& f : m.functions) {
    img.AddStart(f.va, f.size);
    if (f.described) img.prologues.emplace(f.va, f.prologue);
    if (f.handler) img.resumable.insert(f.va);
  }
  // Exports that are code (system modules also export variables).
  for (uint32_t va : m.exports)
    for (const XexSection& s : m.sections)
      if (s.code && va >= s.va && va - s.va < s.size) img.AddStart(va);
  uint32_t c_specific = 0;
  for (const XexImport& i : m.imports)
    if (i.thunk) {
      img.imports[i.va] = {i.module, i.ordinal};
      if (i.module == "xboxkrnl.exe" && i.ordinal == 421) c_specific = i.va;
    }
  // __C_specific_handler's data: a count, then per scope its range, its
  // filter (or __finally block) and its __except block (0 for __finally).
  auto word = [&](uint32_t va) -> uint32_t {
    uint32_t off = va - m.base;
    if (va < m.base || uint64_t(off) + 4 > m.image.size()) return 0;
    return uint32_t(m.image[off]) << 24 | m.image[off + 1] << 16 | m.image[off + 2] << 8 | m.image[off + 3];
  };
  for (const XexFunction& f : m.functions) {
    if (!f.handler || !c_specific || word(f.va - 8) != c_specific) continue;
    uint32_t scopes = word(f.va - 4);
    for (uint32_t n = word(scopes), k = 0; k < n && k < 256; k++)
      if (uint32_t target = word(scopes + 4 + 16 * k + 12)) img.resume_targets.insert(target);
  }
  img.bytes = std::move(m.image);
  return true;
}

const bool kRegistered = (RegisterImageReader({IsXex, Load}), true);

}  // namespace
}  // namespace lifter::ppc
