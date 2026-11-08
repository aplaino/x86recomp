#include "ppc/dispatch.h"

#include <unordered_map>
#include <vector>

#include "ppc/fatal.h"

namespace recomp::ppc {
namespace {

struct Table {
  std::unordered_map<uint32_t, GuestFn> fns;
  std::vector<uint64_t> text_hashes;
};
// Filled before main() by each lifted module's lifted_table.cpp, and by the
// platform's guest functions before the first guest thread.
Table& Lifted() {
  static Table t;
  return t;
}

}  // namespace

void RegisterGuestFunctions(const LiftedFn* fns, uint32_t count) {
  Table& t = Lifted();
  t.fns.reserve(t.fns.size() + count);
  for (uint32_t i = 0; i < count; i++)
    if (!t.fns.emplace(fns[i].va, fns[i].fn).second)
      Fatal(nullptr, "two lifted modules have a function at %x", fns[i].va);
}

void RegisterLiftedFunctions(const LiftedFn* fns, uint32_t count, uint64_t text_hash) {
  RegisterGuestFunctions(fns, count);
  Lifted().text_hashes.push_back(text_hash);
}

GuestFn LookupLifted(uint32_t va) {
  auto it = Lifted().fns.find(va);
  return it == Lifted().fns.end() ? nullptr : it->second;
}

bool HasLiftedModule(uint64_t text_hash) {
  for (uint64_t h : Lifted().text_hashes)
    if (h == text_hash) return true;
  return false;
}

void Dispatch(Ctx& c, uint64_t target, uint64_t va) {
  GuestFn fn = target >> 32 ? nullptr : LookupLifted(uint32_t(target));
  if (!fn) BadJump(c, va, target);
  fn(c);
}

// The continuation runs in the resumable function's catch handler, where
// another throw would leave that function: a later one into the same frame
// is caught here.
void Resume(Ctx& c, GuestResume r, uint32_t lo, uint32_t hi) {
  for (;;) {
    try {
      if (!LookupLifted(r.target))
        Fatal(&c, "an unwind continues at %x in frame %x, which was not lifted as a resume target",
              r.target, r.frame);
      Dispatch(c, r.target, 0);
      return;
    } catch (const GuestResume& again) {
      if (again.frame != r.frame || again.target < lo || again.target >= hi) throw;
      r = again;
    }
  }
}

}  // namespace recomp::ppc
