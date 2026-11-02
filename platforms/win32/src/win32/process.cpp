// The Win32 process (win32/process.h): the PE image and lifted DLLs, the
// PEB, TEBs and static TLS, on the x86 core's process.

#include "win32/process.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"

namespace recomp {

namespace hle {
bool RaiseAccessViolation(Ctx& c, bool write, uint32_t addr);
}

Win32Process* g_win32 = nullptr;

const win32::GameConfig& Game() { return g_win32->game; }

namespace {

constexpr uint32_t kTlsArraySlots = 64;

std::mutex g_teb_mu;
gaddr g_next_teb = 0;  // the next TEB is below it (kTebTop, kTebTop64)

// The image's .text, where its lifted functions are.
std::pair<gaddr, uint64_t> TextOf(const PeImage& img) {
  for (const auto& s : img.sections)
    if (s.name == ".text") return {s.va, s.vsize};
  return {0, 0};
}

// Process::thread_init: the thread's TEB and TLS array, in the layout of
// the process's mode, found through fs (32-bit) or gs (x64).
void InitTeb(Thread* t) {
  Process& p = *g_proc;
  const Win32Process& w = *g_win32;
  const uint32_t stride = p.x64 ? kTebStride64 : kTebStride;
  gaddr teb;
  {
    std::lock_guard<std::mutex> l(g_teb_mu);
    if (!g_next_teb) g_next_teb = p.x64 ? kTebTop64 : kTebTop;
    g_next_teb -= stride;
    if (p.x64 && g_next_teb < kTebLo64) FatalF("too many guest threads");
    teb = g_next_teb;
  }
  p.mem.Commit(teb, stride, Prot::RW);
  if (p.x64)
    t->ctx.gs_base = teb;
  else
    t->ctx.fs_base = teb;
  // Static TLS of the image (__declspec(thread)): a per-thread copy of the
  // template, found through the TEB's ThreadLocalStoragePointer
  // [tls_index].
  const uint32_t ptr = p.x64 ? 8 : 4;
  gaddr tls_array = HeapAllocProcess(kTlsArraySlots * ptr, true);
  if (p.x64) {  // NT_TIB64 and the x64 TEB
    Wr64(teb + 0x08, t->stack_hi);  // StackBase
    Wr64(teb + 0x10, t->stack_lo);  // StackLimit
    Wr64(teb + 0x30, teb);          // Self
    Wr64(teb + 0x40, kGuestPid);    // ClientId
    Wr64(teb + 0x48, t->tid);
    Wr64(teb + 0x58, tls_array);    // ThreadLocalStoragePointer
    Wr64(teb + 0x60, kPebVa64);
  } else {
    Wr32(teb + 0x00, 0xFFFFFFFF);  // SEH chain: empty
    Wr32(teb + 0x04, uint32_t(t->stack_hi));
    Wr32(teb + 0x08, uint32_t(t->stack_lo));
    Wr32(teb + 0x18, uint32_t(teb));  // Self
    Wr32(teb + 0x20, kGuestPid);
    Wr32(teb + 0x24, t->tid);
    Wr32(teb + 0x2C, uint32_t(tls_array));
    Wr32(teb + 0x30, kPebVa);
  }
  auto block = [&](const PeImage& img, int index) {
    const PeTls& tls = img.tls;
    uint64_t raw = tls.raw_end - tls.raw_start;
    gaddr b = HeapAllocProcess(raw + tls.zero_fill + 16, true);
    std::memcpy(GPtr(b), GPtr(tls.raw_start), raw);
    WrPtr(tls_array + ptr * uint32_t(index), b);
  };
  if (w.img.has_tls) block(w.img, 0);
  // Lifted DLLs with static TLS (x64 guests, as on Vista and later): every
  // thread has their blocks, loaded or not, since all are mapped at start.
  for (const Module& m : w.modules)
    if (m.tls_index >= 0) block(m.img, m.tls_index);
}

// Static TLS callbacks are not run (no supported module has any).
void CheckNoTlsCallbacks(const PeImage& img, const std::string& name) {
  if (img.has_tls && img.tls.callbacks && RdPtr(img.tls.callbacks))
    FatalF("%s has TLS callbacks: not supported", name.c_str());
}

}  // namespace

const PeImage* ImageAt(gaddr va) {
  const Win32Process& w = *g_win32;
  if (va - w.img.image_base < w.img.image_size) return &w.img;
  for (const Module& m : w.modules)
    if (va - m.img.image_base < m.img.image_size) return &m.img;
  return nullptr;
}

void MapModule(Module& m) {
  Process& p = *g_proc;
  if (m.img.image_size) p.mem.Decommit(m.img.image_base, m.img.image_size);
  std::string err;
  if (!m.img.Load(m.host_path, p.mem, &err, m.base, hle::ImportThunk))
    FatalF("lifted DLL %s: %s", m.name.c_str(), err.c_str());
  // Windows XP did not set up static TLS for LoadLibrary'd DLLs either,
  // so 32-bit DLLs of that era have none. x64 ones get a slot of their own
  // after the EXE's (slot 0), kept across remaps.
  if (m.img.has_tls) {
    if (!p.x64)
      FatalF("lifted DLL %s has static TLS (__declspec(thread)): not supported",
             m.name.c_str());
    CheckNoTlsCallbacks(m.img, m.name);
    if (m.tls_index < 0) {
      int next = 1;
      for (const Module& o : g_win32->modules) next = std::max(next, o.tls_index + 1);
      m.tls_index = next;
    }
    Wr32(m.img.tls.index_va, uint32_t(m.tls_index));
  }
  m.stale = false;
}

int RunProcess(const win32::GameConfig& game, const std::string& exe_path,
               const std::string& game_dir, const std::string& game_args) {
  g_win32 = new Win32Process;
  Win32Process& w = *g_win32;
  w.game = game;
  Process& p = StartProcess(game, game_dir, PeImage::IsX64File(exe_path));
  p.thread_init = InitTeb;
  p.thread_exit = [](Thread* t, uint32_t code) { OnGuestThreadFinished(t->tid, code); };
  p.guest_fault = [](Ctx& c, bool write, gaddr addr) {
    return hle::RaiseAccessViolation(c, write, uint32_t(addr));
  };
  p.pump_events = PumpHostEvents;
  w.game_args = game_args;
  std::string err;
  if (!w.img.Load(exe_path, p.mem, &err, 0, hle::ImportThunk))
    FatalF("%s", err.c_str());
  for (const auto& d : game.lifted_dlls) {
    Module& m = w.modules.emplace_back();
    m.name = hle::NormaliseDll(d.name);
    m.host_path = HostPath(GuestGameDir() + "\\" + d.name);
    m.base = d.base;
    MapModule(m);
  }

  if (p.x64) {
    // Everything above 4 GiB, below the TEBs.
    p.vm.Init(&p.mem, kUserLo64, kTebLo64);
  } else {
    p.vm.Init(&p.mem, 0x00010000, kThunkBase);
    p.vm.MarkReserved(hle::kModuleRegion, hle::kModuleRegionSize);
  }
  p.vm.MarkReserved(w.img.image_base, w.img.image_size);
  for (const Module& m : w.modules) p.vm.MarkReserved(m.img.image_base, m.img.image_size);

  // PEB: only the fields Win32 code commonly peeks at. x64 guests see
  // Windows 10 (22H2), 32-bit ones Windows XP.
  if (p.x64) {
    p.mem.Commit(kPebVa64, 0x1000, Prot::RW);
    Wr64(kPebVa64 + 0x10, w.img.image_base);
    Wr32(kPebVa64 + 0xB8, GuestProcessorCount());  // NumberOfProcessors
    Wr32(kPebVa64 + 0x118, 10);     // OSMajorVersion
    Wr32(kPebVa64 + 0x11C, 0);      // OSMinorVersion
    Wr16(kPebVa64 + 0x120, 19045);  // OSBuildNumber
    Wr32(kPebVa64 + 0x124, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT
  } else {
    p.mem.Commit(kPebVa, 0x1000, Prot::RW);
    Wr32(kPebVa + 0x08, uint32_t(w.img.image_base));
    Wr32(kPebVa + 0x64, GuestProcessorCount());  // NumberOfProcessors
    Wr32(kPebVa + 0xA4, 5);      // OSMajorVersion (Windows XP)
    Wr32(kPebVa + 0xA8, 1);      // OSMinorVersion
    Wr16(kPebVa + 0xAC, 2600);   // OSBuildNumber
    Wr32(kPebVa + 0xB0, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT
  }

  // The image's TLS index: slot 0 (lifted DLLs have theirs, MapModule).
  if (w.img.has_tls) {
    CheckNoTlsCallbacks(w.img, game.exe_name);
    Wr32(w.img.tls.index_va, 0);
  }

  // Dispatch looks lifted code up in each image's .text: the EXE's first.
  p.code.push_back(TextOf(w.img));
  for (const Module& m : w.modules) p.code.push_back(TextOf(m.img));
  InitDispatch();
  for (const Module& m : w.modules) {
    // A DLL lifted at another base, or not at all, has no functions here.
    if (m.img.entry && !IsLifted(m.img.entry))
      FatalF("%s at %08llX: its entry point %08llX is not lifted (lift it with "
             "recomp_lift --module, at the same --base)",
             m.name.c_str(), (unsigned long long)m.img.image_base,
             (unsigned long long)m.img.entry);
  }
  InitKernel32State();

  Thread* main = CreateGuestThread(w.img.stack_reserve);
  return int(RunMainThread(main, [&](Ctx& c) { return CallGuest(c, w.img.entry, {}); }));
}

}  // namespace recomp
