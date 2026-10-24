#include "program.h"

#include "runtime/unwind.h"

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

uint64_t Hex(const std::string& s) {
  return std::stoull(s, nullptr, 16);
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

std::vector<ImageReader>& Readers() {
  static std::vector<ImageReader> r;
  return r;
}

bool IsPeFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  char mz[2] = {};
  return f.read(mz, 2) && mz[0] == 'M' && mz[1] == 'Z';
}

// Maps a PE image into m and adds its .pdata functions, entry point and
// noreturn imports.
bool LoadPe(Program& p, Module& m, const std::string& path, uint64_t base,
            std::string* err) {
  if (!m.pe.Load(path, p.mem, err, base)) return false;
  const recomp::PeImage& img = m.pe;
  m.base = img.image_base;
  m.preferred_base = img.preferred_base;
  for (const auto& s : img.sections) {
    if (s.name == ".text") {
      m.text_lo = s.va;
      m.text_hi = s.va + s.vsize;
    }
    if (s.name == ".rdata" || s.name == ".data") m.data.push_back({s.va, s.va + s.vsize});
  }
  auto in_text = [&](uint64_t va) { return va >= m.text_lo && va < m.text_hi; };

  // x64: every .pdata entry that is not a chained fragment starts a
  // function (ARCHITECTURE.md, "64-bit guests"); those with a language
  // handler can be resumed into.
  for (const auto& fn : img.functions) {
    if (fn.chained() || !in_text(fn.begin)) continue;
    p.entries.insert(fn.begin);
    recomp::unwind::Info u = recomp::unwind::Read(p.mem, img.image_base, fn.unwind);
    if (u.flags & (recomp::unwind::kEHandler | recomp::unwind::kUHandler))
      p.eh_frames[fn.begin] = {fn.begin + u.prolog, u.frame_reg, u.frame_off};
  }

  if (img.entry) {
    p.entries.insert(img.entry);
    p.names.emplace(img.entry, img.dll ? "_DllMainCRTStartup" : "WinMainCRTStartup");
  }

  // Imports that never return; a call through their IAT slot ends a block.
  for (const auto& imp : img.imports)
    if (imp.name == "ExitProcess" || imp.name == "ExitThread")
      p.noreturn_iat.insert(imp.iat_va);
  return true;
}

}  // namespace

void RegisterImageReader(const ImageReader& r) { Readers().push_back(r); }

bool Program::Load(const std::string& path, const std::string& symdir,
                   uint64_t base, std::string* err) {
  const ImageReader* reader = nullptr;
  bool image_x64 = false;
  if (!std::filesystem::is_regular_file(path)) {
    *err = "cannot open " + path;
    return false;
  }
  if (IsPeFile(path)) {
    image_x64 = recomp::PeImage::IsX64File(path);
    if (symdir.empty()) {
      *err = "--symbols missing";
      return false;
    }
  } else {
    for (const ImageReader& r : Readers())
      if (r.Match(path, &image_x64)) reader = &r;
    if (!reader) {
      *err = "not a PE image, and no enabled platform reads its format";
      return false;
    }
  }
  if (!mem.base()) {
    x64 = image_x64;
    if (!mem.Reserve(x64 ? 36 : 32)) {
      *err = "cannot reserve guest arena";
      return false;
    }
  } else if (image_x64 != x64) {
    *err = "cannot lift 32-bit and x64 modules together";
    return false;
  }
  Module& m = modules.emplace_back();
  m.name = std::filesystem::path(path).stem().string();
  if (!(reader ? reader->Load(*this, m, path, base, err) : LoadPe(*this, m, path, base, err))) {
    if (modules.size() > 1) *err += " (give the module another --base)";
    return false;
  }
  if (!m.text_hi) {
    *err = "no .text section";
    return false;
  }
  if (symdir.empty()) return true;
  // Symbol files hold VAs at the preferred base; a moved module's are
  // shifted by the same delta as its relocations.
  const uint64_t delta = m.base - m.preferred_base;
  auto in_text = [&](uint64_t va) { return va >= m.text_lo && va < m.text_hi; };

  // Ghidra's function list: entry, min, max, chunks, thunk, noreturn,
  // callconv, name.
  if (!ReadTsv(symdir + "/functions.tsv", [&](const auto& c) {
        uint64_t entry = Hex(c[0]) + delta;
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
        uint64_t va = Hex(c[0]) + delta;
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
    const Module& mod = modules.back();
    uint64_t va = Hex(va_s) + (mod.base - mod.preferred_base);
    if (kind == "entry") {
      entries.insert(va);
      if (!name.empty()) names[va] = name;
    } else if (kind == "name") {
      names[va] = name;
    } else if (kind == "resume") {
      resume_sites.insert(va);
    } else if (kind == "setjmp") {
      entries.insert(va);
      setjmps.insert(va);
      if (!name.empty()) names[va] = name;
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
