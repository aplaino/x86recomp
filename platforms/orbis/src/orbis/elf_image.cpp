#include "orbis/elf_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

#include "runtime/pe_image.h"  // the core's thunk region

namespace recomp::orbis {

namespace {

constexpr uint32_t kPtLoad = 1, kPtDynamic = 2, kPtTls = 7;
constexpr uint32_t kPtGnuEhFrame = 0x6474E550;
constexpr uint32_t kPtSceRelro = 0x61000010, kPtSceDynlibData = 0x61000000;
constexpr uint32_t kPfX = 1, kPfW = 2;

constexpr int64_t kDtInit = 0x0C, kDtFini = 0x0D;
constexpr int64_t kDtSceModuleInfo = 0x6100000D, kDtSceNeededModule = 0x6100000F;
constexpr int64_t kDtSceExportLib = 0x61000013, kDtSceImportLib = 0x61000015;
constexpr int64_t kDtSceJmpRel = 0x61000029, kDtScePltRelSz = 0x6100002D;
constexpr int64_t kDtSceRela = 0x6100002F, kDtSceRelaSz = 0x61000031;
constexpr int64_t kDtSceStrTab = 0x61000035, kDtSceStrSz = 0x61000037;
constexpr int64_t kDtSceSymTab = 0x61000039, kDtSceSymTabSz = 0x6100003F;

constexpr uint32_t kR64 = 1, kRGlobDat = 6, kRJumpSlot = 7, kRRelative = 8;
constexpr uint32_t kRDtpMod64 = 16, kRDtpOff64 = 17, kRTpOff64 = 18;

template <typename T>
T Rd(const std::vector<uint8_t>& d, size_t off) {
  T v{};
  if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof(T));
  return v;
}

// Library and module ids in symbol names: base64 digits, most
// significant first.
uint64_t DecodeId(const std::string& s) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  uint64_t v = 0;
  for (char c : s) {
    const char* p = std::strchr(kAlphabet, c);
    v = v * 64 + (p && c ? p - kAlphabet : 0);
  }
  return v;
}

// One DWARF pointer encoding (DW_EH_PE_*) at va, which moves past it.
// Only what .eh_frame uses: absolute, pc-relative and datarel (to
// `data`) applications of the fixed-size and LEB128 formats.
bool ReadEncoded(const GuestMemory& mem, gaddr* va, uint8_t enc, gaddr data, uint64_t* out) {
  const gaddr at = *va;
  uint64_t v;
  auto leb = [&](bool sign) {
    uint64_t r = 0;
    int shift = 0;
    uint8_t b;
    do {
      b = mem.read<uint8_t>((*va)++);
      if (shift < 64) r |= uint64_t(b & 0x7F) << shift;
      shift += 7;
    } while (b & 0x80);
    if (sign && shift < 64 && (b & 0x40)) r |= ~uint64_t(0) << shift;
    return r;
  };
  switch (enc & 0x0F) {
    case 0x00: case 0x04: v = mem.read<uint64_t>(at), *va += 8; break;
    case 0x01: v = leb(false); break;
    case 0x02: v = mem.read<uint16_t>(at), *va += 2; break;
    case 0x03: v = mem.read<uint32_t>(at), *va += 4; break;
    case 0x09: v = leb(true); break;
    case 0x0A: v = uint64_t(int64_t(mem.read<int16_t>(at))), *va += 2; break;
    case 0x0B: v = uint64_t(int64_t(mem.read<int32_t>(at))), *va += 4; break;
    case 0x0C: v = mem.read<uint64_t>(at), *va += 8; break;
    default: return false;
  }
  switch (enc & 0x70) {
    case 0x00: break;
    case 0x10: v += at; break;
    case 0x30: v += data; break;
    default: return false;
  }
  *out = v;
  return true;
}

uint64_t Uleb(const GuestMemory& mem, gaddr* va) {
  uint64_t v = 0;
  ReadEncoded(mem, va, 0x01, 0, &v);
  return v;
}

}  // namespace

bool ElfImage::IsElfFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  uint8_t h[20] = {};
  return f.read(reinterpret_cast<char*>(h), sizeof h) && std::memcmp(h, "\x7F" "ELF", 4) == 0 &&
         h[4] == 2 && h[5] == 1 && (h[18] | h[19] << 8) == 62;
}

bool ElfImage::Map(const std::string& path, GuestMemory& mem, std::string* err, gaddr base) {
  *this = ElfImage{};
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  file_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  const std::vector<uint8_t>& d = file_;
  if (!IsElfFile(path)) {
    *err = "not an x86-64 ELF (a signed SELF must be decrypted first)";
    return false;
  }
  type = Rd<uint16_t>(d, 16);
  if (type != 0xFE00 && type != 0xFE10 && type != 0xFE18) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "ELF type %04X is not an Orbis executable or PRX", type);
    *err = buf;
    return false;
  }
  const uint64_t e_entry = Rd<uint64_t>(d, 24), phoff = Rd<uint64_t>(d, 32);
  const uint16_t phentsize = Rd<uint16_t>(d, 54), phnum = Rd<uint16_t>(d, 56);
  for (uint16_t i = 0; i < phnum; i++) {
    const size_t o = phoff + size_t(i) * phentsize;
    phs_.push_back({Rd<uint32_t>(d, o), Rd<uint32_t>(d, o + 4), Rd<uint64_t>(d, o + 8),
                    Rd<uint64_t>(d, o + 16), Rd<uint64_t>(d, o + 32), Rd<uint64_t>(d, o + 40),
                    Rd<uint64_t>(d, o + 48)});
  }
  auto loadable = [](const Ph& p) { return p.type == kPtLoad || p.type == kPtSceRelro; };
  uint64_t lo = ~uint64_t(0), hi = 0;
  for (const Ph& p : phs_)
    if (loadable(p) && p.memsz) {
      lo = std::min(lo, p.va);
      hi = std::max(hi, p.va + p.memsz);
      if (p.off + p.filesz > d.size()) {
        *err = "a segment runs past the end of the file";
        return false;
      }
    }
  if (!hi) {
    *err = "no loadable segments";
    return false;
  }
  link_base = lo & ~uint64_t(GuestMemory::kPageSize - 1);
  if (type == 0xFE00) {
    if (base && base != link_base) {
      *err = "an ET_SCE_EXEC image cannot move from its link address";
      return false;
    }
    image_base = 0;
  } else if (type == 0xFE10) {
    image_base = base ? base - link_base : kExecBase;
  } else {
    if (!base) {
      *err = "a PRX needs a base";
      return false;
    }
    image_base = base - link_base;
  }

  map_lo_ = image_base + link_base;
  map_hi_ = image_base + hi;
  if (map_hi_ > mem.size()) {
    *err = "image does not fit in the guest arena";
    return false;
  }
  for (gaddr a = map_lo_; a < map_hi_; a += GuestMemory::kPageSize)
    if (mem.IsCommitted(a)) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "image range %llX-%llX is already in use",
                    (unsigned long long)map_lo_, (unsigned long long)map_hi_);
      *err = buf;
      return false;
    }
  // Map everything RW first so relocations can be applied; Link tightens.
  for (const Ph& p : phs_)
    if (loadable(p) && p.memsz) {
      if (!mem.Commit(image_base + p.va, p.memsz, Prot::RW)) {
        *err = "cannot commit a segment";
        return false;
      }
      std::memcpy(mem.ptr(image_base + p.va), d.data() + p.off, p.filesz);
      segments.push_back({image_base + p.va, p.memsz, p.flags});
    }
  entry = e_entry + image_base;
  for (const Ph& p : phs_)
    if (p.type == kPtTls && p.memsz) {
      tls.init_va = image_base + p.va;
      tls.init_size = p.filesz;
      tls.mem_size = p.memsz;
      tls.align = std::max<uint64_t>(p.align, 1);
      tls.module = 1;
      tls.offset = (p.memsz + tls.align - 1) / tls.align * tls.align;
      has_tls = true;
    }

  // Dynamic tags (PT_DYNAMIC and the dynlib data are read from the file).
  const Ph* dyn = nullptr;
  const Ph* dynlib = nullptr;
  for (const Ph& p : phs_) {
    if (p.type == kPtDynamic) dyn = &p;
    if (p.type == kPtSceDynlibData) dynlib = &p;
  }
  if (!dyn || !dynlib || dynlib->off + dynlib->filesz > d.size()) {
    *err = "no PT_DYNAMIC / PT_SCE_DYNLIBDATA";
    return false;
  }
  std::vector<std::pair<int64_t, uint64_t>> tags;
  std::map<int64_t, uint64_t> tag;
  for (uint64_t o = 0; o + 16 <= dyn->filesz; o += 16) {
    const int64_t t = Rd<int64_t>(d, dyn->off + o);
    if (!t) break;
    const uint64_t v = Rd<uint64_t>(d, dyn->off + o + 8);
    tags.push_back({t, v});
    tag.emplace(t, v);
  }
  const size_t lib0 = dynlib->off, lib_end = dynlib->off + dynlib->filesz;
  auto in_lib = [&](uint64_t off, uint64_t n) { return off <= dynlib->filesz && n <= dynlib->filesz - off; };
  if (!in_lib(tag[kDtSceStrTab], tag[kDtSceStrSz]) || !in_lib(tag[kDtSceSymTab], tag[kDtSceSymTabSz]) ||
      !in_lib(tag[kDtSceRela], tag[kDtSceRelaSz]) || !in_lib(tag[kDtSceJmpRel], tag[kDtScePltRelSz])) {
    *err = "a DT_SCE table runs past PT_SCE_DYNLIBDATA";
    return false;
  }
  rela_ = lib0 + tag[kDtSceRela];
  rela_size_ = tag[kDtSceRelaSz];
  jmprel_ = lib0 + tag[kDtSceJmpRel];
  jmprel_size_ = tag[kDtScePltRelSz];
  if (tag.count(kDtInit)) init = image_base + tag[kDtInit];
  if (tag.count(kDtFini)) fini = image_base + tag[kDtFini];
  const size_t strtab = lib0 + tag[kDtSceStrTab], strsz = tag[kDtSceStrSz];
  auto str = [&](uint64_t off) {
    std::string s;
    for (size_t i = strtab + off; i < strtab + strsz && i < lib_end && d[i]; i++) s += char(d[i]);
    return s;
  };
  std::map<uint64_t, std::string> import_libs, needed, export_libs, self;
  for (const auto& [t, v] : tags) {
    if (t == kDtSceImportLib) import_libs[v >> 48] = str(v & 0xFFFFFFFF);
    if (t == kDtSceNeededModule) needed[v >> 48] = str(v & 0xFFFFFFFF);
    if (t == kDtSceExportLib) export_libs[v >> 48] = str(v & 0xFFFFFFFF);
    if (t == kDtSceModuleInfo) self[v >> 48] = str(v & 0xFFFFFFFF);
  }

  // Symbols: defined ones resolve into the image; imported ones (section
  // index 0) are bound by Link.
  auto lookup = [](const std::map<uint64_t, std::string>& m, const std::string& id) {
    auto it = m.find(DecodeId(id));
    return it != m.end() ? it->second : id;
  };
  const size_t symtab = lib0 + tag[kDtSceSymTab];
  for (uint64_t o = 0; o + 24 <= tag[kDtSceSymTabSz]; o += 24) {
    Sym s{Rd<uint8_t>(d, symtab + o + 4), Rd<uint16_t>(d, symtab + o + 6),
          Rd<uint64_t>(d, symtab + o + 8)};
    const std::string name = str(Rd<uint32_t>(d, symtab + o));
    const size_t h1 = name.find('#'), h2 = name.find('#', h1 + 1);
    if (h1 != std::string::npos && h2 != std::string::npos && (s.info >> 4)) {
      const std::string nid = name.substr(0, h1), lib = name.substr(h1 + 1, h2 - h1 - 1),
                        mod = name.substr(h2 + 1);
      if (s.shndx) {
        exports.push_back({nid, lookup(export_libs, lib), lookup(self, mod),
                           uint8_t(s.info & 0xF), image_base + s.value});
      } else {
        s.import = int(imports.size());
        imports.push_back({nid, lookup(import_libs, lib), lookup(needed, mod),
                           uint8_t(s.info & 0xF), 0, {}});
      }
    }
    syms_.push_back(s);
  }
  return true;
}

bool ElfImage::Link(GuestMemory& mem, std::string* err, const ElfImportBinder& bind) {
  const std::vector<uint8_t>& d = file_;
  if (!bind && !mem.Commit(kThunkBase64, kThunkRegionSize, Prot::R)) {
    *err = "cannot commit thunk region";
    return false;
  }
  for (size_t i = 0; i < imports.size(); i++) {
    ElfImport& imp = imports[i];
    imp.value = bind ? bind(imp) : kThunkBase64 + gaddr(i) * kThunkStride;
    if (!bind && imp.value >= kThunkBase64 + kThunkRegionSize) {
      *err = "thunk region exhausted";
      return false;
    }
  }

  // Relocations: DT_SCE_RELA, then the PLT's DT_SCE_JMPREL.
  auto relocate = [&](size_t table, uint64_t size) {
    for (uint64_t o = 0; o + 24 <= size; o += 24) {
      const gaddr at = image_base + Rd<uint64_t>(d, table + o);
      const uint64_t info = Rd<uint64_t>(d, table + o + 8);
      const int64_t addend = Rd<int64_t>(d, table + o + 16);
      const uint32_t rtype = uint32_t(info), sym = uint32_t(info >> 32);
      if (at < map_lo_ || at + 8 > map_hi_) {
        *err = "a relocation targets outside the image";
        return false;
      }
      if (rtype == kRRelative) {
        mem.write<uint64_t>(at, image_base + addend);
        continue;
      }
      if (rtype != kR64 && rtype != kRGlobDat && rtype != kRJumpSlot && rtype != kRDtpMod64 &&
          rtype != kRDtpOff64 && rtype != kRTpOff64) {
        *err = "unsupported relocation type " + std::to_string(rtype);
        return false;
      }
      if (sym >= syms_.size()) {
        *err = "a relocation names symbol " + std::to_string(sym) + " of " + std::to_string(syms_.size());
        return false;
      }
      const Sym& s = syms_[sym];
      if (rtype == kRDtpMod64 || rtype == kRDtpOff64 || rtype == kRTpOff64) {
        // Thread-local: this module's own TLS (a symbol of another
        // module's is not supported).
        if (s.import >= 0 || !tls.module) {
          *err = s.import >= 0 ? "a TLS relocation names an imported symbol"
                               : "a TLS relocation in a module without PT_TLS";
          return false;
        }
        const uint64_t v = rtype == kRDtpMod64   ? tls.module
                           : rtype == kRDtpOff64 ? s.value + addend
                                                 : s.value + addend - tls.offset;
        mem.write<uint64_t>(at, v);
        continue;
      }
      // Lazy binding left each PLT slot pointing just past its stub's
      // jmp, at the push of the stub's index.
      if (rtype == kRJumpSlot && s.import >= 0) {
        const gaddr stub = image_base + mem.read<uint64_t>(at) - 6;
        if (stub >= map_lo_ && stub + 6 <= map_hi_ && mem.read<uint16_t>(stub) == 0x25FF &&
            stub + 6 + int64_t(mem.read<int32_t>(stub + 2)) == at)
          imports[s.import].plt = stub;
      }
      const gaddr target = s.import >= 0 ? imports[s.import].value : image_base + s.value;
      const gaddr v = target + (rtype == kR64 ? addend : 0);
      mem.write<uint64_t>(at, v);
      if (s.import >= 0 && v == target) imports[s.import].slots.push_back(at);
    }
    return true;
  };
  if (!relocate(rela_, rela_size_) || !relocate(jmprel_, jmprel_size_)) return false;

  // Function ranges from .eh_frame, found through PT_GNU_EH_FRAME's
  // eh_frame_ptr (the FDE count there may be omitted, so the FDEs are
  // walked to the terminator). Read after relocation, so absolute
  // encodings hold mapped addresses too.
  for (const Ph& p : phs_) {
    if (p.type != kPtGnuEhFrame) continue;
    const gaddr hdr = image_base + p.va;
    if (hdr < map_lo_ || hdr + 4 > map_hi_ || mem.read<uint8_t>(hdr) != 1) {
      *err = "bad .eh_frame_hdr";
      return false;
    }
    gaddr at = hdr + 4;
    uint64_t eh = 0;
    if (!ReadEncoded(mem, &at, mem.read<uint8_t>(hdr + 1), hdr, &eh) || eh < map_lo_ || eh >= map_hi_) {
      *err = "bad eh_frame_ptr";
      return false;
    }
    std::map<gaddr, uint8_t> cie_enc;  // CIE -> its FDE pointer encoding
    for (gaddr rec = eh; rec + 8 <= map_hi_;) {
      const uint32_t len = mem.read<uint32_t>(rec);
      if (!len || len == 0xFFFFFFFF) break;  // terminator (64-bit lengths: not used)
      const gaddr next = rec + 4 + len, id_at = rec + 4;
      if (next > map_hi_) break;
      const uint32_t cie_off = mem.read<uint32_t>(id_at);
      if (cie_off) {
        const gaddr cie = id_at - cie_off;
        auto it = cie_enc.find(cie);
        if (it == cie_enc.end()) {
          // version, augmentation, code and data alignment, return
          // register, then the augmentation data.
          gaddr c = cie + 8;
          const uint8_t version = mem.read<uint8_t>(c++);
          std::string aug;
          while (char ch = mem.read<char>(c++)) aug += ch;
          if (version >= 4) c += 2;  // address and segment selector sizes
          Uleb(mem, &c);
          uint64_t ignored;
          ReadEncoded(mem, &c, 0x09, 0, &ignored);
          if (version == 1) c++; else Uleb(mem, &c);
          uint8_t enc = 0;
          if (!aug.empty() && aug[0] == 'z') {
            Uleb(mem, &c);
            for (size_t k = 1; k < aug.size(); k++) {
              if (aug[k] == 'R') enc = mem.read<uint8_t>(c++);
              else if (aug[k] == 'L') c++;
              else if (aug[k] == 'P') {
                const uint8_t pe = mem.read<uint8_t>(c++);
                if (!ReadEncoded(mem, &c, pe & 0x7F, 0, &ignored)) break;
              }
            }
          }
          it = cie_enc.emplace(cie, enc).first;
        }
        gaddr q = id_at + 4;
        const gaddr field = q;
        const bool pcrel = (it->second & 0x70) == 0x10;
        uint64_t begin, range;
        if (ReadEncoded(mem, &q, it->second, 0, &begin) &&
            ReadEncoded(mem, &q, it->second & 0x0F, 0, &range) && range &&
            begin != (pcrel ? field : 0))
          functions.push_back({begin, begin + range});
      }
      rec = next;
    }
  }

  for (const ElfSegment& s : segments) {
    if (!(s.flags & kPfX)) {
      if (!(s.flags & kPfW)) rodata.push_back({s.va, s.va + s.size});
      continue;
    }
    if (text_hi) {
      *err = "more than one executable segment";
      return false;
    }
    text_lo = s.va;
    for (const ElfFunction& fn : functions)
      if (fn.begin >= s.va && fn.end <= s.va + s.size) text_hi = std::max(text_hi, fn.end);
    for (const ElfImport& imp : imports)
      if (imp.plt >= s.va && imp.plt + 16 <= s.va + s.size) text_hi = std::max(text_hi, imp.plt + 16);
    if (entry >= s.va && entry < s.va + s.size) text_hi = std::max(text_hi, entry + 1);
    rodata.push_back({text_hi, s.va + s.size});
  }

  // Guest code is lifted, never executed: text only needs to be readable.
  // Read-only first: a page shared with a writable segment stays writable.
  for (const ElfSegment& s : segments)
    if (!(s.flags & kPfW)) mem.Protect(s.va, s.size, Prot::R);
  for (const ElfSegment& s : segments)
    if (s.flags & kPfW) mem.Protect(s.va, s.size, Prot::RW);
  file_.clear();
  file_.shrink_to_fit();
  return true;
}

bool ElfImage::Load(const std::string& path, GuestMemory& mem, std::string* err,
                    gaddr base, const ElfImportBinder& bind) {
  return Map(path, mem, err, base) && Link(mem, err, bind);
}

}  // namespace recomp::orbis
