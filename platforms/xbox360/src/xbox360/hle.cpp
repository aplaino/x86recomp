#include "xbox360/hle.h"

#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ppc/fatal.h"
#include "xbox360/xex.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;

struct Function {
  const char* name;
  HleFn fn;
  ppc::GuestFn call;
};
struct Variable {
  const char* name;
  uint32_t size;
  void (*init)(uint32_t va);
  uint32_t va = 0;
};
struct Area {
  uint32_t base, size;
};
struct Registry {
  std::map<std::string, std::map<uint32_t, Function>> functions;
  std::map<std::string, std::map<uint32_t, Variable>> variables;
  std::mutex lock;  // variables' allocation
  std::map<std::string, Area> areas;
  std::vector<ppc::LiftedFn> exports;  // what MapSystemExports registered
};
// Filled before main() by the HLE modules' registrars.
Registry& R() {
  static Registry r;
  return r;
}

const Function* FindFunction(const char* module, uint32_t ordinal) {
  auto m = R().functions.find(module);
  if (m == R().functions.end()) return nullptr;
  auto f = m->second.find(ordinal);
  return f == m->second.end() ? nullptr : &f->second;
}

// Each thread's handlers by thunk address, so that a call needs no lock.
thread_local std::unordered_map<uint32_t, HleFn> t_thunks;

}  // namespace

Registrar::Registrar(const char* module, uint32_t ordinal, const char* name, HleFn fn,
                     ppc::GuestFn call) {
  if (!R().functions[module].emplace(ordinal, Function{name, fn, call}).second)
    Fatal(nullptr, "two handlers for %s!%u", module, ordinal);
}

VariableRegistrar::VariableRegistrar(const char* module, uint32_t ordinal, const char* name,
                                     uint32_t size, void (*init)(uint32_t va)) {
  if (!R().variables[module].emplace(ordinal, Variable{name, size, init}).second)
    Fatal(nullptr, "two definitions of %s!%u", module, ordinal);
}

std::string ExportName(const char* module, uint32_t ordinal) {
  if (const Function* f = FindFunction(module, ordinal)) return std::string(module) + "!" + f->name;
  auto m = R().variables.find(module);
  if (m != R().variables.end()) {
    auto v = m->second.find(ordinal);
    if (v != m->second.end()) return std::string(module) + "!" + v->second.name;
  }
  return std::string(module) + "!" + std::to_string(ordinal);
}

uint32_t VariableAddress(const char* module, uint32_t ordinal) {
  std::lock_guard<std::mutex> g(R().lock);
  auto m = R().variables.find(module);
  if (m == R().variables.end()) return 0;
  auto it = m->second.find(ordinal);
  if (it == m->second.end()) return 0;
  Variable& v = it->second;
  if (!v.va) {
    v.va = SystemAlloc(v.size);
    if (v.init) v.init(v.va);
  }
  return v.va;
}

namespace {

// A call through the address of an export that has no handler: fatal,
// naming it, as a call to its import is. Lifted code leaves an indirect
// call's target in CTR.
void Unhandled(ppc::Ctx& c) {
  uint32_t target = uint32_t(c.ctr);
  for (const auto& [module, a] : R().areas)
    if (target - a.base < a.size)
      Fatal(&c, "%s has no handler (called through its address from %llx)",
            ExportName(module.c_str(), (target - a.base) / 4).c_str(), static_cast<unsigned long long>(c.lr));
  Fatal(&c, "a system export without a handler was called (from %llx)", static_cast<unsigned long long>(c.lr));
}

}  // namespace

void MapSystemExports() {
  constexpr uint32_t kTrap = 0x7FE00008;  // tw 31, 0, 0
  for (auto& [module, fns] : R().functions) {
    uint32_t size = 4 * kSystemOrdinals, base = SystemAlloc(size);
    R().areas[module] = {base, size};
    for (uint32_t ordinal = 0; ordinal < kSystemOrdinals; ordinal++) {
      uint32_t va = base + 4 * ordinal;
      Wr32(va, kTrap);
      auto f = fns.find(ordinal);
      R().exports.push_back({va, f == fns.end() ? &Unhandled : f->second.call});
    }
  }
  ppc::RegisterGuestFunctions(R().exports.data(), uint32_t(R().exports.size()));
}

bool SystemExportArea(const std::string& module, uint32_t& base, uint32_t& size) {
  auto it = R().areas.find(module);
  if (it == R().areas.end()) return false;
  base = it->second.base;
  size = it->second.size;
  return true;
}

uint32_t SystemExport(const std::string& module, uint32_t ordinal) {
  if (uint32_t va = VariableAddress(module.c_str(), ordinal)) return va;
  auto it = R().areas.find(module);
  if (it == R().areas.end() || ordinal >= kSystemOrdinals) return 0;
  return it->second.base + 4 * ordinal;
}

void BindImports(const XexModule& m) {
  std::string missing;
  for (const XexImport& i : m.imports) {
    if (i.thunk) continue;
    // A function import's two records: this slot and the thunk after it.
    const XexImport* thunk = nullptr;
    for (const XexImport& t : m.imports)
      if (t.thunk && t.module == i.module && t.ordinal == i.ordinal) thunk = &t;
    if (thunk) {
      Wr32(i.va, thunk->va);
      continue;
    }
    uint32_t va = VariableAddress(i.module.c_str(), i.ordinal);
    if (!va) missing += "\n  " + ExportName(i.module.c_str(), i.ordinal);
    Wr32(i.va, va);
  }
  if (!missing.empty())
    Fatal(nullptr, "%s imports variables that have no definition:%s", m.original_name.c_str(),
          missing.c_str());
}

std::string ReadString(uint32_t va) {
  std::string s;
  for (uint8_t b; (b = Rd8(va)) != 0; va++) s.push_back(char(b));
  return s;
}

std::string ReadAnsiString(uint32_t ansi_string) {
  uint32_t buf = Rd32(ansi_string + 4);
  return buf ? std::string(reinterpret_cast<const char*>(Mem() + buf), Rd16(ansi_string)) : std::string();
}

std::u16string ReadString16(uint32_t va) {
  std::u16string s;
  for (uint16_t u; (u = Rd16(va)) != 0; va += 2) s.push_back(char16_t(u));
  return s;
}

}  // namespace recomp::xbox360

namespace recomp::ppc {

void Import(Ctx& c, const char* module, uint32_t ordinal, uint64_t va) {
  auto it = xbox360::t_thunks.find(uint32_t(va));
  xbox360::HleFn fn;
  if (it != xbox360::t_thunks.end()) {
    fn = it->second;
  } else {
    const xbox360::Function* f = xbox360::FindFunction(module, ordinal);
    if (!f)
      Fatal(&c, "%s has no handler (called from %llx)", xbox360::ExportName(module, ordinal).c_str(),
            static_cast<unsigned long long>(c.lr));
    fn = f->fn;
    xbox360::t_thunks.emplace(uint32_t(va), fn);
  }
  c.r[3] = fn(c);
}

}  // namespace recomp::ppc
