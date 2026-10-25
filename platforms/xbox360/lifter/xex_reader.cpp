// recomp_lift_ppc's reader for XEX2 (xbox360/xex.h), at the image's load
// address: function starts from the entry point, .pdata and the exports by
// ordinal; import thunks become calls to the platform's Import().

#include "program.h"
#include "xbox360/xex.h"

namespace lifter::ppc {
namespace {

using namespace recomp::xbox360;

bool Load(Image& img, const std::string& path, std::string& err) {
  XexModule m;
  if (!LoadXex(path, m, err)) return false;
  img.base = m.base;
  for (const XexSection& s : m.sections)
    if (s.code && s.size) img.code.emplace_back(s.va, uint64_t(s.va) + s.size);
  if (m.entry) img.AddStart(m.entry);
  for (const XexFunction& f : m.functions) img.AddStart(f.va, f.size);
  // Exports that are code (system modules also export variables).
  for (uint32_t va : m.exports)
    for (const XexSection& s : m.sections)
      if (s.code && va >= s.va && va - s.va < s.size) img.AddStart(va);
  for (const XexImport& i : m.imports)
    if (i.thunk) img.imports[i.va] = {i.module, i.ordinal};
  img.bytes = std::move(m.image);
  return true;
}

const bool kRegistered = (RegisterImageReader({IsXex, Load}), true);

}  // namespace
}  // namespace lifter::ppc
