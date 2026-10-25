// What the lifted fixture calls (ppc/lifted.h), in a library of its own as
// a platform's runtime is: the oracle runs it bare metal, without a
// kernel, and it ends with `sc` (oracle/crt0.S), which here compares its
// output with what the oracle printed.

#include "fixture_platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "oracle.h"
#include "ppc/fatal.h"

namespace {
std::vector<uint8_t> g_expected;
}

void SetExpectedOutput(std::vector<uint8_t> expected) { g_expected = std::move(expected); }

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
