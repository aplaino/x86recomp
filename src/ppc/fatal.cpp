#include "ppc/fatal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "ppc/lifted.h"

namespace recomp::ppc {

void Fatal(const Ctx* c, const char* fmt, ...) {
  std::fflush(stdout);
  std::fputs("fatal: ", stderr);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
  if (c) {
    for (int r = 0; r < 32; r++)
      std::fprintf(stderr, "  r%-2d %016llx%s", r, static_cast<unsigned long long>(c->r[r]),
                   r % 4 == 3 ? "\n" : "");
    std::fprintf(stderr, "  lr  %016llx  ctr %016llx\n  cr  %08x  xer %08x\n",
                 static_cast<unsigned long long>(c->lr), static_cast<unsigned long long>(c->ctr),
                 c->cr, c->xer);
  }
  std::abort();
}

void Unimplemented(Ctx& c, uint64_t va, const char* what) {
  Fatal(&c, "unimplemented at %llx: %s", static_cast<unsigned long long>(va), what);
}
void BadJump(Ctx& c, uint64_t va, uint64_t target) {
  Fatal(&c, "jump from %llx to %llx, which was not lifted", static_cast<unsigned long long>(va),
        static_cast<unsigned long long>(target));
}

}  // namespace recomp::ppc
