// The lifted fixture (fixture/), run against what the same program printed
// bare metal on the oracle (oracle/qemu_run.py).
//   ppc_fixture <fixture.elf> <oracle-output.bin>
// The ELF image is loaded into a 4 GiB arena at its address and run from
// its entry point in 32-bit mode, as the oracle runs it; the program ends
// with `sc` (oracle/crt0.S), which fixture_platform.cpp turns into the
// comparison of its output.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "arena.h"
#include "elf.h"
#include "fixture_platform.h"
#include "ppc/dispatch.h"
#include "ppc/fatal.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: ppc_fixture fixture.elf oracle-output.bin\n");
    return 2;
  }
  lifter::ppc::ElfImage m;
  std::string err;
  if (!lifter::ppc::LoadElf(argv[1], m, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  if (!recomp::ppc::HasLiftedModule(lifter::ppc::TextHash(m))) {
    std::fprintf(stderr, "%s is not the module this build was lifted from\n", argv[1]);
    return 1;
  }
  std::ifstream f(argv[2], std::ios::binary);
  SetExpectedOutput({std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()});

  uint8_t* mem = MapArena();
  std::memcpy(mem + m.base, m.bytes.data(), m.bytes.size());
  recomp::ppc::Ctx c{};
  c.mem = mem;
  c.msr = 0x8000000000000000ull;  // the 970's 64-bit mode at reset; crt0 leaves it
  recomp::ppc::GuestFn entry = recomp::ppc::LookupLifted(uint32_t(m.entry));
  if (!entry) {
    std::fprintf(stderr, "entry point %llx was not lifted\n", (unsigned long long)m.entry);
    return 1;
  }
  entry(c);
  std::fprintf(stderr, "the fixture returned without its closing sc\n");
  return 1;
}
