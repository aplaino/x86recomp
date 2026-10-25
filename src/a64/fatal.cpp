#include "a64/fatal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "a64/lifted.h"

namespace recomp::a64 {

void Fatal(const Ctx* c, const char* fmt, ...) {
  std::fflush(stdout);
  std::fputs("fatal: ", stderr);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
  if (c) {
    for (int r = 0; r < 31; r++)
      std::fprintf(stderr, "  x%-2d %016llx%s", r, static_cast<unsigned long long>(c->x[r]),
                   r % 4 == 3 ? "\n" : "");
    std::fprintf(stderr, "  sp  %016llx\n  nzcv %08x tpidr_el0 %016llx\n",
                 static_cast<unsigned long long>(c->sp), c->nzcv,
                 static_cast<unsigned long long>(c->tpidr_el0));
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
void Brk(Ctx& c, uint64_t va, uint32_t imm) {
  Fatal(&c, "brk #%u at %llx", imm, static_cast<unsigned long long>(va));
}

}  // namespace recomp::a64
