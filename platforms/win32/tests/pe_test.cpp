// recomp_pe (win32/pe_image.h): the loader refuses images whose headers
// or tables point outside the file or the image.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "win32/pe_image.h"

using namespace recomp;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

template <typename T>
void Put(std::vector<uint8_t>& d, size_t off, T v) {
  std::memcpy(d.data() + off, &v, sizeof v);
}

// A PE32 EXE at 0x400000: 0x2000 bytes of image, headers in the first
// page, one section (.text, RVA 0x1000, 0x1000 bytes of file at 0x200).
// File offset 0x200 + n is RVA 0x1000 + n.
constexpr gaddr kBase = 0x400000;
constexpr size_t kPe = 0x40, kOpt = kPe + 24, kDirs = kOpt + 96, kSec = kOpt + 0xE0;

std::vector<uint8_t> Image() {
  std::vector<uint8_t> d(0x1200);
  Put<uint16_t>(d, 0, 0x5A4D);
  Put<uint32_t>(d, 0x3C, kPe);
  Put<uint32_t>(d, kPe, 0x00004550);
  Put<uint16_t>(d, kPe + 4, 0x14C);
  Put<uint16_t>(d, kPe + 6, 1);
  Put<uint16_t>(d, kPe + 20, 0xE0);
  Put<uint16_t>(d, kPe + 22, 0x0102);
  Put<uint16_t>(d, kOpt, 0x10B);
  Put<uint32_t>(d, kOpt + 16, 0x1000);    // entry
  Put<uint32_t>(d, kOpt + 28, kBase);     // ImageBase
  Put<uint32_t>(d, kOpt + 56, 0x2000);    // SizeOfImage
  Put<uint32_t>(d, kOpt + 60, 0x200);     // SizeOfHeaders
  Put<uint32_t>(d, kOpt + 72, 0x100000);  // SizeOfStackReserve
  Put<uint32_t>(d, kOpt + 92, 16);        // NumberOfRvaAndSizes
  std::memcpy(d.data() + kSec, ".text", 5);
  Put<uint32_t>(d, kSec + 8, 0x1000);   // VirtualSize
  Put<uint32_t>(d, kSec + 12, 0x1000);  // VirtualAddress
  Put<uint32_t>(d, kSec + 16, 0x1000);  // SizeOfRawData
  Put<uint32_t>(d, kSec + 20, 0x200);   // PointerToRawData
  Put<uint32_t>(d, kSec + 36, 0x60000020);
  return d;
}

// Loads d at `base` (0: its preferred one); clears the range afterwards.
bool Load(GuestMemory& mem, const std::vector<uint8_t>& d, std::string* err, gaddr base = 0) {
  char path[] = "/tmp/pe_test_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0 || write(fd, d.data(), d.size()) != ssize_t(d.size())) {
    std::printf("cannot write %s\n", path);
    std::exit(1);
  }
  close(fd);
  PeImage img;
  bool ok = img.Load(path, mem, err, base, [](const std::string&, const std::string&) {
    return gaddr(0x7F000000);
  });
  unlink(path);
  mem.Decommit(base ? base : kBase, 0x2000);
  return ok;
}

void Loader(GuestMemory& mem) {
  std::string err;
  Check(Load(mem, Image(), &err), "a well-formed image loads");

  auto refused = [&](std::vector<uint8_t> d, const char* what) {
    std::string e;
    bool ok = Load(mem, d, &e);
    Check(!ok, what);
    if (!ok) std::printf("  %s: %s\n", what, e.c_str());
  };
  auto d = Image();
  Put<uint16_t>(d, kPe + 6, 0x100);
  refused(d, "section table past the end of the file");
  d = Image();
  Put<uint32_t>(d, kSec + 20, 0xFFFFFF00);  // rptr + size wraps in 32 bits
  refused(d, "section data past the end of the file");
  d = Image();
  Put<uint32_t>(d, kSec + 12, 0x1800);
  refused(d, "section outside the image");
  d = Image();
  Put<uint32_t>(d, kOpt + 60, 0x4000);
  refused(d, "headers larger than the image");
  d = Image();
  Put<uint32_t>(d, kDirs + 8, 0x10000);  // import directory
  Put<uint32_t>(d, kDirs + 12, 20);
  refused(d, "import directory outside the image");
  d = Image();
  Put<uint32_t>(d, kDirs + 8, 0x1000);
  Put<uint32_t>(d, kDirs + 12, 20);
  Put<uint32_t>(d, 0x200 + 12, 0x1FFC);  // name: no NUL before the end
  std::memcpy(d.data() + 0x200 + 0xFFC, "abcd", 4);
  refused(d, "import DLL name running off the image");
  d = Image();
  Put<uint32_t>(d, kDirs + 8, 0x1000);
  Put<uint32_t>(d, kDirs + 12, 20);
  Put<uint32_t>(d, 0x200 + 0, 0x1FFC);  // lookup table: its terminator past the end
  Put<uint32_t>(d, 0x200 + 12, 0x1300);
  Put<uint32_t>(d, 0x200 + 16, 0x1200);
  std::memcpy(d.data() + 0x200 + 0x300, "a.dll", 6);
  Put<uint32_t>(d, 0x200 + 0xFFC, 0x80000001);
  refused(d, "import lookup table running off the image");
  d = Image();
  Put<uint32_t>(d, kDirs + 40, 0x1000);  // relocations
  Put<uint32_t>(d, kDirs + 44, 0x10);
  Put<uint32_t>(d, 0x200, 0x1000);
  Put<uint32_t>(d, 0x204, 0x7FFFFFF0);  // block size beyond the directory
  {
    std::string e;
    bool ok = Load(mem, d, &e, 0x500000);
    Check(!ok, "relocation block beyond its directory");
    if (!ok) std::printf("  relocation block: %s\n", e.c_str());
  }
  d = Image();
  Put<uint32_t>(d, kDirs + 40, 0x1000);
  Put<uint32_t>(d, kDirs + 44, 0x10);
  Put<uint32_t>(d, 0x200, 0x1000);
  Put<uint32_t>(d, 0x204, 0x0A);
  Put<uint16_t>(d, 0x208, 0x3FFE);  // HIGHLOW at RVA 0x1FFE: 2 bytes left
  {
    std::string e;
    bool ok = Load(mem, d, &e, 0x500000);
    Check(!ok, "relocation at the end of the image");
    if (!ok) std::printf("  relocation: %s\n", e.c_str());
  }
  d = Image();
  Put<uint32_t>(d, kOpt + 92, 1);  // only the export directory exists
  Put<uint32_t>(d, kDirs + 8, 0x10000);
  Put<uint32_t>(d, kDirs + 12, 20);
  Check(Load(mem, d, &err), "directories past NumberOfRvaAndSizes are empty");
}

}  // namespace

int main() {
  GuestMemory mem;
  if (!mem.Reserve(32)) {
    std::printf("cannot set up guest memory\n");
    return 1;
  }
  Loader(mem);
  if (g_failures) return 1;
  std::printf("pe: ok\n");
  return 0;
}
