#include "ppc/dispatch.h"

#include <unordered_map>

namespace recomp::ppc {
namespace {

struct Table {
  std::unordered_map<uint32_t, GuestFn> fns;
  uint64_t text_hash = 0;
};
// Filled before main() by the game's lifted_table.cpp.
Table& Lifted() {
  static Table t;
  return t;
}

}  // namespace

void RegisterLiftedFunctions(const LiftedFn* fns, uint32_t count, uint64_t text_hash) {
  Table& t = Lifted();
  t.fns.reserve(count);
  for (uint32_t i = 0; i < count; i++) t.fns.emplace(fns[i].va, fns[i].fn);
  t.text_hash = text_hash;
}

GuestFn LookupLifted(uint32_t va) {
  auto it = Lifted().fns.find(va);
  return it == Lifted().fns.end() ? nullptr : it->second;
}

uint64_t LiftedTextHash() { return Lifted().text_hash; }

void Dispatch(Ctx& c, uint64_t target, uint64_t va) {
  GuestFn fn = target >> 32 ? nullptr : LookupLifted(uint32_t(target));
  if (!fn) BadJump(c, va, target);
  fn(c);
}

}  // namespace recomp::ppc
