// recomp_lift_ppc's reader for its own format, ELF (elf.h): function starts
// from the entry point and the sized function symbols.

#include "elf.h"
#include "program.h"

namespace lifter::ppc {
namespace {

bool Load(Image& img, const std::string& path, std::string& err) {
  ElfImage e;
  if (!LoadElf(path, e, err)) return false;
  img.base = e.base;
  img.code = e.code;
  if (e.entry) img.AddStart(e.entry);
  for (const auto& [va, size] : e.functions) img.AddStart(va, size);
  img.bytes = std::move(e.bytes);
  return true;
}

const bool kRegistered = (RegisterImageReader({IsElf, Load}), true);

}  // namespace
}  // namespace lifter::ppc
