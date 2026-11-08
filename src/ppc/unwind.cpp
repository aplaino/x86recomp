#include "ppc/unwind.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "ppc/fatal.h"

namespace recomp::ppc {
namespace {

struct Frames {
  std::vector<LiftedFrame> frames;  // sorted by va
  std::vector<LiftedSave> saves;
};
// Filled before main() by each lifted module's lifted_table.cpp.
Frames& All() {
  static Frames f;
  return f;
}

}  // namespace

void RegisterLiftedFrames(const LiftedFrame* frames, uint32_t count, const LiftedSave* saves) {
  Frames& a = All();
  uint32_t base = uint32_t(a.saves.size()), n = 0;
  for (uint32_t i = 0; i < count; i++) n = std::max(n, frames[i].first + frames[i].saves);
  a.saves.insert(a.saves.end(), saves, saves + n);
  for (uint32_t i = 0; i < count; i++) {
    LiftedFrame f = frames[i];
    f.first += base;
    a.frames.push_back(f);
  }
  std::sort(a.frames.begin(), a.frames.end(),
            [](const LiftedFrame& x, const LiftedFrame& y) { return x.va < y.va; });
}

const LiftedFrame* LookupFrame(uint32_t pc) {
  const std::vector<LiftedFrame>& f = All().frames;
  auto it = std::upper_bound(f.begin(), f.end(), pc,
                             [](uint32_t v, const LiftedFrame& x) { return v < x.va; });
  if (it == f.begin()) return nullptr;
  --it;
  return pc < it->end ? &*it : nullptr;
}

uint32_t VirtualUnwind(const LiftedFrame& f, Ctx& s) {
  if (f.unreadable)
    Fatal(&s, "unwinding through %x: the lifter could not read its prologue", f.va);
  // r1 on entry: from the frame pointer if the prologue set one (the stack
  // pointer may have moved since, alloca), else from r1.
  uint32_t entry = f.fp ? uint32_t(s.r[f.fp]) - uint32_t(f.fp_offset)
                        : uint32_t(s.r[1]) - uint32_t(f.sp);
  const Ctx in = s;
  const LiftedSave* sv = All().saves.data() + f.first;
  for (uint32_t k = 0; k < f.saves; k++) {
    const LiftedSave& v = sv[k];
    uint32_t at = entry + uint32_t(v.where);
    uint64_t x = 0;
    if (v.size == 0) x = in.r[v.where];
    else if (v.size == 4) x = lift::Ld<uint32_t>(s.mem, at);
    else if (v.size == 8) x = lift::Ld<uint64_t>(s.mem, at);
    if (v.reg >= kSaveVr) {
      std::memcpy(s.v[v.reg - kSaveVr].b, s.mem + at, 16);
    } else if (v.reg >= kSaveFpr) {
      s.f[v.reg - kSaveFpr] = x;
    } else if (v.reg == kSaveLr) {
      s.lr = uint32_t(x);
    } else if (v.reg == kSaveCr) {
      s.cr = uint32_t(x);
    } else {
      s.r[v.reg] = x;
    }
  }
  s.r[1] = entry;
  return entry;
}

void Jump(Ctx& c, uint64_t target, uint64_t va) {
  uint32_t t = uint32_t(target) & ~3u;
  const LiftedFrame* f = LookupFrame(t);
  if (!f)
    Fatal(&c, "jump at %llx to %x: its function has no frame description",
          static_cast<unsigned long long>(va), t);
  Ctx probe = c;
  uint32_t frame = VirtualUnwind(*f, probe);
  c.resv_valid = false;
  throw GuestResume{t, frame};
}

}  // namespace recomp::ppc
