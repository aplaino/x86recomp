#include "runtime/pe_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace recomp {

namespace {

constexpr uint32_t kScnMemWrite = 0x80000000;
constexpr uint16_t kFileDll = 0x2000;

template <typename T>
T Rd(const std::vector<uint8_t>& d, size_t off) {
  T v{};
  if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof(T));
  return v;
}

Prot SectionProt(uint32_t ch) {
  // Guest code is lifted, never executed, so .text only needs to be
  // readable (jump tables and constants live there).
  return (ch & kScnMemWrite) ? Prot::RW : Prot::R;
}

std::string CStr(const GuestMemory& mem, gaddr va) {
  return std::string(mem.ptr<const char>(va));
}

}  // namespace

bool PeImage::Load(const std::string& path, GuestMemory& mem,
                   std::string* err, gaddr base, const ImportBinder& bind) {
  *this = PeImage{};
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

  if (Rd<uint16_t>(d, 0) != 0x5A4D) {
    *err = "missing MZ header";
    return false;
  }
  uint32_t pe = Rd<uint32_t>(d, 0x3C);
  if (Rd<uint32_t>(d, pe) != 0x00004550 || Rd<uint16_t>(d, pe + 4) != 0x14C) {
    *err = "not a PE32 i386 image";
    return false;
  }
  uint16_t nsec = Rd<uint16_t>(d, pe + 6);
  uint16_t opt_size = Rd<uint16_t>(d, pe + 20);
  size_t oh = pe + 24;
  if (Rd<uint16_t>(d, oh) != 0x10B) {
    *err = "optional header is not PE32";
    return false;
  }
  dll = (Rd<uint16_t>(d, pe + 22) & kFileDll) != 0;
  entry = Rd<uint32_t>(d, oh + 16);
  preferred_base = Rd<uint32_t>(d, oh + 28);
  image_base = base ? base : preferred_base;
  image_size = Rd<uint32_t>(d, oh + 56);
  uint32_t headers_size = Rd<uint32_t>(d, oh + 60);
  stack_reserve = Rd<uint32_t>(d, oh + 72);
  if (entry) entry += image_base;  // DLLs may have no entry point
  auto dir = [&](int i) {
    return std::pair<uint32_t, uint32_t>{Rd<uint32_t>(d, oh + 96 + i * 8),
                                         Rd<uint32_t>(d, oh + 100 + i * 8)};
  };

  if (uint64_t(image_base) + image_size > GuestMemory::kSize) {
    *err = "image does not fit below 4 GiB";
    return false;
  }
  for (uint32_t a = 0; a < image_size; a += GuestMemory::kPageSize)
    if (mem.IsCommitted(image_base + a)) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "image range %08X-%08X is already in use",
                    image_base, image_base + image_size);
      *err = buf;
      return false;
    }

  // Map everything RW first so imports can be patched, then tighten.
  if (!mem.Commit(image_base, image_size, Prot::RW)) {
    *err = "cannot commit image range";
    return false;
  }
  std::memcpy(mem.ptr(image_base), d.data(),
              std::min<size_t>(headers_size, d.size()));

  size_t sh = oh + opt_size;
  for (uint16_t i = 0; i < nsec; i++) {
    size_t o = sh + i * 40;
    char name[9] = {};
    std::memcpy(name, d.data() + o, 8);
    uint32_t vsize = Rd<uint32_t>(d, o + 8);
    uint32_t va = Rd<uint32_t>(d, o + 12);
    uint32_t rsize = Rd<uint32_t>(d, o + 16);
    uint32_t rptr = Rd<uint32_t>(d, o + 20);
    uint32_t ch = Rd<uint32_t>(d, o + 36);
    uint32_t n = std::min(vsize ? vsize : rsize, rsize);
    if (rptr + n > d.size()) {
      *err = std::string("section ") + name + " runs past end of file";
      return false;
    }
    std::memcpy(mem.ptr(image_base + va), d.data() + rptr, n);
    sections.push_back({name, image_base + va, vsize, ch});
  }

  // Base relocations: add the load delta to every HIGHLOW fixup.
  if (image_base != preferred_base) {
    auto [rel_rva, rel_size] = dir(5);
    if (!rel_size) {
      *err = "image has no relocations and cannot move from its preferred base";
      return false;
    }
    const uint32_t delta = image_base - preferred_base;
    for (gaddr blk = image_base + rel_rva; blk < image_base + rel_rva + rel_size;) {
      uint32_t page = mem.read<uint32_t>(blk), size = mem.read<uint32_t>(blk + 4);
      if (size < 8) break;
      for (uint32_t i = 8; i + 2 <= size; i += 2) {
        uint16_t e = mem.read<uint16_t>(blk + i);
        gaddr at = image_base + page + (e & 0xFFF);
        switch (e >> 12) {
          case 0:  // IMAGE_REL_BASED_ABSOLUTE: padding
            break;
          case 3:  // IMAGE_REL_BASED_HIGHLOW
            mem.write<uint32_t>(at, mem.read<uint32_t>(at) + delta);
            break;
          default:
            *err = "unsupported base relocation type " + std::to_string(e >> 12);
            return false;
        }
      }
      blk += size;
    }
  }

  // Imports: point every IAT slot at its thunk address.
  if (!bind && !mem.Commit(kThunkBase, kThunkRegionSize, Prot::R)) {
    *err = "cannot commit thunk region";
    return false;
  }
  auto [imp_rva, imp_size] = dir(1);
  for (gaddr desc = image_base + imp_rva; imp_size; desc += 20) {
    uint32_t ilt = mem.read<uint32_t>(desc);
    uint32_t name_rva = mem.read<uint32_t>(desc + 12);
    uint32_t iat = mem.read<uint32_t>(desc + 16);
    if (!name_rva) break;
    std::string dll = CStr(mem, image_base + name_rva);
    gaddr lookup = image_base + (ilt ? ilt : iat);
    for (gaddr slot = image_base + iat;; slot += 4, lookup += 4) {
      uint32_t v = mem.read<uint32_t>(lookup);
      if (!v) break;
      std::string sym = (v & 0x80000000)
                            ? "#" + std::to_string(v & 0xFFFF)
                            : CStr(mem, image_base + v + 2);
      gaddr thunk = bind ? bind(dll, sym)
                         : kThunkBase + uint32_t(imports.size()) * kThunkStride;
      if (!bind && thunk >= kThunkBase + kThunkRegionSize) {
        *err = "thunk region exhausted";
        return false;
      }
      mem.write<uint32_t>(slot, thunk);
      imports.push_back({dll, sym, slot, thunk});
    }
  }

  auto [exp_rva, exp_size] = dir(0);
  if (exp_size) {
    gaddr e = image_base + exp_rva;
    uint32_t ord_base = mem.read<uint32_t>(e + 16);
    uint32_t nfuncs = mem.read<uint32_t>(e + 20), nnames = mem.read<uint32_t>(e + 24);
    gaddr funcs = image_base + mem.read<uint32_t>(e + 28);
    gaddr names = image_base + mem.read<uint32_t>(e + 32);
    gaddr ords = image_base + mem.read<uint32_t>(e + 36);
    std::vector<std::string> by_index(nfuncs);
    for (uint32_t i = 0; i < nnames; i++) {
      uint16_t idx = mem.read<uint16_t>(ords + 2 * i);
      if (idx < nfuncs)
        by_index[idx] = CStr(mem, image_base + mem.read<uint32_t>(names + 4 * i));
    }
    for (uint32_t i = 0; i < nfuncs; i++) {
      uint32_t rva = mem.read<uint32_t>(funcs + 4 * i);
      if (!rva || (rva >= exp_rva && rva < exp_rva + exp_size))
        continue;  // unused slot, or a forwarder string
      exports.push_back({by_index[i], ord_base + i, image_base + rva});
    }
  }

  auto [tls_rva, tls_size] = dir(9);
  if (tls_size) {
    gaddr t = image_base + tls_rva;
    tls.raw_start = mem.read<uint32_t>(t);
    tls.raw_end = mem.read<uint32_t>(t + 4);
    tls.index_va = mem.read<uint32_t>(t + 8);
    tls.zero_fill = mem.read<uint32_t>(t + 16);
    has_tls = true;
  }

  mem.Protect(image_base, headers_size, Prot::R);
  for (const auto& s : sections)
    mem.Protect(s.va, s.vsize, SectionProt(s.characteristics));
  return true;
}

}  // namespace recomp
