#include <cstring>
#include <vector>

#include "xbox360/fs.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/xex.h"

namespace recomp::xbox360 {
namespace {

// LDR_DATA_TABLE_ENTRY: the XEX headers' address at kLdrXexHeader is what
// titles and xam read (xam's own code, through XexExecutableModuleHandle);
// the other fields are as Xenia lays them out, awaiting the console check
// (docs/xboxkrnl.md).
constexpr uint32_t kLdrSize = 0x64, kLdrInLoadOrder = 0x0, kLdrInMemoryOrder = 0x8,
                   kLdrInInitializationOrder = 0x10, kLdrDllBase = 0x18, kLdrImageBase = 0x1C,
                   kLdrImageSize = 0x20, kLdrFullDllName = 0x24, kLdrBaseDllName = 0x2C,
                   kLdrFullImageSize = 0x38, kLdrEntryPoint = 0x3C, kLdrLoadCount = 0x40,
                   kLdrTimeDateStamp = 0x50;
// Optional header: the image's checksum and time stamp.
constexpr uint32_t kXexChecksumTimestamp = 0x00018002;

// The system modules a title may ask the loader for, by the names the HLE
// modules register their handlers under.
constexpr const char* kSystemModules[] = {"xboxkrnl.exe", "xam.xex"};
struct SystemEntry {
  const char* name;
  uint32_t ldr;
};
std::vector<SystemEntry> g_system;

uint32_t g_ldr, g_title_id;
std::string g_name;
std::vector<uint32_t> g_exports;
uint32_t g_export_base;

// A UNICODE_STRING at `at` (length and capacity in bytes, buffer) holding
// the ASCII name s, its buffer in kernel memory.
void UnicodeString(uint32_t at, const std::string& s) {
  uint32_t n = uint32_t(s.size()), buf = SystemAlloc(2 * n + 2);
  for (uint32_t k = 0; k < n; k++) Wr16(buf + 2 * k, uint8_t(s[k]));
  Wr16(at, uint16_t(2 * n));
  Wr16(at + 2, uint16_t(2 * n + 2));
  Wr32(at + 4, buf);
}

// A loader entry, not in a list of loaded modules (each of its lists is
// empty).
uint32_t LoaderEntry(const std::string& full_name, const std::string& name, uint32_t base,
                     uint32_t size, uint32_t entry) {
  uint32_t ldr = SystemAlloc(kLdrSize);
  std::memset(Mem() + ldr, 0, kLdrSize);
  for (uint32_t list : {kLdrInLoadOrder, kLdrInMemoryOrder, kLdrInInitializationOrder}) {
    Wr32(ldr + list, ldr + list);
    Wr32(ldr + list + 4, ldr + list);
  }
  Wr32(ldr + kLdrDllBase, base);
  Wr32(ldr + kLdrImageBase, base);
  Wr32(ldr + kLdrImageSize, size);
  UnicodeString(ldr + kLdrFullDllName, full_name);
  UnicodeString(ldr + kLdrBaseDllName, name);
  Wr32(ldr + kLdrFullImageSize, size);
  Wr32(ldr + kLdrEntryPoint, entry);
  Wr16(ldr + kLdrLoadCount, 1);
  return ldr;
}

}  // namespace

void LoadExecutableModule(const XexModule& m, const std::string& name) {
  uint32_t header = SystemAlloc(uint32_t(m.headers.size()));
  std::memcpy(Mem() + header, m.headers.data(), m.headers.size());
  // The system starts a title from its disc's root.
  uint32_t ldr =
      LoaderEntry("\\Device\\CdRom0\\" + name, name, m.base, uint32_t(m.image.size()), m.entry);
  if (uint32_t stamp = XexHeaderField(header, kXexChecksumTimestamp))
    Wr32(ldr + kLdrTimeDateStamp, Rd32(stamp + 4));
  Wr32(ldr + kLdrXexHeader, header);
  g_ldr = ldr;
  g_title_id = m.title_id;
  g_name = name;
  g_exports = m.exports;
  g_export_base = m.export_base;
}

// The system modules' entries: the area of their exports' addresses as
// the image (chosen: the console's modules are images in system memory),
// no entry point.
void LoadSystemModules() {
  MapSystemExports();
  for (const char* name : kSystemModules) {
    uint32_t base = 0, size = 0;
    SystemExportArea(name, base, size);
    g_system.push_back({name, LoaderEntry(std::string("\\SystemRoot\\") + name, name, base, size, 0)});
  }
}

uint32_t SystemModule(const std::string& name) {
  for (const SystemEntry& e : g_system)
    if (SameName(name, e.name)) return e.ldr;
  return 0;
}

const char* SystemModuleName(uint32_t handle) {
  for (const SystemEntry& e : g_system)
    if (handle && handle == e.ldr) return e.name;
  return nullptr;
}

uint32_t ExecutableModule() { return g_ldr; }
uint32_t ExecutableTitleId() { return g_title_id; }
const std::string& ExecutableModuleName() { return g_name; }

uint32_t ExecutableExport(uint32_t ordinal) {
  if (ordinal < g_export_base || ordinal - g_export_base >= g_exports.size()) return 0;
  return g_exports[ordinal - g_export_base];
}

// Titles read the data of keys with low byte 1 through the address
// returned (a title reads 0x00020401's dword so), xam that of larger ones
// (0x00040006's title id at 0xC); that a key with low byte 0 returns the
// value itself is Xenia's, awaiting the console check.
uint32_t XexHeaderField(uint32_t header, uint32_t key) {
  if (!header) return 0;
  uint32_t count = Rd32(header + 20);
  for (uint32_t k = 0; k < count; k++) {
    uint32_t entry = header + 24 + 8 * k;
    if (Rd32(entry) != key) continue;
    switch (key & 0xFF) {
      case 0: return Rd32(entry + 4);
      case 1: return entry + 4;
      default: return header + Rd32(entry + 4);
    }
  }
  return 0;
}

}  // namespace recomp::xbox360
