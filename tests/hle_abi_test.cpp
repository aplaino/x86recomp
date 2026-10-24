// hle_abi_test: Arg and ArgD/ArgF under the x64 ABIs (runtime/hle/hle.h).

#include <cstdio>
#include <cstring>

#include "runtime/hle/hle.h"

using namespace recomp;
using namespace recomp::hle;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

}  // namespace

int main() {
  Process p;
  constexpr gaddr kStack = 0x100000;
  if (!p.mem.Reserve(36) || !p.mem.Commit(kStack, 0x1000, Prot::RW)) {
    std::printf("cannot set up guest memory\n");
    return 1;
  }
  g_proc = &p;
  p.x64 = true;
  Ctx c{};
  c.rdi = 1, c.rsi = 2, c.rdx = 3, c.rcx = 4, c.r8 = 5, c.r9 = 6;
  c.rsp = kStack;
  for (int k = 0; k < 8; k++) {  // [rsp]: the return address
    p.mem.write<uint64_t>(kStack + 8 * k, 100 + k);
    const double v = 0.5 + k;
    std::memcpy(c.xmm[k].b, &v, 8);
  }

  p.sysv = true;
  Check(Arg(c, 0) == 1 && Arg(c, 1) == 2 && Arg(c, 2) == 3 && Arg(c, 3) == 4 &&
            Arg(c, 4) == 5 && Arg(c, 5) == 6, "System V: rdi, rsi, rdx, rcx, r8, r9");
  Check(Arg(c, 6) == 101 && Arg(c, 7) == 102, "System V: then the stack");
  Check(ArgD(c, 0) == 0.5 && ArgD(c, 5) == 5.5 && ArgD(c, 7) == 7.5,
        "System V: xmm0-7, counted apart");

  p.sysv = false;
  Check(Arg(c, 0) == 4 && Arg(c, 1) == 3 && Arg(c, 2) == 5 && Arg(c, 3) == 6,
        "Win64: rcx, rdx, r8, r9");
  Check(Arg(c, 4) == 105, "Win64: then the stack past the shadow space");
  Check(ArgD(c, 3) == 3.5, "Win64: xmm by position");

  std::printf("%s\n", failures ? "hle_abi: FAILED" : "hle_abi: ok");
  return failures ? 1 : 0;
}
