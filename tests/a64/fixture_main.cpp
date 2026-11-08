// The lifted test fixture: a64_fixture <fixture.elf>. Loads it as the
// oracle does (at kElfBase, the main thread's stack below it) and runs it
// from its entry point.
#include <cstdio>
#include <cstring>
#include <string>

#include "a64/ctx.h"
#include "a64/dispatch.h"
#include "a64/fatal.h"
#include "a64/image.h"
#include "a64/platform.h"

namespace {

// Repeated in oracle/oracle.py.
constexpr uint64_t kStackBase = 0x7000000000ull, kStackSize = 0x100000;

}  // namespace

int main(int argc, char** argv) {
  using namespace recomp::a64;
  if (argc != 2) {
    std::fprintf(stderr, "usage: a64_fixture <fixture.elf>\n");
    return 2;
  }
  Module m;
  std::string err;
  if (!LoadElf(argv[1], m, err) || !Relocate(m, kElfBase, err)) Fatal(nullptr, "%s", err.c_str());
  if (TextHash(m) != LiftedTextHash())
    Fatal(nullptr, "%s is not the module this program was lifted from", argv[1]);
  if (!platform::MapFixed(kElfBase, m.image.size()) || !platform::MapFixed(kStackBase, kStackSize))
    Fatal(nullptr, "cannot map the fixture or its stack");
  std::memcpy(reinterpret_cast<void*>(kElfBase), m.image.data(), m.image.size());
  Ctx c{};
  c.sp = kStackBase + kStackSize;
  Dispatch(c, kElfBase + m.text.off, 0);
  return 0;
}
