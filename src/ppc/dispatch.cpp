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
// Filled before main() by each lifted module's lifted_table.cpp.
Table& Lifted() {
  static Table t;
  return t;
}

}  // namespace

void RegisterLiftedFunctions(const LiftedFn* fns, uint32_t count, uint64_t text_hash) {
  Table& t = Lifted();
  t.fns.reserve(t.fns.size() + count);
  for (uint32_t i = 0; i < count; i++)
    if (!t.fns.emplace(fns[i].va, fns[i].fn).second)
      Fatal(nullptr, "two lifted modules have a function at %x", fns[i].va);
  t.text_hashes.push_back(text_hash);
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

}  // namespace recomp::ppc
