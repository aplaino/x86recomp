#include "orbis/process.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "orbis/elf_image.h"
#include "orbis/hle.h"
#include "orbis/nid.h"
#include "runtime/log.h"

namespace recomp::orbis {

namespace {

struct Loaded {
  std::string name;
  ElfImage img;
};

// The process's modules: the eboot first, then the PRXs.
std::vector<Loaded> g_modules;
std::map<std::pair<std::string, std::string>, gaddr> g_exports;  // (library, NID)

// Static TLS (FreeBSD's variant II): every module's block sits below the
// thread pointer, at tls.offset; the TCB at the thread pointer holds its
// own address, then the DTV ([0] generation, [1] count, [1 + i] module
// i's block).
uint64_t g_tls_size = 0, g_tls_align = 16;
uint64_t g_tls_modules = 0;
constexpr uint64_t kTcbSize = 0x40;

std::map<std::pair<std::string, std::string>, std::string>& NidNames() {
  static std::map<std::pair<std::string, std::string>, std::string> m;
  return m;
}

void ThreadInit(Thread* t) {
  Process& p = *g_proc;
  const uint64_t dtv_size = 8 * (2 + g_tls_modules);
  const uint64_t size = g_tls_size + kTcbSize + dtv_size + g_tls_align;
  const gaddr base = p.vm.Reserve(0, size);
  if (!base || !p.vm.Commit(base, size, Prot::RW)) FatalF("cannot allocate a TLS area");
  const gaddr tp = (base + g_tls_size + g_tls_align - 1) / g_tls_align * g_tls_align;
  const gaddr dtv = tp + kTcbSize;
  Wr64(tp, tp);       // tcb_self
  Wr64(tp + 8, dtv);  // tcb_dtv
  Wr64(dtv, 1);       // generation
  Wr64(dtv + 8, g_tls_modules);
  for (const Loaded& m : g_modules) {
    const ElfTls& tls = m.img.tls;
    if (!m.img.has_tls) continue;
    const gaddr block = tp - tls.offset;
    std::memcpy(GPtr(block), GPtr(tls.init_va), tls.init_size);
    Wr64(dtv + 8 * (1 + tls.module), block);
  }
  t->ctx.fs_base = tp;
}

gaddr Bind(const ElfImport& imp, const std::string& module) {
  if (auto it = g_exports.find({imp.library, imp.nid}); it != g_exports.end()) return it->second;
  if (const std::string* name = NameOfNid(imp.library, imp.nid))
    return hle::BindImport(imp.library, *name);
  if (imp.type == 1)
    FatalF("%s imports data %s!%s (module %s), which nothing defines", module.c_str(),
           imp.library.c_str(), imp.nid.c_str(), imp.module.c_str());
  return hle::BindImport(imp.library, imp.nid);  // fatal when called
}

// argc, argv[], NULL, an empty environment and auxv, then the strings.
gaddr ArgBlock(const std::vector<std::string>& args) {
  Process& p = *g_proc;
  uint64_t size = 8 * (args.size() + 5);
  for (const auto& a : args) size += a.size() + 1;
  const gaddr b = p.vm.Reserve(0, size);
  if (!b || !p.vm.Commit(b, size, Prot::RW)) FatalF("cannot allocate the argument block");
  Wr64(b, args.size());
  gaddr str = b + 8 * (args.size() + 5);
  for (size_t i = 0; i < args.size(); i++) {
    Wr64(b + 8 * (1 + i), str);
    PutStr(str, args[i]);
    str += args[i].size() + 1;
  }
  return b;  // the NULL terminators are already zero
}

}  // namespace

NidName::NidName(const char* library, const char* name) {
  NidNames()[{library, Nid(name)}] = name;
}

const std::string* NameOfNid(const std::string& library, const std::string& nid) {
  auto it = NidNames().find({library, nid});
  return it != NidNames().end() ? &it->second : nullptr;
}

int RunProcess(const GameConfig& game, const std::string& game_dir, const Config& config) {
  Process& p = StartProcess(game, game_dir, true);
  p.sysv = true;
  p.fpu_control = 0x037F;  // FreeBSD's for new threads
  p.thread_init = ThreadInit;
  p.vm.Init(&p.mem, kUserLo64, kTebLo64);

  std::string err;
  auto map = [&](const std::string& path, gaddr base) {
    Loaded& m = g_modules.emplace_back();
    m.name = path.substr(path.find_last_of('/') + 1);
    if (!m.img.Map(path, p.mem, &err, base)) FatalF("%s: %s", path.c_str(), err.c_str());
    for (const ElfSegment& s : m.img.segments) {
      const gaddr lo = s.va & ~gaddr(GuestMemory::kPageSize - 1);
      p.vm.MarkReserved(lo, (s.va + s.size - lo + GuestMemory::kPageSize - 1) &
                                ~uint64_t(GuestMemory::kPageSize - 1));
    }
  };
  map(config.eboot, 0);
  for (const Prx& prx : config.prxs) map(prx.path, prx.base);

  // TLS slots in load order, the eboot's (if any) first and right below
  // the thread pointer.
  for (Loaded& m : g_modules) {
    if (!m.img.has_tls) continue;
    ElfTls& tls = m.img.tls;
    tls.module = ++g_tls_modules;
    tls.offset = (g_tls_size + tls.mem_size + tls.align - 1) / tls.align * tls.align;
    g_tls_size = tls.offset;
    g_tls_align = std::max(g_tls_align, tls.align);
  }
  for (const Loaded& m : g_modules)
    for (const ElfExport& e : m.img.exports) g_exports.emplace(std::make_pair(e.library, e.nid), e.va);
  for (Loaded& m : g_modules) {
    if (!m.img.Link(p.mem, &err, [&](const ElfImport& imp) { return Bind(imp, m.name); }))
      FatalF("%s: %s", m.name.c_str(), err.c_str());
    p.code.push_back({m.img.text_lo, m.img.text_hi - m.img.text_lo});
  }

  InitDispatch();
  Thread* main = CreateGuestThread(config.main_stack);
  const gaddr args = ArgBlock(config.args);
  const gaddr cleanup = hle::BindImport("rtld", "rtld_cleanup");
  return int(RunMainThread(main, [&](Ctx& c) {
    // PRX initialisers: _init(args, argp, module_start) runs the
    // module's constructors, then module_start (its own, when the third
    // argument is null).
    for (size_t i = 1; i < g_modules.size(); i++)
      if (g_modules[i].img.init) CallGuest(c, g_modules[i].img.init, {0, 0, 0});
    // _start(the argument block, a cleanup function for atexit).
    return CallGuest(c, g_modules[0].img.entry, {args, cleanup});
  }));
}

}  // namespace recomp::orbis

// The cleanup function the loader passes to _start, which registers it
// with atexit: nothing to undo.
HLE64("rtld", rtld_cleanup, 0) { return 0; }

// The thread's block of TLS module ti->module, plus ti->offset (general-
// dynamic TLS).
ORBIS_HLE("libkernel", __tls_get_addr, 1) {
  const ::recomp::gaddr ti = ::recomp::hle::Arg(c, 0);
  const ::recomp::gaddr dtv = ::recomp::Rd64(c.fs_base + 8);
  const uint64_t module = ::recomp::Rd64(ti);
  if (module == 0 || module > ::recomp::Rd64(dtv + 8))
    ::recomp::FatalF("__tls_get_addr: TLS module %llu of %llu", (unsigned long long)module,
                     (unsigned long long)::recomp::Rd64(dtv + 8));
  return ::recomp::Rd64(dtv + 8 * (1 + module)) + ::recomp::Rd64(ti + 8);
}
