#include "orbis/process.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include "orbis/elf_image.h"
#include "orbis/hle.h"
#include "orbis/host.h"
#include "orbis/nid.h"
#include "runtime/log.h"

namespace recomp::orbis {

namespace {

struct Loaded {
  std::string name;
  ElfImage img;
};

// The process's modules: the eboot first, then the PRXs it starts with,
// then those the game loaded; a module's handle is its index. Once guest
// code runs, g_modules and g_exports change under g_load_mu.
std::deque<Loaded> g_modules;
std::map<std::pair<std::string, std::string>, gaddr> g_exports;  // (library, NID)
std::mutex g_load_mu;
std::vector<Prx> g_loadable;  // Config::loadable

ParamSfo g_param;

// Static TLS (FreeBSD's variant II): every module's block sits below the
// thread pointer, at tls.offset; the TCB at the thread pointer (process.h)
// holds its own address, then the DTV ([0] generation, [1] count, [1 + i]
// module i's block).
uint64_t g_tls_size = 0, g_tls_align = 16;
uint64_t g_tls_modules = 0;
constexpr uint64_t kTcbSize = 0x40;

// The allocator's range: above 4 GiB, as Win64's, and below the top
// 64 MiB of the arena, where the core's thunk region is (x86/layout.h).
constexpr gaddr kUserHi64 = 0xFFC000000;

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
  std::lock_guard<std::mutex> l(g_load_mu);
  for (const Loaded& m : g_modules) {
    const ElfTls& tls = m.img.tls;
    if (!m.img.has_tls) continue;
    const gaddr block = tp - tls.offset;
    std::memcpy(GPtr(block), GPtr(tls.init_va), tls.init_size);
    Wr64(dtv + 8 * (1 + tls.module), block);
  }
  t->ctx.fs_base = tp;
}

// Dev aid: <ID>_NID_NAMES names a file of symbol names (one per line)
// that imports without a handler are named by (else by NID) in traces
// and errors.
const std::map<std::string, std::string>& DevNames() {
  static const std::map<std::string, std::string> names = [] {
    std::map<std::string, std::string> m;
    if (const char* path = Env("NID_NAMES"))
      if (std::FILE* f = std::fopen(path, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
          std::string n = line;
          while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
          if (!n.empty()) m.emplace(Nid(n), n);
        }
        std::fclose(f);
      }
    return m;
  }();
  return names;
}

gaddr Bind(const ElfImport& imp, const std::string& module) {
  if (auto it = g_exports.find({imp.library, imp.nid}); it != g_exports.end()) return it->second;
  if (const std::string* name = NameOfNid(imp.library, imp.nid))
    return hle::BindImport(imp.library, *name);
  auto dev = DevNames().find(imp.nid);
  const std::string& shown = dev != DevNames().end() ? dev->second : imp.nid;
  if (imp.type == 1)
    FatalF("%s imports data %s!%s (module %s), which nothing defines", module.c_str(),
           imp.library.c_str(), shown.c_str(), imp.module.c_str());
  return hle::BindImport(imp.library, shown);  // fatal when called
}

Loaded& MapModule(const std::string& path, gaddr base) {
  Process& p = *g_proc;
  std::string err;
  Loaded& m = g_modules.emplace_back();
  m.name = path.substr(path.find_last_of('/') + 1);
  if (!m.img.Map(path, p.mem, &err, base)) FatalF("%s: %s", path.c_str(), err.c_str());
  for (const ElfSegment& s : m.img.segments) {
    const gaddr lo = s.va & ~gaddr(GuestMemory::kPageSize - 1);
    p.vm.MarkReserved(lo, (s.va + s.size - lo + GuestMemory::kPageSize - 1) &
                              ~uint64_t(GuestMemory::kPageSize - 1));
  }
  return m;
}

void LinkModule(Loaded& m) {
  std::string err;
  if (!m.img.Link(g_proc->mem, &err, [&](const ElfImport& imp) { return Bind(imp, m.name); }))
    FatalF("%s: %s", m.name.c_str(), err.c_str());
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

gaddr ProcParam() { return g_modules.empty() ? 0 : g_modules[0].img.proc_param; }

const ParamSfo& AppParam() { return g_param; }

NidName::NidName(const char* library, const char* name) {
  NidNames()[{library, Nid(name)}] = name;
}

NidName::NidName(const char* library, const char* nid, Raw) { NidNames()[{library, nid}] = nid; }

const std::string* NameOfNid(const std::string& library, const std::string& nid) {
  auto it = NidNames().find({library, nid});
  return it != NidNames().end() ? &it->second : nullptr;
}

int RunProcess(const GameConfig& game, const std::string& game_dir, const Config& config) {
  Process& p = StartProcess(game, game_dir, true);
  if (const char* t = Env("TRACE")) g_trace = *t && *t != '0';
  g_param = config.param;
  p.sysv = true;
  p.fpu_control = 0x037F;  // FreeBSD's for new threads
  p.thread_init = ThreadInit;
  p.vm.Init(&p.mem, kUserLo64, kUserHi64);

  MapModule(config.eboot, 0);
  for (const Prx& prx : config.prxs) MapModule(prx.path, prx.base);

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
    LinkModule(m);
    p.code.push_back({m.img.text_lo, m.img.text_hi - m.img.text_lo});
  }
  // The PRXs the game may load: their range of the dispatch tables is
  // set up now, as the tables do not change once guest code runs.
  g_loadable = config.loadable;
  for (const Prx& prx : g_loadable) {
    ElfImage img;
    std::string err;
    if (!img.Read(prx.path, &err, prx.base)) FatalF("%s: %s", prx.path.c_str(), err.c_str());
    for (const ElfSegment& s : img.segments)
      if (s.flags & 1) p.code.push_back({s.va, s.size});
  }

  InitDispatch();
  Thread* main = CreateGuestThread(config.main_stack);
  const gaddr args = ArgBlock(config.args);
  const gaddr cleanup = hle::BindImport("rtld", "rtld_cleanup");
  return int(RunMainThreadBeside(main, [&](Ctx& c) {
    // PRX initialisers: _init(args, argp, module_start) runs the
    // module's constructors, then module_start (its own, when the third
    // argument is null).
    for (size_t i = 1; i < g_modules.size(); i++)
      if (g_modules[i].img.init) CallGuest(c, g_modules[i].img.init, {0, 0, 0});
    // _start(the argument block, a cleanup function for atexit).
    return CallGuest(c, g_modules[0].img.entry, {args, cleanup});
  }, RunHostLoop));
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

// int sceKernelLoadStartModule(const char* path, size_t argc,
// const void* argv, uint32_t flags, const void* opt, int* result): maps
// one of the PRXs the game ships (named by its file name, at the base it
// was lifted at), binds its imports, then runs its initialiser with argc
// and argv (module_start's, as at process start), whose return value goes
// to *result. Returns the module's handle.
ORBIS_HLE("libkernel", sceKernelLoadStartModule, 6) {
  using namespace ::recomp::orbis;
  using ::recomp::hle::Arg;
  const std::string path = ::recomp::GStr(Arg(c, 0));
  const std::string file = path.substr(path.find_last_of('/') + 1);
  if (Arg(c, 3) || Arg(c, 4))
    ::recomp::FatalF("sceKernelLoadStartModule %s: flags %llx, options %llx: not implemented",
                     path.c_str(), (unsigned long long)Arg(c, 3), (unsigned long long)Arg(c, 4));
  uint64_t handle;
  ::recomp::gaddr init;
  {
    std::lock_guard<std::mutex> l(g_load_mu);
    for (const Loaded& m : g_modules)
      if (m.name == file)
        ::recomp::FatalF("sceKernelLoadStartModule %s: already loaded (not implemented)",
                         path.c_str());
    auto it = std::find_if(g_loadable.begin(), g_loadable.end(), [&](const Prx& p) {
      return p.path.substr(p.path.find_last_of('/') + 1) == file;
    });
    if (it == g_loadable.end())
      ::recomp::FatalF("sceKernelLoadStartModule %s: not a PRX the game ships (system "
                       "libraries: not implemented)",
                       path.c_str());
    Loaded& m = MapModule(it->path, it->base);
    if (m.img.has_tls)
      ::recomp::FatalF("sceKernelLoadStartModule %s: TLS in a module loaded later: not "
                       "implemented",
                       path.c_str());
    for (const ElfExport& e : m.img.exports)
      g_exports.emplace(std::make_pair(e.library, e.nid), e.va);
    LinkModule(m);
    handle = g_modules.size() - 1;
    init = m.img.init;
  }
  const uint64_t r = init ? ::recomp::CallGuest(c, init, {Arg(c, 1), Arg(c, 2), 0}) : 0;
  if (const ::recomp::gaddr result = Arg(c, 5)) ::recomp::Wr32(result, uint32_t(r));
  return handle;
}

// int sceKernelDlsym(int handle, const char* symbol, void** address):
// the address the module exports symbol at (by its NID).
ORBIS_HLE("libkernel", sceKernelDlsym, 3) {
  using namespace ::recomp::orbis;
  using ::recomp::hle::Arg;
  const uint64_t handle = uint32_t(Arg(c, 0));
  const std::string symbol = ::recomp::GStr(Arg(c, 1)), nid = Nid(symbol);
  std::lock_guard<std::mutex> l(g_load_mu);
  if (handle >= g_modules.size())
    ::recomp::FatalF("sceKernelDlsym %s: no module %llu", symbol.c_str(),
                     (unsigned long long)handle);
  const Loaded& m = g_modules[handle];
  for (const ElfExport& e : m.img.exports)
    if (e.nid == nid) {
      ::recomp::Wr64(Arg(c, 2), e.va);
      return 0;
    }
  ::recomp::FatalF("sceKernelDlsym: %s does not export %s (not implemented)", m.name.c_str(),
                   symbol.c_str());
}
