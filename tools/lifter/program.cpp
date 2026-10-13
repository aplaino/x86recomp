#include "program.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace lifter {

namespace {

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, sep)) out.push_back(cur);
  if (!s.empty() && s.back() == sep) out.push_back("");
  return out;
}

uint32_t Hex(const std::string& s) {
  return static_cast<uint32_t>(std::stoul(s, nullptr, 16));
}

// Reads a TSV with a header row; calls fn(columns) per data row.
template <typename Fn>
bool ReadTsv(const std::string& path, Fn fn, std::string* err) {
  std::ifstream f(path);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  std::string line;
  std::getline(f, line);  // header
  while (std::getline(f, line))
    if (!line.empty()) fn(Split(line, '\t'));
  return true;
}

}  // namespace

bool Program::Load(const std::string& path, const std::string& symdir,
                   uint32_t base, std::string* err) {
  if (!mem.base() && !mem.Reserve()) {
    *err = "cannot reserve guest arena";
    return false;
  }
  Module& m = modules.emplace_back();
  m.name = std::filesystem::path(path).stem().string();
  if (!m.img.Load(path, mem, err, base)) {
    if (modules.size() > 1) *err += " (give the module another --base)";
    return false;
  }
  const recomp::PeImage& img = m.img;
  for (const auto& s : img.sections)
    if (s.name == ".text") {
      m.text_lo = s.va;
      m.text_hi = s.va + s.vsize;
    }
  if (!m.text_hi) {
    *err = "no .text section";
    return false;
  }
  // Symbol files hold VAs at the preferred base; a moved module's are
  // shifted by the same delta as its relocations.
  const uint32_t delta = img.image_base - img.preferred_base;
  auto in_text = [&](uint32_t va) { return va >= m.text_lo && va < m.text_hi; };

  // Ghidra's function list: entry, min, max, chunks, thunk, noreturn,
  // callconv, name.
  if (!ReadTsv(symdir + "/functions.tsv", [&](const auto& c) {
        uint32_t entry = Hex(c[0]) + delta;
        if (!in_text(entry)) return;
        entries.insert(entry);
        if (c.size() > 5 && c[5] == "1") noreturn.insert(entry);
        if (c.size() > 7 && c[7].rfind("FUN_", 0) != 0) names[entry] = c[7];
      }, err))
    return false;

  // Exports: every exported function is a call target (a game may call
  // its own exports by address), and its demangled name beats Ghidra's.
  if (!ReadTsv(symdir + "/exports.tsv", [&](const auto& c) {
        if (c.size() < 4 || c[1] != "func") return;
        uint32_t va = Hex(c[0]) + delta;
        if (!in_text(va)) return;
        entries.insert(va);
        if (!c[3].empty()) names[va] = c[3];
      }, err))
    return false;

  if (!ReadTsv(symdir + "/jumptables.tsv", [&](const auto& c) {
        if (c.size() < 2 || c[1].empty()) return;
        auto& t = jumptables[Hex(c[0]) + delta];
        for (const auto& s : Split(c[1], ',')) t.push_back(Hex(s) + delta);
      }, err))
    return false;

  if (img.entry) {
    entries.insert(img.entry);
    names.emplace(img.entry, img.dll ? "_DllMainCRTStartup" : "WinMainCRTStartup");
  }

  // Imports that never return; a call through their IAT slot ends a block.
  for (const auto& imp : img.imports)
    if (imp.name == "ExitProcess" || imp.name == "ExitThread")
      noreturn_iat.insert(imp.iat_va);
  return true;
}

bool Program::LoadManifest(const std::string& path, std::string* err) {
  std::ifstream f(path);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  std::string line;
  int no = 0;
  while (std::getline(f, line)) {
    no++;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string kind, va_s, name;
    if (!(in >> kind)) continue;
    if (!(in >> va_s)) {
      *err = path + ":" + std::to_string(no) + ": missing address";
      return false;
    }
    std::getline(in >> std::ws, name);
    const recomp::PeImage& img = modules.back().img;
    uint32_t va = Hex(va_s) + (img.image_base - img.preferred_base);
    if (kind == "entry") {
      entries.insert(va);
      if (!name.empty()) names[va] = name;
    } else if (kind == "name") {
      names[va] = name;
    } else if (kind == "resume") {
      resume_sites.insert(va);
    } else if (kind == "probe") {
      entries.insert(va);
      probes[va] = name.empty() ? "probe" : name;
    } else if (kind == "replace") {
      entries.insert(va);
      replaced.insert(va);
      if (!name.empty()) names[va] = name;
    } else {
      *err = path + ":" + std::to_string(no) + ": unknown directive " + kind;
      return false;
    }
  }
  return true;
}

}  // namespace lifter
