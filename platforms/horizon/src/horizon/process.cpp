#include "horizon/process.h"

#include <cstring>

#include "a64/dispatch.h"
#include "a64/fatal.h"
#include "a64/platform.h"
#include "horizon/layout.h"
#include "horizon/nso_image.h"

namespace recomp::horizon {

using a64::Fatal;
namespace platform = a64::platform;

int RunProcess(const GameConfig& cfg) {
  Module m;
  std::string err;
  if (!LoadNro(cfg.nro, m, err) || !Relocate(m, kMainBase, err)) Fatal(nullptr, "%s", err.c_str());
  if (TextHash(m) != a64::LiftedTextHash())
    Fatal(nullptr, "%s is not the module this program was lifted from", cfg.nro.c_str());
  if (!platform::MapFixed(kMainBase, m.image.size()))
    Fatal(nullptr, "cannot map the main module at %llx", static_cast<unsigned long long>(kMainBase));
  std::memcpy(reinterpret_cast<void*>(kMainBase), m.image.data(), m.image.size());
  // Text and read-only data stay read-only, as on the console, in whole
  // host pages (16 KiB on Apple Silicon) that hold no data; .bss is already
  // zero.
  uint64_t ro_end = m.data.off & ~uint64_t(platform::PageSize() - 1);
  if (ro_end && !platform::ProtectReadOnly(kMainBase, ro_end))
    Fatal(nullptr, "cannot protect the main module's text");
  if (!platform::MapFixed(kMainStackBase, kMainStackSize))
    Fatal(nullptr, "cannot map the main thread's stack at %llx",
          static_cast<unsigned long long>(kMainStackBase));

  a64::Ctx c{};
  c.sp = kMainStackBase + kMainStackSize;
  a64::Dispatch(c, kMainBase + m.text.off, 0);
  return 0;
}

}  // namespace recomp::horizon
