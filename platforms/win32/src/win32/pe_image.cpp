#include "win32/pe_image.h"

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
  uint16_t machine = Rd<uint16_t>(d, pe + 4);
  if (Rd<uint32_t>(d, pe) != 0x00004550 || (machine != 0x14C && machine != 0x8664)) {
    *err = "not a PE32 i386 or PE32+ AMD64 image";
    return false;
  }
  x64 = machine == 0x8664;
  uint16_t nsec = Rd<uint16_t>(d, pe + 6);
  uint16_t opt_size = Rd<uint16_t>(d, pe + 20);
  const size_t oh = size_t(pe) + 24, sh = oh + opt_size;
  if (sh + size_t(nsec) * 40 > d.size()) {
    *err = "section table runs past end of file";
    return false;
  }
  if (Rd<uint16_t>(d, oh) != (x64 ? 0x20B : 0x10B)) {
    *err = x64 ? "optional header is not PE32+" : "optional header is not PE32";
    return false;
  }
  dll = (Rd<uint16_t>(d, pe + 22) & kFileDll) != 0;
  entry = Rd<uint32_t>(d, oh + 16);
  // PE32+ widens ImageBase and the stack and heap sizes to 64 bits (and
  // drops BaseOfData), which moves the data directories by 16 bytes.
  preferred_base = x64 ? Rd<uint64_t>(d, oh + 24) : Rd<uint32_t>(d, oh + 28);
  image_base = base ? base : preferred_base;
  image_size = Rd<uint32_t>(d, oh + 56);
  uint32_t headers_size = Rd<uint32_t>(d, oh + 60);
  stack_reserve = x64 ? Rd<uint64_t>(d, oh + 72) : Rd<uint32_t>(d, oh + 72);
  if (!image_size || headers_size > image_size) {
    *err = "headers do not fit in the image";
    return false;
  }
  if (entry) entry += image_base;  // DLLs may have no entry point
  // The data directories: NumberOfRvaAndSizes of them, inside the optional
  // header; any other is empty.
  const size_t dirs = oh + (x64 ? 112 : 96);
  const uint32_t ndirs = Rd<uint32_t>(d, oh + (x64 ? 108 : 92));
  auto dir = [&](uint32_t i) {
    if (i >= ndirs || dirs + 8 * (i + 1) > sh) return std::pair<uint32_t, uint32_t>{0, 0};
    return std::pair<uint32_t, uint32_t>{Rd<uint32_t>(d, dirs + i * 8),
                                         Rd<uint32_t>(d, dirs + 4 + i * 8)};
  };
  // Everything the tables below point at must lie inside the image.
  auto inside = [&](uint64_t rva, uint64_t n) { return rva <= image_size && n <= image_size - rva; };
  auto bad = [&](const char* what) {
    *err = std::string(what) + " lies outside the image";
    return false;
  };
  // A NUL-terminated string at rva, ending inside the image.
  auto str = [&](uint64_t rva, std::string* out) {
    if (rva >= image_size) return false;
    const char* p = mem.ptr<const char>(image_base + rva);
    const size_t n = strnlen(p, size_t(image_size - rva));
    if (n == image_size - rva) return false;
    out->assign(p, n);
    return true;
  };
  const uint32_t ptr_size = x64 ? 8 : 4;
  auto rd_ptr = [&](gaddr va) -> uint64_t {
    return x64 ? mem.read<uint64_t>(va) : mem.read<uint32_t>(va);
  };

  if (image_base + image_size > mem.size()) {
    *err = "image does not fit in the guest arena";
    return false;
  }
  for (uint32_t a = 0; a < image_size; a += GuestMemory::kPageSize)
    if (mem.IsCommitted(image_base + a)) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "image range %08llX-%08llX is already in use",
                    (unsigned long long)image_base,
                    (unsigned long long)(image_base + image_size));
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
    if (uint64_t(rptr) + n > d.size()) {
      *err = std::string("section ") + name + " runs past end of file";
      return false;
    }
    if (!inside(va, std::max(vsize, n))) {
      *err = std::string("section ") + name + " lies outside the image";
      return false;
    }
    std::memcpy(mem.ptr(image_base + va), d.data() + rptr, n);
    sections.push_back({name, image_base + va, vsize, ch});
  }

  // Base relocations: add the load delta to every HIGHLOW (and, in x64
  // images, DIR64) fixup.
  if (image_base != preferred_base) {
    auto [rel_rva, rel_size] = dir(5);
    if (!rel_size) {
      *err = "image has no relocations and cannot move from its preferred base";
      return false;
    }
    if (!inside(rel_rva, rel_size)) return bad("relocation directory");
    const uint64_t delta = image_base - preferred_base;
    const gaddr rel_end = image_base + rel_rva + rel_size;
    for (gaddr blk = image_base + rel_rva; blk + 8 <= rel_end;) {
      uint32_t page = mem.read<uint32_t>(blk), size = mem.read<uint32_t>(blk + 4);
      if (size < 8) break;
      if (size > rel_end - blk) return bad("relocation block");
      for (uint32_t i = 8; i + 2 <= size; i += 2) {
        uint16_t e = mem.read<uint16_t>(blk + i);
        const uint64_t rva = uint64_t(page) + (e & 0xFFF);
        gaddr at = image_base + rva;
        switch (e >> 12) {
          case 0:  // IMAGE_REL_BASED_ABSOLUTE: padding
            break;
          case 3:  // IMAGE_REL_BASED_HIGHLOW
            if (!inside(rva, 4)) return bad("relocation");
            mem.write<uint32_t>(at, uint32_t(mem.read<uint32_t>(at) + delta));
            break;
          case 10:  // IMAGE_REL_BASED_DIR64
            if (!inside(rva, 8)) return bad("relocation");
            mem.write<uint64_t>(at, mem.read<uint64_t>(at) + delta);
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
  const gaddr thunk_base = x64 ? kThunkBase64 : kThunkBase;
  if (!bind && !mem.Commit(thunk_base, kThunkRegionSize, Prot::R)) {
    *err = "cannot commit thunk region";
    return false;
  }
  auto [imp_rva, imp_size] = dir(1);
  for (uint64_t desc_rva = imp_rva; imp_size; desc_rva += 20) {
    if (!inside(desc_rva, 20)) return bad("import descriptor");
    const gaddr desc = image_base + desc_rva;
    uint32_t ilt = mem.read<uint32_t>(desc);
    uint32_t name_rva = mem.read<uint32_t>(desc + 12);
    uint32_t iat = mem.read<uint32_t>(desc + 16);
    if (!name_rva) break;
    std::string dll;
    if (!str(name_rva, &dll)) return bad("import DLL name");
    uint64_t lookup_rva = ilt ? ilt : iat;
    for (uint64_t slot_rva = iat;; slot_rva += ptr_size, lookup_rva += ptr_size) {
      if (!inside(lookup_rva, ptr_size) || !inside(slot_rva, ptr_size))
        return bad("import lookup table");
      const gaddr slot = image_base + slot_rva;
      uint64_t v = rd_ptr(image_base + lookup_rva);
      if (!v) break;
      const uint64_t by_ordinal = x64 ? 1ull << 63 : 1ull << 31;
      std::string sym;
      if (v & by_ordinal)
        sym = "#" + std::to_string(v & 0xFFFF);
      else if (!str(uint64_t(uint32_t(v)) + 2, &sym))
        return bad("import name");
      gaddr thunk = bind ? bind(dll, sym)
                         : thunk_base + uint32_t(imports.size()) * kThunkStride;
      if (!bind && thunk >= thunk_base + kThunkRegionSize) {
        *err = "thunk region exhausted";
        return false;
      }
      if (x64)
        mem.write<uint64_t>(slot, thunk);
      else
        mem.write<uint32_t>(slot, uint32_t(thunk));
      imports.push_back({dll, sym, slot, thunk});
    }
  }

  auto [exp_rva, exp_size] = dir(0);
  if (exp_size) {
    if (!inside(exp_rva, 40)) return bad("export directory");
    gaddr e = image_base + exp_rva;
    uint32_t ord_base = mem.read<uint32_t>(e + 16);
    uint32_t nfuncs = mem.read<uint32_t>(e + 20), nnames = mem.read<uint32_t>(e + 24);
    uint32_t funcs_rva = mem.read<uint32_t>(e + 28), names_rva = mem.read<uint32_t>(e + 32),
             ords_rva = mem.read<uint32_t>(e + 36);
    if (!inside(funcs_rva, 4ull * nfuncs) || !inside(names_rva, 4ull * nnames) ||
        !inside(ords_rva, 2ull * nnames))
      return bad("export table");
    gaddr funcs = image_base + funcs_rva, names = image_base + names_rva,
          ords = image_base + ords_rva;
    std::vector<std::string> by_index(nfuncs);
    for (uint32_t i = 0; i < nnames; i++) {
      uint16_t idx = mem.read<uint16_t>(ords + 2 * i);
      if (idx < nfuncs && !str(mem.read<uint32_t>(names + 4 * i), &by_index[idx]))
        return bad("export name");
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
    // IMAGE_TLS_DIRECTORY: four VAs, then SizeOfZeroFill.
    if (!inside(tls_rva, 4 * ptr_size + 4)) return bad("TLS directory");
    gaddr t = image_base + tls_rva;
    tls.raw_start = rd_ptr(t);
    tls.raw_end = rd_ptr(t + ptr_size);
    tls.index_va = rd_ptr(t + 2 * ptr_size);
    tls.callbacks = rd_ptr(t + 3 * ptr_size);
    tls.zero_fill = mem.read<uint32_t>(t + 4 * ptr_size);
    // The VAs are relocated already: they are inside the image as mapped.
    auto in_image = [&](gaddr va, uint64_t n) {
      return va >= image_base && inside(va - image_base, n);
    };
    if (tls.raw_end < tls.raw_start || !in_image(tls.raw_start, tls.raw_end - tls.raw_start) ||
        !in_image(tls.index_va, 4) || (tls.callbacks && !in_image(tls.callbacks, ptr_size)))
      return bad("TLS template, index or callbacks");
    has_tls = true;
  }

  // x64 exception directory: RUNTIME_FUNCTION {begin, end, unwind} RVAs.
  // An unwind RVA with bit 0 set points at the parent's RUNTIME_FUNCTION
  // (a chain in short form).
  auto [exc_rva, exc_size] = dir(3);
  if (x64 && exc_size && !inside(exc_rva, exc_size)) return bad("exception directory");
  if (x64)
    for (uint32_t o = 0; o + 12 <= exc_size; o += 12) {
      gaddr e = image_base + exc_rva + o;
      uint32_t b = mem.read<uint32_t>(e), en = mem.read<uint32_t>(e + 4);
      uint32_t u = mem.read<uint32_t>(e + 8);
      if (!b) continue;
      if (!inside(u & ~1u, (u & 1) ? 12 : 4)) return bad("unwind information");
      uint8_t fl = (u & 1) ? 4 : mem.read<uint8_t>(image_base + u) >> 3;
      functions.push_back({e, image_base + b, image_base + en, image_base + (u & ~1u), fl});
    }

  mem.Protect(image_base, headers_size, Prot::R);
  for (const auto& s : sections)
    mem.Protect(s.va, s.vsize, SectionProt(s.characteristics));
  return true;
}

bool PeImage::IsX64File(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> d(0x1000);
  f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
  d.resize(size_t(f.gcount()));
  uint32_t pe = Rd<uint32_t>(d, 0x3C);
  return Rd<uint16_t>(d, 0) == 0x5A4D && Rd<uint32_t>(d, pe) == 0x00004550 &&
         Rd<uint16_t>(d, pe + 4) == 0x8664;
}

}  // namespace recomp
