// recomp_lift_a64's reader for NROs (horizon/nso_image.h), loaded and
// relocated at kMainBase.

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
  recomp::a64::Module m;
  if (!LoadNro(path, m, err) || !recomp::a64::Relocate(m, kMainBase, err)) return false;
  ImageFromModule(img, std::move(m), kMainBase);
  return true;
}

const bool kRegistered = (RegisterImageReader({Match, Load}), true);

}  // namespace
}  // namespace lifter::a64
