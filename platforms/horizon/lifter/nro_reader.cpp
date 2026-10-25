// recomp_lift_a64's reader for NROs (horizon/nso_image.h), loaded and
// relocated at kMainBase: function starts from the entry point (the start
// of .text), .eh_frame FDEs, sized function symbols and relative
// relocations that point into .text.

#include <cstring>
#include <fstream>

#include "horizon/layout.h"
#include "horizon/nso_image.h"
#include "program.h"

namespace lifter::a64 {
namespace {

using namespace recomp::horizon;

bool Match(const std::string& path) {
  char head[0x14] = {};
  std::ifstream f(path, std::ios::binary);
  return f.read(head, sizeof head) && !std::memcmp(head + 0x10, "NRO0", 4);
}

bool Load(Image& img, const std::string& path, std::string& err) {
  Module m;
  if (!LoadNro(path, m, err) || !Relocate(m, kMainBase, err)) return false;
  img.base = kMainBase;
  img.text_lo = kMainBase + m.text.off;
  img.text_hi = img.text_lo + m.text.size;
  img.AddStart(img.text_lo);
  for (const FdeRange& r : EhFrameFunctions(m)) img.AddStart(kMainBase + r.start, r.size);
  for (const Symbol& s : m.symbols) {
    if (!s.defined || !s.func) continue;
    img.names.emplace(kMainBase + s.value, s.name);
    img.AddStart(kMainBase + s.value, s.size);
  }
  for (const Reloc& r : m.relocs) {
    uint64_t t = kMainBase + uint64_t(r.addend);
    if (r.type == kRelocAarch64Relative && t >= img.text_lo && t < img.text_hi && t % 4 == 0)
      img.AddStart(t);
  }
  img.bytes = std::move(m.image);
  return true;
}

const bool kRegistered = (RegisterImageReader({Match, Load}), true);

}  // namespace
}  // namespace lifter::a64
