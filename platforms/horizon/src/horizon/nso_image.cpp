#include "horizon/nso_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "a64/text_hash.h"

namespace recomp::horizon {
namespace {

template <typename T>
T Get(const std::vector<uint8_t>& b, uint64_t off) {
  T v{};
  if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
  return v;
}

std::string Hex(uint64_t v) {
  char s[24];
  std::snprintf(s, sizeof s, "0x%llx", static_cast<unsigned long long>(v));
  return s;
}

constexpr uint32_t kPage = 0x1000;
uint64_t PageUp(uint64_t v) { return (v + kPage - 1) & ~uint64_t(kPage - 1); }

// ELF dynamic tags used below.
enum : int64_t {
  kDtNull = 0, kDtPltRelSz = 2, kDtHash = 4, kDtStrTab = 5, kDtSymTab = 6,
  kDtRela = 7, kDtRelaSz = 8, kDtRelaEnt = 9, kDtStrSz = 10, kDtSymEnt = 11,
  kDtPltRel = 20, kDtJmpRel = 23,
};

bool ReadDynamic(Module& m, std::string& err) {
  uint64_t strtab = 0, strsz = 0, symtab = 0, hash = 0, rela = 0, relasz = 0,
           jmprel = 0, pltrelsz = 0;
  for (uint64_t p = m.dynamic;; p += 16) {
    if (p + 16 > m.image.size()) {
      err = ".dynamic runs off the image";
      return false;
    }
    int64_t tag = Get<int64_t>(m.image, p);
    uint64_t val = Get<uint64_t>(m.image, p + 8);
    if (tag == kDtNull) break;
    switch (tag) {
      case kDtStrTab: strtab = val; break;
      case kDtStrSz: strsz = val; break;
      case kDtSymTab: symtab = val; break;
      case kDtHash: hash = val; break;
      case kDtRela: rela = val; break;
      case kDtRelaSz: relasz = val; break;
      case kDtJmpRel: jmprel = val; break;
      case kDtPltRelSz: pltrelsz = val; break;
      case kDtSymEnt:
        if (val != 24) { err = "DT_SYMENT is not 24"; return false; }
        break;
      case kDtRelaEnt:
        if (val != 24) { err = "DT_RELAENT is not 24"; return false; }
        break;
      case kDtPltRel:
        if (val != kDtRela) { err = "DT_PLTREL is not DT_RELA"; return false; }
        break;
      default: break;
    }
  }
  if (symtab) {
    if (!hash) {
      err = "no DT_HASH: cannot size .dynsym";
      return false;
    }
    uint32_t count = Get<uint32_t>(m.image, hash + 4);  // nchain
    for (uint32_t i = 0; i < count; i++) {
      uint64_t s = symtab + uint64_t(i) * 24;
      uint32_t name = Get<uint32_t>(m.image, s);
      uint8_t info = Get<uint8_t>(m.image, s + 4);
      uint16_t shndx = Get<uint16_t>(m.image, s + 6);
      Symbol sym;
      if (name < strsz) {
        const char* p = reinterpret_cast<const char*>(m.image.data() + strtab + name);
        sym.name.assign(p, strnlen(p, strsz - name));
      }
      sym.value = Get<uint64_t>(m.image, s + 8);
      sym.size = Get<uint64_t>(m.image, s + 16);
      sym.func = (info & 0xF) == 2;
      sym.defined = shndx != 0;
      m.symbols.push_back(std::move(sym));
    }
  }
  auto read_relocs = [&](uint64_t at, uint64_t size) {
    for (uint64_t p = at; p + 24 <= at + size; p += 24) {
      uint64_t info = Get<uint64_t>(m.image, p + 8);
      m.relocs.push_back({Get<uint64_t>(m.image, p), uint32_t(info),
                          uint32_t(info >> 32), Get<int64_t>(m.image, p + 16)});
    }
  };
  read_relocs(rela, relasz);
  read_relocs(jmprel, pltrelsz);
  return true;
}

// DWARF exception-header pointer encodings (LSB "Linux Standard Base Core
// Specification", .eh_frame), decoded in image-offset space.
struct Reader {
  const std::vector<uint8_t>& b;
  uint64_t p;
  uint64_t Uleb() {
    uint64_t v = 0;
    for (int s = 0; p < b.size(); s += 7) {
      uint8_t x = b[p++];
      if (s < 64) v |= uint64_t(x & 0x7F) << s;
      if (!(x & 0x80)) break;
    }
    return v;
  }
  int64_t Sleb() {
    uint64_t v = 0;
    int s = 0;
    uint8_t x = 0;
    do {
      x = p < b.size() ? b[p++] : 0;
      if (s < 64) v |= uint64_t(x & 0x7F) << s;
      s += 7;
    } while (x & 0x80);
    if (s < 64 && (x & 0x40)) v |= ~uint64_t(0) << s;
    return int64_t(v);
  }
  // Returns false for encodings this decoder does not know.
  bool Ptr(uint8_t enc, uint64_t datarel, uint64_t& out) {
    if (enc == 0xFF) return false;
    uint64_t at = p, v = 0;
    switch (enc & 0x0F) {
      case 0x00: v = Get<uint64_t>(b, p); p += 8; break;
      case 0x01: v = Uleb(); break;
      case 0x02: v = Get<uint16_t>(b, p); p += 2; break;
      case 0x03: v = Get<uint32_t>(b, p); p += 4; break;
      case 0x04: v = Get<uint64_t>(b, p); p += 8; break;
      case 0x09: v = uint64_t(Sleb()); break;
      case 0x0A: v = uint64_t(int64_t(Get<int16_t>(b, p))); p += 2; break;
      case 0x0B: v = uint64_t(int64_t(Get<int32_t>(b, p))); p += 4; break;
      case 0x0C: v = Get<uint64_t>(b, p); p += 8; break;
      default: return false;
    }
    switch (enc & 0x70) {
      case 0x00: break;
      case 0x10: v += at; break;
      case 0x30: v += datarel; break;
      default: return false;
    }
    out = v;
    return true;
  }
};

// The FDE pointer encoding ('R') from a CIE's augmentation data.
bool CieFdeEncoding(const std::vector<uint8_t>& b, uint64_t cie, uint8_t& enc) {
  Reader r{b, cie};
  uint32_t len = Get<uint32_t>(b, r.p);
  r.p += 4;
  if (len == 0xFFFFFFFF) r.p += 8;
  r.p += 4;  // CIE id
  uint8_t version = Get<uint8_t>(b, r.p++);
  std::string aug;
  while (r.p < b.size() && b[r.p]) aug += char(b[r.p++]);
  r.p++;
  enc = 0x00;  // absptr unless 'R' says otherwise
  if (aug.empty()) return true;
  if (aug[0] != 'z') return false;
  r.Uleb();                          // code alignment
  r.Sleb();                          // data alignment
  if (version == 1) r.p++; else r.Uleb();  // return address register
  r.Uleb();                          // augmentation data length
  for (size_t i = 1; i < aug.size(); i++) {
    switch (aug[i]) {
      case 'R': enc = b[r.p++]; break;
      case 'L': r.p++; break;
      case 'P': {
        uint8_t penc = b[r.p++];
        uint64_t ignored;
        if (!r.Ptr(penc & 0x7F, 0, ignored)) return false;
        break;
      }
      case 'S': case 'B': break;
      default: return false;
    }
  }
  return true;
}

}  // namespace

bool LoadNro(const std::string& path, Module& m, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = path + ": cannot open";
    return false;
  }
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
  if (file.size() < 0x80 || std::memcmp(file.data() + 0x10, "NRO0", 4)) {
    err = path + ": not an NRO";
    return false;
  }
  m = Module{};
  m.path = path;
  Segment* segs[3] = {&m.text, &m.ro, &m.data};
  for (int i = 0; i < 3; i++) {
    segs[i]->off = Get<uint32_t>(file, 0x20 + i * 8);
    segs[i]->size = Get<uint32_t>(file, 0x24 + i * 8);
    if (uint64_t(segs[i]->off) + segs[i]->size > file.size() || segs[i]->off % kPage) {
      err = path + ": segment " + std::to_string(i) + " is outside the file or unaligned";
      return false;
    }
  }
  m.bss.off = m.data.off + m.data.size;
  m.bss.size = Get<uint32_t>(file, 0x38);
  m.image.assign(PageUp(uint64_t(m.bss.off) + m.bss.size), 0);
  for (Segment* s : segs) std::memcpy(m.image.data() + s->off, file.data() + s->off, s->size);

  m.mod0 = Get<uint32_t>(file, 0x04);
  if (m.mod0 + 0x1C > m.image.size() || std::memcmp(m.image.data() + m.mod0, "MOD0", 4)) {
    err = path + ": no MOD0 header at " + Hex(m.mod0);
    return false;
  }
  auto mod0_field = [&](int i) { return m.mod0 + Get<int32_t>(m.image, m.mod0 + 4 + i * 4); };
  m.dynamic = mod0_field(0);
  uint32_t hdr_start = mod0_field(3), hdr_end = mod0_field(4);
  if (hdr_end > hdr_start) {
    m.eh_frame_hdr = hdr_start;
    m.eh_frame_hdr_size = hdr_end - hdr_start;
  }
  if (!ReadDynamic(m, err)) {
    err = path + ": " + err;
    return false;
  }
  return true;
}

bool Relocate(Module& m, uint64_t base, std::string& err) {
  for (const Reloc& r : m.relocs) {
    if (r.type != kRelocAarch64Relative) {
      err = m.path + ": relocation type " + std::to_string(r.type) + " at " + Hex(r.off) +
            " is not supported";
      return false;
    }
    if (r.off + 8 > m.image.size()) {
      err = m.path + ": relocation at " + Hex(r.off) + " is outside the image";
      return false;
    }
    uint64_t v = base + uint64_t(r.addend);
    std::memcpy(m.image.data() + r.off, &v, 8);
  }
  return true;
}

uint64_t TextHash(const Module& m) { return a64::TextHash(m.image.data() + m.text.off, m.text.size); }

std::vector<FdeRange> EhFrameFunctions(const Module& m) {
  std::vector<FdeRange> out;
  if (!m.eh_frame_hdr) return out;
  const auto& b = m.image;
  uint64_t hdr = m.eh_frame_hdr;
  if (Get<uint8_t>(b, hdr) != 1) return out;
  uint8_t ptr_enc = b[hdr + 1], count_enc = b[hdr + 2], table_enc = b[hdr + 3];
  Reader r{b, hdr + 4};
  uint64_t eh_frame = 0, count = 0;
  if (!r.Ptr(ptr_enc, hdr, eh_frame) || !r.Ptr(count_enc, hdr, count)) return out;
  for (uint64_t i = 0; i < count; i++) {
    uint64_t loc = 0, fde = 0;
    if (!r.Ptr(table_enc, hdr, loc) || !r.Ptr(table_enc, hdr, fde)) break;
    Reader f{b, fde};
    uint64_t len = Get<uint32_t>(b, f.p);
    f.p += 4;
    if (len == 0xFFFFFFFF) f.p += 8;
    uint64_t cie_field = f.p;
    uint64_t cie = cie_field - Get<uint32_t>(b, f.p);
    f.p += 4;
    uint8_t enc = 0;
    if (!CieFdeEncoding(b, cie, enc)) continue;
    uint64_t start = 0, size = 0;
    if (!f.Ptr(enc, 0, start) || !f.Ptr(enc & 0x0F, 0, size)) continue;
    out.push_back({start, size});
  }
  return out;
}

}  // namespace recomp::horizon
