#include "xbox360/xex.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "text_hash.h"

#include "xbox360/lzx.h"

namespace recomp::xbox360 {
namespace {

// Optional header keys. The low byte of a key is 0 or 1 for a value held in
// the entry itself, 0xFF for data that starts with its own size, and the
// data's size in dwords otherwise.
enum : uint32_t {
  kFileFormat = 0x000003FF,
  kEntryPoint = 0x00010100,
  kImageBase = 0x00010201,
  kImportLibraries = 0x000103FF,
  kOriginalName = 0x000183FF,
  kExecutionInfo = 0x00040006,
  kTlsInfo = 0x00020104,
  kDefaultStackSize = 0x00020200,
};
constexpr uint32_t kSecurityLoadAddress = 0x110, kSecurityExportTable = 0x160;
constexpr uint16_t kNoEncryption = 0;
enum : uint16_t { kStored = 0, kBasic = 1, kNormal = 2 };

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}
  bool Has(uint64_t off, uint64_t n) const { return off + n <= d_.size(); }
  uint32_t U32(uint64_t off) const {
    if (!Has(off, 4)) return 0;
    return uint32_t(d_[off]) << 24 | uint32_t(d_[off + 1]) << 16 | uint32_t(d_[off + 2]) << 8 |
           d_[off + 3];
  }
  uint16_t U16(uint64_t off) const {
    if (!Has(off, 2)) return 0;
    return uint16_t(d_[off] << 8 | d_[off + 1]);
  }
  const uint8_t* At(uint64_t off) const { return d_.data() + off; }

 private:
  const std::vector<uint8_t>& d_;
};

uint32_t Le32(const std::vector<uint8_t>& d, uint64_t off) {
  if (off + 4 > d.size()) return 0;
  return uint32_t(d[off]) | uint32_t(d[off + 1]) << 8 | uint32_t(d[off + 2]) << 16 |
         uint32_t(d[off + 3]) << 24;
}
uint32_t Be32(const std::vector<uint8_t>& d, uint64_t off) {
  if (off + 4 > d.size()) return 0;
  return uint32_t(d[off]) << 24 | uint32_t(d[off + 1]) << 16 | uint32_t(d[off + 2]) << 8 |
         d[off + 3];
}

// The image from the data after the headers, by the file format info.
bool Image(const Reader& r, const std::vector<uint8_t>& file, uint32_t header_size,
           uint32_t ffi, uint32_t image_size, std::vector<uint8_t>& out, std::string& err) {
  if (r.U16(ffi + 4) != kNoEncryption) {
    err = "the XEX is encrypted; decrypt it first with xbox360_xex_decrypt and the owner's key";
    return false;
  }
  const uint8_t* data = file.data() + header_size;
  size_t data_size = file.size() - header_size;
  switch (r.U16(ffi + 6)) {
    case kStored:
      out.assign(data, data + std::min<size_t>(data_size, image_size));
      out.resize(image_size);
      return true;
    case kBasic: {
      // Runs of data and zeros: (data_size, zero_size) pairs.
      uint32_t info_size = r.U32(ffi);
      out.clear();
      size_t p = 0;
      for (uint32_t off = ffi + 8; off + 8 <= ffi + info_size; off += 8) {
        uint32_t n = r.U32(off), zeros = r.U32(off + 4);
        if (p + n > data_size) {
          err = "basic compression: block past the end of the file";
          return false;
        }
        out.insert(out.end(), data + p, data + p + n);
        out.insert(out.end(), zeros, 0);
        p += n;
      }
      out.resize(image_size);
      return true;
    }
    case kNormal: {
      // A chain of blocks, each starting with the next block's size and
      // hash, holding chunks of LZX data prefixed by their 16-bit size.
      uint32_t window = r.U32(ffi + 8);
      uint32_t block = r.U32(ffi + 12);
      std::vector<uint8_t> lzx;
      size_t p = 0;
      while (block) {
        if (p + block > data_size || block < 24) {
          err = "normal compression: block past the end of the file";
          return false;
        }
        size_t next = p + block;
        uint32_t next_block = uint32_t(data[p]) << 24 | data[p + 1] << 16 | data[p + 2] << 8 | data[p + 3];
        p += 24;
        for (;;) {
          if (p + 2 > next) {
            err = "normal compression: chunk past the end of its block";
            return false;
          }
          size_t chunk = size_t(data[p]) << 8 | data[p + 1];
          p += 2;
          if (!chunk) break;
          if (p + chunk > next) {
            err = "normal compression: chunk past the end of its block";
            return false;
          }
          lzx.insert(lzx.end(), data + p, data + p + chunk);
          p += chunk;
        }
        p = next;
        block = next_block;
      }
      return LzxDecompress(lzx.data(), lzx.size(), window, image_size, out, err);
    }
    default:
      err = "unsupported XEX compression (delta patches are not read)";
      return false;
  }
}

// The PE headers inside the image: sections and .pdata.
bool ReadPe(XexModule& m, std::string& err) {
  const std::vector<uint8_t>& d = m.image;
  if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') {
    err = "the XEX image has no PE header";
    return false;
  }
  uint32_t nt = Le32(d, 0x3C);
  if (Le32(d, nt) != 0x00004550) {
    err = "the XEX image has no PE header";
    return false;
  }
  uint32_t nsec = d[nt + 6] | d[nt + 7] << 8;
  uint32_t opt = nt + 24, opt_size = d[nt + 20] | d[nt + 21] << 8;
  uint32_t sec = opt + opt_size;
  for (uint32_t k = 0; k < nsec; k++) {
    uint32_t s = sec + 40 * k;
    if (s + 40 > d.size()) break;
    XexSection x;
    x.name.assign(reinterpret_cast<const char*>(&d[s]), strnlen(reinterpret_cast<const char*>(&d[s]), 8));
    x.size = Le32(d, s + 8);
    x.va = m.base + Le32(d, s + 12);
    // IMAGE_SCN_CNT_CODE or IMAGE_SCN_MEM_EXECUTE.
    x.code = Le32(d, s + 36) & 0x20000020;
    m.sections.push_back(x);
  }
  // Data directory 3, the exception table: .pdata entries of the
  // function's start and, in bits 8-29 of the second word, its length in
  // instructions.
  uint32_t pdata = Le32(d, opt + 96 + 8 * 3), pdata_size = Le32(d, opt + 100 + 8 * 3);
  for (uint32_t off = pdata; off + 8 <= pdata + pdata_size && off + 8 <= d.size(); off += 8) {
    uint32_t begin = Be32(d, off), info = Be32(d, off + 4);
    uint32_t len = (info >> 8) & 0x3FFFFF;
    if (begin && len) m.functions.push_back({begin, len * 4});
  }
  return true;
}

}  // namespace

uint64_t TextHash(const XexModule& m) {
  std::vector<const XexSection*> code;
  for (const XexSection& s : m.sections)
    if (s.code && s.size) code.push_back(&s);
  std::sort(code.begin(), code.end(), [](auto a, auto b) { return a->va < b->va; });
  std::vector<uint8_t> text;
  for (const XexSection* s : code) {
    size_t off = s->va - m.base, end = std::min(m.image.size(), off + s->size);
    if (off < end) text.insert(text.end(), m.image.begin() + off, m.image.begin() + end);
  }
  return recomp::TextHash(text.data(), text.size());
}

bool IsXex(const std::string& path) {
  char head[4] = {};
  std::ifstream f(path, std::ios::binary);
  return f.read(head, sizeof head) && !std::memcmp(head, "XEX2", 4);
}

bool LoadXex(const std::string& path, XexModule& m, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = path + ": cannot open";
    return false;
  }
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!ParseXex(file, m, err)) {
    err = path + ": " + err;
    return false;
  }
  return true;
}

bool ParseXex(const std::vector<uint8_t>& file, XexModule& m, std::string& err) {
  Reader r(file);
  if (file.size() < 24 || std::memcmp(file.data(), "XEX2", 4)) {
    err = "not a XEX2 file";
    return false;
  }
  m = XexModule{};
  m.module_flags = r.U32(4);
  uint32_t header_size = r.U32(8), security = r.U32(16), count = r.U32(20);
  uint32_t ffi = 0, imports = 0;
  for (uint32_t k = 0; k < count; k++) {
    uint32_t key = r.U32(24 + 8 * k), value = r.U32(28 + 8 * k);
    switch (key) {
      case kFileFormat: ffi = value; break;
      case kEntryPoint: m.entry = value; break;
      case kImageBase: m.base = value; break;
      case kImportLibraries: imports = value; break;
      case kOriginalName:
        if (r.Has(value, 4) && r.Has(value + 4, r.U32(value) - 4))
          m.original_name.assign(reinterpret_cast<const char*>(r.At(value + 4)),
                                 strnlen(reinterpret_cast<const char*>(r.At(value + 4)), r.U32(value) - 4));
        break;
      case kExecutionInfo: m.title_id = r.U32(value + 12); break;
      case kTlsInfo:
        m.tls = {r.U32(value), r.U32(value + 4), r.U32(value + 8), r.U32(value + 12)};
        break;
      case kDefaultStackSize: m.stack_size = value; break;
      default: break;
    }
  }
  if (!ffi || !security || header_size > file.size() || !r.Has(security, 0x184)) {
    err = "malformed XEX headers";
    return false;
  }
  uint32_t image_size = r.U32(security + 4);
  m.headers.assign(file.begin(), file.begin() + header_size);
  if (!m.base) m.base = r.U32(security + kSecurityLoadAddress);
  if (!Image(r, file, header_size, ffi, image_size, m.image, err) || !ReadPe(m, err)) return false;

  // Import libraries: a string table of module names, then per library
  // its records. A record's word in the image says what it is: type in the
  // top byte (0 variable, 1 thunk) and the ordinal in the low 16 bits.
  if (imports) {
    uint32_t strings_size = r.U32(imports + 4), libs = r.U32(imports + 8);
    std::vector<std::string> names;
    for (uint32_t off = imports + 12; off < imports + 12 + strings_size && r.Has(off, 1);) {
      std::string s(reinterpret_cast<const char*>(r.At(off)));
      if (!s.empty()) names.push_back(s);
      off += uint32_t(s.size()) + 1;
      while (off < imports + 12 + strings_size && r.Has(off, 1) && !*r.At(off)) off++;
    }
    uint32_t lib = imports + 12 + strings_size;
    for (uint32_t k = 0; k < libs && r.Has(lib, 0x28); k++) {
      uint32_t size = r.U32(lib);
      uint16_t name_index = r.U16(lib + 0x24), n = r.U16(lib + 0x26);
      if (name_index >= names.size()) {
        err = "malformed XEX import library";
        return false;
      }
      for (uint32_t j = 0; j < n; j++) {
        uint32_t va = r.U32(lib + 0x28 + 4 * j);
        uint32_t word = Be32(m.image, uint64_t(va) - m.base);
        m.imports.push_back({names[name_index], word & 0xFFFF, va, (word >> 24) == 1});
      }
      if (!size) break;
      lib += size;
    }
  }

  // System modules export by ordinal from a table in the image: three magic
  // words, then the high half of the image base, the count, the first
  // ordinal and an offset per ordinal.
  uint32_t table = r.U32(security + kSecurityExportTable);
  if (table) {
    uint64_t t = uint64_t(table) - m.base;
    if (Be32(m.image, t) == 0x48000000 && Be32(m.image, t + 4) == 0x00485645) {
      uint32_t hi = Be32(m.image, t + 0x20), n = Be32(m.image, t + 0x24);
      m.export_base = Be32(m.image, t + 0x28);
      for (uint32_t k = 0; k < n; k++) {
        uint32_t off = Be32(m.image, t + 0x2C + 4 * k);
        m.exports.push_back(off ? (hi << 16) + off : 0);
      }
    }
  }
  return true;
}

}  // namespace recomp::xbox360
