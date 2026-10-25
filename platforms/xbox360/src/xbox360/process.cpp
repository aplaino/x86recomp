#include "xbox360/process.h"

#include <unistd.h>

#include <cstring>

#include "ppc/dispatch.h"
#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/memory.h"
#include "xbox360/xex.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;

// The main thread's stack when the XEX names none, as Xenia chose it (a
// console check is pending), and the TLS slots when the XEX has no TLS
// info, as ReXGlue chose them.
constexpr uint32_t kDefaultStackSize = 1 << 20, kDefaultTlsSlots = 1024;

// Maps an image at its base in the image heap that holds it, with its
// sections' protections.
void MapImage(const XexModule& m) {
  Heap h;
  uint32_t size = uint32_t(m.image.size());
  if (!HeapOf(m.base, h) || (h != Heap::kImage64k && h != Heap::kImage4k) ||
      Reserve(h, m.base, size, 0, false) != m.base || !Commit(m.base, size, kPageReadWrite))
    Fatal(nullptr, "cannot map %s at %x (%u bytes)", m.original_name.c_str(), m.base, size);
  std::memcpy(Mem() + m.base, m.image.data(), size);
  for (const XexSection& s : m.sections)
    if (s.code) Protect(s.va, s.size, kPageExecuteRead, nullptr);
}

}  // namespace

void RunProcess(const GameConfig& cfg) {
  XexModule m;
  std::string err;
  if (!LoadXex(cfg.xex, m, err)) Fatal(nullptr, "%s", err.c_str());
  if (!ppc::HasLiftedModule(TextHash(m)))
    Fatal(nullptr, "%s is not the module this program was lifted from", cfg.xex.c_str());
  MapGuestMemory();
  MapImage(m);
  BindImports(m);
  SetProcessInfo({m.tls.slots ? m.tls.slots : kDefaultTlsSlots, m.tls.data_size, m.tls.raw_data,
                  m.tls.raw_size, m.stack_size ? m.stack_size : kDefaultStackSize});
  CreateThread({0, 0, m.entry, 0, 0, true});
  // The title runs on its own threads; this one waits for the end, which
  // ExitProcess makes.
  for (;;) pause();
}

}  // namespace recomp::xbox360
