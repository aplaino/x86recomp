// The lifted fixture (fixture/), run against what the same program printed
// bare metal on the oracle (oracle/qemu_run.py).
//   xbox360_fixture <fixture.xex> <oracle-output.bin>
// The XEX image is loaded into a 4 GiB arena at its address and run from
// its entry point in 32-bit mode, as the oracle runs it; the program ends
// with `sc` (oracle/crt0.S), which here compares its output.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "arena.h"
#include "oracle.h"
#include "ppc/dispatch.h"
#include "ppc/fatal.h"
#include "xbox360/xex.h"

namespace {
std::vector<uint8_t> g_expected;
}

namespace recomp::ppc {
void Sc(Ctx& c, uint64_t) {
  const OracleDone* done = reinterpret_cast<const OracleDone*>(c.mem + ORACLE_DONE);
  auto be = [](uint32_t v) { return __builtin_bswap32(v); };
  uint32_t data = be(done->data), length = be(done->length);
  std::fwrite(c.mem + data, 1, length, stdout);
  bool same = length == g_expected.size() && !std::memcmp(c.mem + data, g_expected.data(), length);
  std::printf("%s (exit code %u)\n", same ? "same output as the oracle" : "DIFFERENT output from the oracle",
              unsigned(uint32_t(c.r[3])));
  std::fflush(stdout);
  std::exit(same ? 0 : 1);
}
void Trap(Ctx& c, uint32_t raw, uint64_t va) { Fatal(&c, "trap %08x at %llx", raw, (unsigned long long)va); }
uint64_t TimeBase(Ctx&) { return 0; }
void Import(Ctx& c, const char* module, uint32_t ordinal, uint64_t) {
  Fatal(&c, "the fixture imports nothing, yet called %s ordinal %u", module, ordinal);
}
}  // namespace recomp::ppc

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: xbox360_fixture fixture.xex oracle-output.bin\n");
    return 2;
  }
  recomp::xbox360::XexModule m;
  std::string err;
  if (!recomp::xbox360::LoadXex(argv[1], m, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  if (recomp::xbox360::TextHash(m) != recomp::ppc::LiftedTextHash()) {
    std::fprintf(stderr, "%s is not the module this build was lifted from\n", argv[1]);
    return 1;
  }
  std::ifstream f(argv[2], std::ios::binary);
  g_expected.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());

  uint8_t* mem = MapArena();
  std::memcpy(mem + m.base, m.image.data(), m.image.size());
  recomp::ppc::Ctx c{};
  c.mem = mem;
  c.msr = 0x8000000000000000ull;  // the 970's 64-bit mode at reset; crt0 leaves it
  recomp::ppc::GuestFn entry = recomp::ppc::LookupLifted(m.entry);
  if (!entry) {
    std::fprintf(stderr, "entry point %x was not lifted\n", m.entry);
    return 1;
  }
  entry(c);
  std::fprintf(stderr, "the fixture returned without its closing sc\n");
  return 1;
}
