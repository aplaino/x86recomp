// The NRO reader (horizon/nso_image.h) on headers that do not describe an
// image: segments out of order or overlapping, a MOD0 offset past the
// image. Each must be refused, never copied or read past the image.
//   horizon_nro_test <scratch file>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "horizon/nso_image.h"

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (ok) return;
  std::printf("FAIL %s\n", what);
  failures++;
}

template <typename T>
void Put(std::vector<uint8_t>& b, uint64_t off, T v) {
  std::memcpy(b.data() + off, &v, sizeof(T));
}

struct Seg {
  uint32_t off, size;
};

// An NRO of 0x3000 bytes with these text, read-only and data segments and
// MOD0 at `mod0`, its .dynamic (empty) at 0x2000.
std::vector<uint8_t> Nro(const Seg (&segs)[3], uint32_t mod0 = 0x80) {
  std::vector<uint8_t> f(0x3000, 0);
  Put<uint32_t>(f, 0x04, mod0);
  std::memcpy(f.data() + 0x10, "NRO0", 4);
  Put<uint32_t>(f, 0x18, uint32_t(f.size()));
  for (int i = 0; i < 3; i++) {
    Put(f, 0x20 + i * 8, segs[i].off);
    Put(f, 0x24 + i * 8, segs[i].size);
  }
  std::memcpy(f.data() + 0x80, "MOD0", 4);
  Put<int32_t>(f, 0x84, 0x2000 - 0x80);
  return f;
}

bool Load(const char* path, const std::vector<uint8_t>& f) {
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
  }
  recomp::a64::Module m;
  std::string err;
  return recomp::horizon::LoadNro(path, m, err);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: horizon_nro_test scratch-file\n");
    return 2;
  }
  const char* path = argv[1];
  Check(Load(path, Nro({{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x1000}})), "a well-formed NRO is read");
  // Read-only data after data, whose end sizes the image.
  Check(!Load(path, Nro({{0, 0x1000}, {0x2000, 0x1000}, {0x1000, 0x100}})),
        "segments out of order are refused");
  Check(!Load(path, Nro({{0, 0x1000}, {0, 0x1000}, {0x2000, 0x1000}})), "overlapping segments are refused");
  Check(!Load(path, Nro({{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x1000}}, 0xFFFFFFF0u)),
        "a MOD0 offset that wraps is refused");
  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
