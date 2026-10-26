#include "xbox360/process.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "ppc/dispatch.h"
#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/fs.h"
#include "xbox360/gpu.h"
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

std::filesystem::path DataDirectory(const std::string& id) {
  const char* home = std::getenv("HOME");
  std::filesystem::path h = home ? home : ".";
#if defined(__APPLE__)
  return h / "Library" / "Application Support" / id;
#else
  const char* xdg = std::getenv("XDG_DATA_HOME");
  return (xdg && *xdg ? std::filesystem::path(xdg) : h / ".local" / "share") / id;
#endif
}

// The title's disc and the hard disk's data partition (allocation units of
// 2 KiB, GDFX's sectors, and 16 KiB, FATX's clusters), and the links the
// system makes for a title it launches from a disc (xam creates GAME:; D:
// as Xenia has it).
void MountFiles(const GameConfig& cfg) {
  namespace fs = std::filesystem;
  fs::path game = cfg.game_dir.empty() ? fs::absolute(cfg.xex).parent_path() : fs::path(cfg.game_dir);
  fs::path data = (cfg.data_dir.empty() ? DataDirectory(cfg.id) : fs::path(cfg.data_dir)) / "Partition1";
  std::error_code ec;
  fs::create_directories(data, ec);
  if (ec) Fatal(nullptr, "cannot create %s: %s", data.c_str(), ec.message().c_str());
  MountDevice({"\\Device\\CdRom0", game, false, "GDFX", kFileDeviceCdRom, 0x800});
  MountDevice({"\\Device\\Harddisk0\\Partition1", data, true, "FATX", kFileDeviceDisk, 0x4000});
  CreateSymbolicLink("GAME:", "\\Device\\CdRom0");
  CreateSymbolicLink("D:", "\\Device\\CdRom0");
}

}  // namespace

void RunProcess(const GameConfig& cfg) {
  XexModule m;
  std::string err;
  if (!LoadXex(cfg.xex, m, err)) Fatal(nullptr, "%s", err.c_str());
  if (!ppc::HasLiftedModule(TextHash(m)))
    Fatal(nullptr, "%s is not the module this program was lifted from", cfg.xex.c_str());
  SetSignedInProfile(cfg.profile);
  MapGuestMemory();
  MountFiles(cfg);
  MapImage(m);
  LoadExecutableModule(m, std::filesystem::path(cfg.xex).filename().string());
  BindImports(m);
  SetProcessInfo({m.tls.slots ? m.tls.slots : kDefaultTlsSlots, m.tls.data_size, m.tls.raw_data,
                  m.tls.raw_size, m.stack_size ? m.stack_size : kDefaultStackSize});
  StartGpu();
  CreateThread({0, 0, m.entry, 0, 0, true});
  // The title runs on its own threads; this one waits for the end, which
  // ExitProcess makes.
  for (;;) pause();
}

}  // namespace recomp::xbox360
