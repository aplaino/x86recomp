// recomp_lift_a64's own reader: static-PIE AArch64 ELF (a64/image.h),
// loaded and relocated at kElfBase. The core's tests lift their fixture
// through it, without any platform module.

#include <cstring>
#include <fstream>

#include "a64/image.h"
#include "program.h"

namespace lifter::a64 {
namespace {

using namespace recomp::a64;

bool Match(const std::string& path) {
  char head[20] = {};
  std::ifstream f(path, std::ios::binary);
  return f.read(head, sizeof head) && !std::memcmp(head, "\x7f" "ELF", 4) && head[18] == char(183);
}

bool Load(Image& img, const std::string& path, std::string& err) {
  Module m;
  if (!LoadElf(path, m, err) || !Relocate(m, kElfBase, err)) return false;
  ImageFromModule(img, std::move(m), kElfBase);
  return true;
}

const bool kRegistered = (RegisterImageReader({Match, Load}), true);

}  // namespace
}  // namespace lifter::a64
