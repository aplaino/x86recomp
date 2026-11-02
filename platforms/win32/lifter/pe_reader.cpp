// recomp_lift's reader for PE images (win32/pe_image.h): the entry point,
// noreturn imports and, for x64 images, function entries and ranges from
// .pdata with the frames of those that have a language handler. PE images
// say too little about their code without Ghidra's symbol files.

#include <algorithm>
#include <fstream>

#include "program.h"
#include "win32/pe_image.h"
#include "win32/unwind.h"

namespace lifter {
namespace {

bool Match(const std::string& path, bool* x64) {
  std::ifstream f(path, std::ios::binary);
  char mz[2] = {};
  if (!(f.read(mz, 2) && mz[0] == 'M' && mz[1] == 'Z')) return false;
  *x64 = recomp::PeImage::IsX64File(path);
  return true;
}

// The .pdata entry whose range holds va, or null.
const recomp::PeFunction* PdataAt(const recomp::PeImage& img, uint64_t va) {
  const auto& f = img.functions;
  auto it = std::upper_bound(f.begin(), f.end(), va,
                             [](uint64_t v, const recomp::PeFunction& e) { return v < e.begin; });
  if (it == f.begin()) return nullptr;
  --it;
  return va < it->end ? &*it : nullptr;
}

// The start of the function a .pdata range belongs to: chained fragments
// (code the compiler moved out of line) lead to their parent, in short
// form (unwind RVA with bit 0 set) or through the UNWIND_INFO.
uint64_t PrimaryOf(const Program& p, const recomp::PeImage& img, const recomp::PeFunction* f) {
  for (int depth = 0; f && f->chained() && depth < 32; depth++) {
    uint64_t parent = p.mem.read<uint32_t>(f->entry + 8) & 1
                          ? f->unwind
                          : recomp::unwind::Read(p.mem, img.image_base, f->unwind).chained;
    f = PdataAt(img, img.image_base + p.mem.read<uint32_t>(parent));
  }
  return f ? f->begin : 0;
}

bool Load(Program& p, Module& m, const std::string& path, uint64_t base, std::string* err) {
  recomp::PeImage img;
  if (!img.Load(path, p.mem, err, base)) return false;
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
    m.ranges.push_back({fn.begin, fn.end, PrimaryOf(p, img, &fn)});
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

const bool kRegistered = (RegisterImageReader({Match, Load, true}), true);

}  // namespace
}  // namespace lifter
