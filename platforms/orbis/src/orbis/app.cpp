#include "orbis/app.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>

#include "orbis/elf_image.h"

namespace recomp::orbis {

bool AppModules(const std::string& app0, std::vector<AppModule>* out, std::string* err) {
  out->clear();
  ElfImage eboot;
  const std::string eboot_path = app0 + "/eboot.bin";
  if (!eboot.Read(eboot_path, err)) {
    *err = eboot_path + ": " + *err;
    return false;
  }

  // The shipped PRXs by module name (files named otherwise are found too).
  std::map<std::string, std::string> prxs;
  std::map<std::string, ElfImage> images;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(app0 + "/sce_module", ec)) {
    const std::string ext = e.path().extension().string();
    if (!e.is_regular_file() || (ext != ".prx" && ext != ".sprx")) continue;
    ElfImage img;
    if (!img.Read(e.path().string(), err)) {
      *err = e.path().string() + ": " + *err;
      return false;
    }
    if (img.type != 0xFE18) continue;
    const std::string name = img.module_name.empty() ? e.path().stem().string() : img.module_name;
    if (prxs.count(name)) {
      *err = "two PRXs of module " + name + " in " + app0 + "/sce_module";
      return false;
    }
    prxs[name] = e.path().string();
    images[name] = std::move(img);
  }
  out->push_back({eboot_path, "eboot", 0, true});
  std::set<std::string> seen;
  std::vector<std::string> order;
  std::function<void(const ElfImage&)> visit = [&](const ElfImage& img) {
    for (const std::string& n : img.needed_modules) {
      if (!prxs.count(n) || !seen.insert(n).second) continue;
      visit(images[n]);
      order.push_back(n);
    }
  };
  visit(eboot);
  const size_t started = order.size();
  for (const auto& [n, path] : prxs)
    if (!seen.count(n)) order.push_back(n);
  gaddr base = kPrxBase;
  for (size_t i = 0; i < order.size(); i++) {
    const std::string& n = order[i];
    out->push_back({prxs[n], n, base, i < started});
    const ElfImage& img = images[n];
    const gaddr end = base + img.Size();
    base = (end + kPrxAlign - 1) / kPrxAlign * kPrxAlign;
  }
  return true;
}

// The format: a header ("\0PSF", version, key table offset, data table
// offset, entry count), then 16-byte entries (key offset u16, format u16:
// 0x0004 and 0x0204 UTF-8, 0x0404 a u32; length u32, maximum length u32,
// data offset u32).
bool ReadParamSfo(const std::string& path, ParamSfo* out, std::string* err) {
  *out = ParamSfo{};
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto rd = [&](size_t off, size_t n) {
    uint32_t v = 0;
    if (off + n <= d.size()) std::memcpy(&v, d.data() + off, n);
    return v;
  };
  if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) {
    *err = path + ": not a param.sfo";
    return false;
  }
  const uint32_t keys = rd(8, 4), data = rd(12, 4), count = rd(16, 4);
  for (uint32_t i = 0; i < count; i++) {
    const size_t e = 20 + size_t(i) * 16;
    const size_t key = keys + rd(e, 2), fmt = rd(e + 2, 2), len = rd(e + 4, 4),
                 at = size_t(data) + rd(e + 12, 4);
    if (e + 16 > d.size() || key >= d.size() || at + len > d.size()) {
      *err = path + ": an entry runs past the end";
      return false;
    }
    std::string name;
    for (size_t k = key; k < d.size() && d[k]; k++) name += char(d[k]);
    if (fmt == 0x0404) {
      out->ints[name] = rd(at, 4);
    } else {
      std::string v(reinterpret_cast<const char*>(d.data() + at), len);
      v.resize(std::strlen(v.c_str()));
      out->strings[name] = v;
    }
  }
  return true;
}

}  // namespace recomp::orbis
