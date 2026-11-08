// Frame descriptions (recomp::ppc::LiftedFrame): what a function's prologue
// does to the stack pointer and where it leaves the values the registers
// had on entry, read by following the prologue's instructions over symbolic
// values (src/ppc/ARCHITECTURE.md, "Unwinding").

#include <set>
#include <vector>

#include "program.h"

namespace lifter::ppc {

using ppc::Insn;
using ppc::Op;
using recomp::ppc::kSaveCr;
using recomp::ppc::kSaveFpr;
using recomp::ppc::kSaveGpr;
using recomp::ppc::kSaveLr;
using recomp::ppc::kSaveVr;
using recomp::ppc::LiftedSave;

namespace {

// A value in the prologue: unknown, the value register `v` had on entry (a
// LiftedSave id), r1 on entry + v, or the constant v.
struct Val {
  enum Kind : uint8_t { kUnknown, kEntry, kSp, kConst } k = kUnknown;
  int64_t v = 0;
  bool operator==(const Val&) const = default;
};
constexpr Val kZero{Val::kConst, 0};

bool IsBlr(const Insn& i) { return i.op == Op::kBclr && !i.lk && (i.bo & 0x14) == 0x14; }

Val Sum(const Val& a, const Val& b) {
  if (a.k == Val::kConst && (b.k == Val::kConst || b.k == Val::kSp)) return {b.k, a.v + b.v};
  if (a.k == Val::kSp && b.k == Val::kConst) return {Val::kSp, a.v + b.v};
  return {};
}

class Reader {
 public:
  Reader(const Program& p, Cpu cpu, Frame& f) : p_(p), cpu_(cpu), f_(f) {
    for (int r = 0; r < 32; r++) g_[r] = {Val::kEntry, kSaveGpr + r};
    g_[1] = {Val::kSp, 0};
  }

  // One instruction of the prologue (or of a register save routine it
  // calls, `in_routine`). False if it is one the reader does not follow.
  bool Step(const Insn& i, bool in_routine) {
    switch (i.op) {
      case Op::kMfspr:
        if (i.spr == kLr) {
          g_[i.rd] = lr_;
          return true;
        }
        break;
      case Op::kMfcr:
        g_[i.rd] = cr_;
        return true;
      case Op::kOr:  // mr ra, rs
        if (i.rd == i.rb && !i.rc) {
          g_[i.ra] = g_[i.rd];
          return true;
        }
        break;
      case Op::kAddi:
        g_[i.rd] = Sum(i.ra ? g_[i.ra] : kZero, {Val::kConst, i.imm});
        return true;
      case Op::kOri:
        if (i.rc) break;
        g_[i.ra] = g_[i.rd].k == Val::kConst ? Val{Val::kConst, g_[i.rd].v | i.imm}
                   : i.imm == 0             ? g_[i.rd]
                                            : Val{};
        return true;
      case Op::kStore: {
        Val at = Ea(i);
        if (g_[i.rd].k == Val::kEntry && (i.size == 4 || i.size == 8) && !i.byterev)
          Save(int(g_[i.rd].v), i.size, at);
        if (i.update) g_[i.ra] = at;
        return true;
      }
      case Op::kStmw: {
        Val at = Ea(i);
        for (int r = i.rd; r < 32; r++)
          if (g_[r].k == Val::kEntry) Save(int(g_[r].v), 4, Sum(at, {Val::kConst, 4 * (r - i.rd)}));
        return true;
      }
      case Op::kFpStore: {
        Val at = Ea(i);
        if (i.size == 8 && i.opc == 0 && !fpr_changed_[i.rd]) Save(kSaveFpr + i.rd, 8, at);
        if (i.update) g_[i.ra] = at;
        return true;
      }
      case Op::kVstore:
        if (i.opc == ppc::kVecQuad && !vr_changed_[i.rd])
          Save(kSaveVr + i.rd, 16, Sum(i.ra ? g_[i.ra] : kZero, g_[i.rb]));
        return true;
      case Op::kB:
        if (i.lk && !in_routine) {
          lr_ = {};  // the call's return address
          return Routine(uint64_t(i.imm));
        }
        return false;
      case Op::kSubf:  // rd = rb - ra
        if (i.oe || i.rc) break;
        g_[i.rd] = g_[i.ra].k == Val::kConst ? Sum(g_[i.rb], {Val::kConst, -g_[i.ra].v}) : Val{};
        return true;
      case Op::kBc: case Op::kBclr: case Op::kBcctr: case Op::kSc: case Op::kTw: case Op::kTwi:
      case Op::kTd: case Op::kTdi: case Op::kInvalid:
        return false;
      default:
        break;
    }
    // Anything else: what it writes is unknown from here on.
    for (int r = 0; r < 32; r++)
      if (WritesReg(i, r)) g_[r] = {};
    switch (i.op) {
      case Op::kFpLoad: case Op::kFpArith: case Op::kFpMove: case Op::kFsel: case Op::kFrsp:
      case Op::kFctiw: case Op::kFcfid: case Op::kMffs:
        fpr_changed_[i.rd] = true;
        break;
      case Op::kVmx: case Op::kVload:
        vr_changed_[i.rd] = true;
        break;
      case Op::kMtspr:
        if (i.spr == kLr) lr_ = {};
        break;
      case Op::kMtcrf: case Op::kCmp: case Op::kCmpl: case Op::kCmpi: case Op::kCmpli:
      case Op::kCrLogic: case Op::kMcrf: case Op::kMcrxr: case Op::kFcmp: case Op::kMcrfs:
      case Op::kVcmp:
        cr_ = {};
        break;
      default:
        if (i.rc) cr_ = {};
        break;
    }
    return true;
  }

  // A routine the prologue calls: a register save routine, straight-line
  // code up to its blr, whose stores are the prologue's; or one with
  // branches (a stack probe), which saves nothing: what any of its
  // instructions writes is unknown after it.
  bool Routine(uint64_t va) {
    std::vector<Insn> line;
    for (uint64_t at = va;; at += 4) {
      Insn r = ppc::Decode(at, p_.Word(at), cpu_);
      if (IsBlr(r)) {
        for (const Insn& x : line)
          if (!Step(x, true)) return false;
        return true;
      }
      if (r.op == Op::kB || r.op == Op::kBc || r.op == Op::kBclr || r.op == Op::kBcctr) break;
      if (line.size() == 256) return false;
      line.push_back(r);
    }
    std::set<uint64_t> seen;
    std::vector<uint64_t> work{va};
    while (!work.empty()) {
      uint64_t at = work.back();
      work.pop_back();
      if (!seen.insert(at).second) continue;
      if (seen.size() > 256) return false;
      Insn r = ppc::Decode(at, p_.Word(at), cpu_);
      if (r.op == Op::kInvalid || r.op == Op::kBcctr || (r.op == Op::kB && r.lk)) return false;
      if (r.op == Op::kStore || r.op == Op::kFpStore || r.op == Op::kVstore || r.op == Op::kStmw) {
        if (r.update) g_[r.ra] = {};
      } else if (r.op != Op::kB && r.op != Op::kBc && r.op != Op::kBclr && !Step(r, true)) {
        return false;
      }
      if (IsBlr(r)) continue;
      if (r.op == Op::kB) {
        work.push_back(uint64_t(r.imm));
        continue;
      }
      if (r.op == Op::kBc) work.push_back(uint64_t(r.imm));
      work.push_back(at + 4);
    }
    return true;
  }

  // The frame once the prologue has run: the stack pointer, a frame
  // pointer, and for each value a caller expects back (the nonvolatile
  // registers, LR, CR) where it is if the prologue moved it.
  bool Finish() {
    if (g_[1].k != Val::kSp) return false;
    f_.sp = int32_t(g_[1].v);
    for (int r = 31; r >= 14; r--)
      if (g_[r].k == Val::kSp) {
        f_.fp = uint8_t(r);
        f_.fp_offset = int32_t(g_[r].v);
        break;
      }
    auto saved = [&](int id) {
      for (const LiftedSave& s : f_.saves)
        if (s.reg == id) return true;
      return false;
    };
    // A value still in a register (LR in r31, say).
    auto held = [&](int id) {
      for (int r = 31; r >= 0; r--)
        if (g_[r] == Val{Val::kEntry, id}) {
          f_.saves.push_back({uint8_t(id), 0, r});
          return true;
        }
      return false;
    };
    // LR: saved, or moved to a register the calls in the body keep (r31),
    // or, in a leaf, still in LR.
    if (!saved(kSaveLr) && !held(kSaveLr) && !(lr_ == Val{Val::kEntry, kSaveLr})) return false;
    // CR, and a nonvolatile register the prologue changes without saving
    // (a funclet's frame pointer), are left as they are, as the function's
    // own epilogue leaves them.
    if (!saved(kSaveCr)) held(kSaveCr);
    for (int r = 14; r < 32; r++)
      if (!(g_[r] == Val{Val::kEntry, kSaveGpr + r}) && !saved(kSaveGpr + r)) held(kSaveGpr + r);
    return true;
  }

 private:
  void Save(int id, int size, const Val& at) {
    if (at.k != Val::kSp) return;  // a store elsewhere is not a save
    for (const LiftedSave& s : f_.saves)
      if (s.reg == id) return;  // the first save holds it
    f_.saves.push_back({uint8_t(id), uint8_t(size), int32_t(at.v)});
  }
  // The address ra|0 + rb (indexed) or ra|0 + imm.
  Val Ea(const Insn& i) const {
    return Sum(i.ra ? g_[i.ra] : kZero, i.indexed ? g_[i.rb] : Val{Val::kConst, i.imm});
  }

  const Program& p_;
  Cpu cpu_;
  Frame& f_;
  Val g_[32], lr_{Val::kEntry, kSaveLr}, cr_{Val::kEntry, kSaveCr};
  bool fpr_changed_[32] = {}, vr_changed_[128] = {};
};

}  // namespace

Frame Program::ReadPrologue(uint64_t va, uint64_t len) const {
  Frame f;
  f.va = va;
  Reader r(*this, img_.cpu, f);
  bool ok = true;
  for (uint64_t at = va; ok && at < va + len; at += 4) ok = r.Step(ppc::Decode(at, Word(at), img_.cpu), false);
  if (!ok || !r.Finish()) {
    f.unreadable = true;
    f.saves.clear();
  }
  return f;
}

std::vector<Frame> Program::Frames() const {
  std::vector<Frame> out;
  for (const auto& [va, len] : img_.prologues) {
    auto fn = funcs_.find(va);
    if (fn == funcs_.end() || fn->second.import) continue;
    Frame f = ReadPrologue(va, len);
    f.end = fn->second.end;
    out.push_back(std::move(f));
  }
  return out;
}

}  // namespace lifter::ppc
