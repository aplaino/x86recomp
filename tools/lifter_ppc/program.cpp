#include "program.h"

#include <algorithm>
#include <cstdio>
#include <deque>

#include "text_hash.h"

namespace lifter::ppc {

using ppc::Insn;
using ppc::Op;

bool WritesReg(const Insn& i, int r) {
  switch (i.op) {
    // RT is the destination.
    case Op::kAddi: case Op::kAddic: case Op::kSubfic: case Op::kMulli:
    case Op::kAdd: case Op::kAddc: case Op::kAdde: case Op::kAddme: case Op::kAddze:
    case Op::kSubf: case Op::kSubfc: case Op::kSubfe: case Op::kSubfme: case Op::kSubfze:
    case Op::kNeg: case Op::kMullw: case Op::kMulld: case Op::kMulhw: case Op::kMulhwu:
    case Op::kMulhd: case Op::kMulhdu: case Op::kDivw: case Op::kDivwu: case Op::kDivd:
    case Op::kDivdu: case Op::kMfcr: case Op::kMfspr: case Op::kMftb: case Op::kMfmsr:
    case Op::kLarx:
      return i.rd == r;
    // RA is the destination.
    case Op::kAndi: case Op::kOri: case Op::kXori: case Op::kAnd: case Op::kAndc: case Op::kOr:
    case Op::kOrc: case Op::kXor: case Op::kNand: case Op::kNor: case Op::kEqv: case Op::kExtsb:
    case Op::kExtsh: case Op::kExtsw: case Op::kCntlzw: case Op::kCntlzd: case Op::kRlwinm:
    case Op::kRlwimi: case Op::kRlwnm: case Op::kRldicl: case Op::kRldicr: case Op::kRldic:
    case Op::kRldimi: case Op::kRldcl: case Op::kRldcr: case Op::kSlw: case Op::kSrw:
    case Op::kSraw: case Op::kSrawi: case Op::kSld: case Op::kSrd: case Op::kSrad: case Op::kSradi:
      return i.ra == r;
    case Op::kLoad:
      return i.rd == r || (i.update && i.ra == r);
    case Op::kStore: case Op::kFpLoad: case Op::kFpStore:
      return i.update && i.ra == r;
    case Op::kLmw:
      return r >= i.rd;
    case Op::kB: case Op::kBc: case Op::kBclr: case Op::kBcctr:
      return i.lk;  // the callee may change anything
    case Op::kSc: case Op::kTw: case Op::kTwi: case Op::kTd: case Op::kTdi:
      return true;  // so may the platform
    default:
      return false;
  }
}

namespace {

std::vector<ImageReader>& Readers() {
  static std::vector<ImageReader> readers;
  return readers;
}

}  // namespace

void RegisterImageReader(const ImageReader& r) { Readers().push_back(r); }

bool Program::Load(const std::string& path, std::string& err) {
  for (const ImageReader& r : Readers())
    if (r.Match(path)) {
      if (!r.Load(img_, path, err)) return false;
      std::sort(img_.code.begin(), img_.code.end());
      return true;
    }
  err = path + ": no image reader for this format (is its platform module enabled?)";
  return false;
}

bool Program::InText(uint64_t va) const { return CodeEnd(va) != 0; }

uint64_t Program::CodeEnd(uint64_t va) const {
  for (const auto& [lo, hi] : img_.code)
    if (va >= lo && va < hi) return hi;
  return 0;
}

uint64_t Program::text_hash() const {
  std::vector<uint8_t> text;
  for (const auto& [lo, hi] : img_.code)
    text.insert(text.end(), img_.bytes.begin() + (lo - img_.base),
                img_.bytes.begin() + (hi - img_.base));
  return recomp::TextHash(text.data(), text.size());
}

uint32_t Program::Word(uint64_t va) const { return uint32_t(Read(va, 4, false)); }

uint64_t Program::Read(uint64_t va, int size, bool sign) const {
  uint64_t off = va - img_.base, v = 0;
  if (va < img_.base || off + size > img_.bytes.size()) return 0;
  for (int k = 0; k < size; k++) v = v << 8 | img_.bytes[off + k];
  if (sign && size < 8) {
    int s = 64 - size * 8;
    v = uint64_t(int64_t(v << s) >> s);
  }
  return v;
}

const std::string* Program::NameOf(uint64_t va) const {
  auto it = img_.names.find(va);
  return it == img_.names.end() ? nullptr : &it->second;
}

void Program::Warn(const std::string& w) {
  if (std::find(warnings_.begin(), warnings_.end(), w) == warnings_.end()) warnings_.push_back(w);
}

// The value of `reg` before the instruction at `va`, if the straight-line
// code above it (back to `lo`) computes a constant: LIS / LI, then ADDI or
// ORI on it.
bool Program::ConstOf(uint64_t va, uint64_t lo, int reg, uint64_t& out, int depth) const {
  if (depth > 4) return false;
  for (uint64_t p = va; p > lo && va - p < 16 * 4;) {
    p -= 4;
    Insn i = ppc::Decode(p, Word(p), img_.cpu);
    if (!WritesReg(i, reg)) continue;
    uint64_t v;
    if (i.op == Op::kAddi) {
      if (i.ra == 0) {
        out = uint64_t(i.imm);
        return true;
      }
      if (!ConstOf(p, lo, i.ra, v, depth + 1)) return false;
      out = v + uint64_t(i.imm);
      return true;
    }
    if (i.op == Op::kOri && ConstOf(p, lo, i.rd, v, depth + 1)) {
      out = v | uint64_t(i.imm);
      return true;
    }
    return false;
  }
  return false;
}

// Compiler jump tables, ending in `mtctr T`, `bcctr`:
//   absolute: T = `lwzx TABLE, IDX` with IDX = `slwi X, 2`;
//   relative: T = `add BASE, OFF`, OFF = E or `slwi E, n`, and E =
//   `lbzx`/`lhzx`/`lwzx TABLE, IDX` with IDX = X scaled by the entry size;
// TABLE and BASE constants, X bounded by a `cmplwi crN, X, n` with a
// `bgt crN` (or `bge` on n) above it, or, with no bound, an absolute table
// right after the BCCTR. Each entry's target is computed as the code
// computes it. The walk is over straight-line code above the BCCTR, which
// is how compilers lay these out.
bool Program::JumpTable(const Function& f, uint64_t bcctr_va, std::vector<uint64_t>& targets) {
  auto writer = [&](uint64_t from, auto pred, Insn& out) {
    for (uint64_t p = from; p > f.va && from - p < 24 * 4;) {
      p -= 4;
      Insn i = ppc::Decode(p, Word(p), img_.cpu);
      if (pred(i)) {
        out = i;
        return true;
      }
    }
    return false;
  };
  // The instruction that last set `reg` (`ori r, r, 0`, the no-op compilers
  // pad with, does not count).
  auto def = [&](uint64_t from, int reg, Insn& out) {
    auto pred = [&](const Insn& i) {
      bool nop = i.op == Op::kOri && i.ra == i.rd && i.imm == 0;
      return (WritesReg(i, reg) && !nop) || i.op == Op::kB || i.op == Op::kBc;
    };
    return writer(from, pred, out) && WritesReg(out, reg);
  };
  // `slwi D, S, n` (rlwinm S, n, 0, 31 - n): n, or -1.
  auto slwi = [](const Insn& i) {
    return i.op == Op::kRlwinm && !i.rc && i.mb == 0 && i.me == 31 - i.sh ? int(i.sh) : -1;
  };
  Insn mtctr;
  if (!writer(bcctr_va, [](const Insn& i) { return (i.op == Op::kMtspr && i.spr == ppc::kCtr) ||
                                                   i.op == Op::kB || i.op == Op::kBc; }, mtctr) ||
      mtctr.op != Op::kMtspr)
    return false;
  Insn t;
  if (!def(mtctr.va, mtctr.rd, t)) return false;
  uint64_t base = 0;
  int shift = 0;
  bool relative = false;
  Insn ld = t;
  if (t.op == Op::kAdd && !t.rc && !t.oe) {
    relative = true;
    int off = t.rb;
    if (!ConstOf(t.va, f.va, t.ra, base)) {
      if (!ConstOf(t.va, f.va, t.rb, base)) return false;
      off = t.ra;
    }
    Insn o;
    if (!def(t.va, off, o)) return false;
    if (slwi(o) > 0) {
      shift = slwi(o);
      if (!def(o.va, o.rd, ld)) return false;
    } else {
      ld = o;
    }
  }
  if (ld.op != Op::kLoad || !ld.indexed || ld.sign || ld.byterev || ld.update) return false;
  if (!relative && ld.size != 4) return false;
  // Either operand may be the table.
  uint64_t table;
  int idx;
  if (ld.ra && ConstOf(ld.va, f.va, ld.ra, table)) idx = ld.rb;
  else if (ld.ra && ConstOf(ld.va, f.va, ld.rb, table)) idx = ld.ra;
  else return false;
  // The index scaled by the entry size (bytes need no scaling).
  int x = idx;
  uint64_t from = ld.va;
  if (ld.size > 1) {
    Insn sc;
    if (!def(ld.va, idx, sc) || (1 << slwi(sc)) != ld.size) return false;
    x = sc.rd;
    from = sc.va;
  }

  // Code that keeps the index in memory loads it again for the compare and
  // for the table: unoptimised code from its stack slot, other code from
  // the field of an object it compared. `lwz X, d(B)` makes that word the
  // index, and a compare on a register loaded from it bounds it, unless the
  // word can change in between: a store (for a stack slot, one to that
  // slot), a call, a change of B.
  struct Slot {
    int base;
    int64_t disp;
  };
  auto slot_load = [](const Insn& i, int r) {
    return i.op == Op::kLoad && i.size == 4 && !i.indexed && !i.update && !i.sign && i.ra != 0 &&
           i.ra != r && i.rd == r;
  };
  auto slot_hazard = [](const Insn& i, const Slot& w) {
    bool call = (i.op == Op::kB || i.op == Op::kBc || i.op == Op::kBclr || i.op == Op::kBcctr) && i.lk;
    bool store = i.op == Op::kStore || i.op == Op::kFpStore || i.op == Op::kStmw;
    bool aliases = w.base == 1 ? i.ra == 1 && (i.indexed || i.imm == w.disp) : true;
    return (store && aliases) || WritesReg(i, w.base) || call;
  };
  auto from_slot = [&](uint64_t cmp, int r, const Slot& w) {
    for (uint64_t p = cmp; p > f.va && cmp - p < 8 * 4;) {
      p -= 4;
      Insn i = ppc::Decode(p, Word(p), img_.cpu);
      if (slot_load(i, r)) return i.ra == w.base && i.imm == w.disp;
      if (WritesReg(i, r) || slot_hazard(i, w)) return false;
    }
    return false;
  };
  int crf = -1;
  bool gt = false;
  bool in_slot = false;
  Slot slot{};
  uint64_t count = 0;
  // The bound, or none (count 0) when the code above has none.
  for (uint64_t p = from; p > f.va && from - p < 24 * 4;) {
    p -= 4;
    Insn i = ppc::Decode(p, Word(p), img_.cpu);
    // bgt crN (BO 12, BI 4N+1) or bge crN (BO 4, BI 4N+0) to the default,
    // or the same conditions on a return (bgtlr, bgelr: no default case).
    if ((i.op == Op::kBc || i.op == Op::kBclr) && !i.lk && crf < 0) {
      if (i.bo == 12 && i.bi % 4 == 1) gt = true;
      else if (i.bo == 4 && i.bi % 4 == 0) gt = false;
      else break;
      crf = i.bi / 4;
      continue;
    }
    if (i.op == Op::kCmpli && !i.l && i.crfd == crf &&
        (in_slot ? from_slot(p, i.ra, slot) : i.ra == x)) {
      count = uint64_t(i.imm) + (gt ? 1 : 0);
      break;
    }
    if (!in_slot && slot_load(i, x)) {
      in_slot = true;
      slot = {i.ra, i.imm};
      continue;
    }
    // A move of the index (`mr`, or the zero extension `clrldi X, Y, 32`
    // as `rlwinm X, Y, 0, 0, 31`) keeps the 32 bits the compare saw.
    if (!in_slot && !i.rc && i.ra == x &&
        ((i.op == Op::kRlwinm && i.sh == 0 && i.mb == 0 && i.me == 31) ||
         (i.op == Op::kOr && i.rd == i.rb))) {
      x = i.rd;
      continue;
    }
    if (in_slot ? slot_hazard(i, slot) : WritesReg(i, x)) break;
  }
  // No bound (the compiler knew the index's range, a bit field's): a table
  // of words right after the BCCTR, whose targets are in the function and
  // which ends where the code after it begins, its lowest target past it.
  if (!count && !relative && ld.size == 4 && uint32_t(table) == bcctr_va + 4) {
    uint64_t start = uint32_t(table), lowest = f.end;
    while (start + 4 * count < lowest) {
      uint64_t e = Read(start + 4 * count, 4, false);
      if (e % 4 || e < f.va || e >= f.end || (e >= start && e < start + 4 * (count + 1))) return false;
      if (e > start) lowest = std::min(lowest, e);
      count++;
    }
    if (start + 4 * count != lowest) return false;
  }
  if (!count || count > 4096) return false;

  std::set<uint64_t> seen;
  for (uint64_t k = 0; k < count; k++) {
    uint64_t e = Read(uint32_t(table + ld.size * k), ld.size, false);
    uint64_t tgt = relative ? uint32_t(base + (e << shift)) : e;
    if (!InText(tgt) || tgt % 4) return false;
    if (seen.insert(tgt).second) targets.push_back(tgt);
  }
  return true;
}

Function Program::Build(uint64_t start, uint64_t end, std::set<uint64_t>& found) {
  Function f;
  f.va = start;
  f.end = end;
  f.resumable = img_.resumable.count(start) != 0;
  auto imp = img_.imports.find(start);
  if (imp != img_.imports.end()) {
    f.import = &imp->second;
    return f;
  }
  std::deque<uint64_t> work{start};
  auto branch = [&](uint64_t t) {
    if (t >= start && t < end) {
      f.labels.insert(t);
      work.push_back(t);
    } else {
      found.insert(t);
    }
  };
  while (!work.empty()) {
    uint64_t va = work.front();
    work.pop_front();
    if (va < start || va >= end || f.insns.count(va)) continue;
    const Insn& i = f.insns[va] = ppc::Decode(va, Word(va), img_.cpu);
    switch (i.op) {
      case Op::kB:
        if (i.lk) {
          found.insert(uint64_t(i.imm));
          break;
        }
        branch(uint64_t(i.imm));
        continue;
      case Op::kBc:
        if (i.lk) found.insert(uint64_t(i.imm));
        else branch(uint64_t(i.imm));
        if (ppc::IsTerminator(i)) continue;
        break;
      case Op::kBcctr:
        if (!i.lk) {
          std::vector<uint64_t> targets;
          if (JumpTable(f, va, targets)) {
            for (uint64_t t : targets) branch(t);
            f.tables[va] = std::move(targets);
          }
        }
        if (ppc::IsTerminator(i)) continue;
        break;
      default:
        if (ppc::IsTerminator(i)) continue;
        break;
    }
    work.push_back(va + 4);
  }
  // Jumps: a BCLR whose straight-line code above it (back to a label) last
  // set r1 with a load from memory other than the stack, where a return
  // has r1 from the stack (lwz r1, 0(r1)) or arithmetic on a frame pointer.
  for (const auto& [va, i] : f.insns) {
    if (i.op != Op::kBclr || i.lk) continue;
    for (uint64_t p = va; p > start && va - p < 64 * 4;) {
      if (f.labels.count(p)) break;
      p -= 4;
      auto it = f.insns.find(p);
      if (it == f.insns.end()) break;
      const Insn& w = it->second;
      if ((w.op == Op::kB || w.op == Op::kBc || w.op == Op::kBclr || w.op == Op::kBcctr) && !w.lk)
        break;
      if (!WritesReg(w, 1) || (w.op == Op::kB && w.lk)) continue;
      if (w.op == Op::kLoad && w.rd == 1 && w.ra != 1 && !w.update) f.jumps.insert(va);
      break;
    }
  }
  return f;
}

bool Program::SavesContext(const Function& f) const {
  // In address order from the start, before anything writes r3: a store of
  // r1 and one of the register MFLR copied LR to, both based on r3.
  int lr_reg = -1;
  bool sp = false, ret = false;
  for (const auto& [va, i] : f.insns) {
    if (i.op == Op::kMfspr && i.spr == kLr) lr_reg = i.rd;
    if (i.op == Op::kStore && i.ra == 3 && !i.indexed && !i.update && (i.size == 4 || i.size == 8)) {
      if (i.rd == 1) sp = true;
      if (int(i.rd) == lr_reg) ret = true;
    }
    if (sp && ret) return true;
    if (WritesReg(i, 3) && !(i.op == Op::kB && i.lk)) return false;
    if (i.op == Op::kB && i.lk) return false;
  }
  return false;
}

bool Program::SetjmpReturns() {
  std::set<uint64_t> savers;
  for (const auto& [va, f] : funcs_)
    if (!f.import && SavesContext(f)) savers.insert(va);
  bool added = false;
  // The functions the image describes a frame for: the activations a jump
  // finds (not the starts overlapping them).
  for (const auto& [va, f] : funcs_)
    for (const auto& [iva, i] : f.insns)
      if (i.op == Op::kB && i.lk && savers.count(uint64_t(i.imm)) && img_.prologues.count(va)) {
        added |= img_.resumable.insert(va).second;
        added |= img_.resume_targets.insert(iva + 4).second;
      }
  return added;
}

std::set<uint64_t> Program::Constants(bool data) const {
  std::set<uint64_t> consts;
  for (uint64_t off = 0; data && off + 4 <= img_.bytes.size(); off += 4)
    if (!InText(img_.base + off)) consts.insert(Word(img_.base + off));
  for (const auto& [va, f] : funcs_)
    for (const auto& [iva, i] : f.insns) {
      uint64_t v;
      if ((i.op == Op::kAddi && i.ra && ConstOf(iva, f.va, i.ra, v)) ||
          (i.op == Op::kOri && ConstOf(iva, f.va, i.rd, v)))
        consts.insert(uint32_t(i.op == Op::kAddi ? v + uint64_t(i.imm) : v | uint64_t(i.imm)));
    }
  return consts;
}

// Constants (Constants) that land in code past the last instruction their
// function reaches (in the gap after its return): a function called only
// through a pointer that has no .pdata entry, such as a one-branch thunk
// between two functions' padding. A constant in a function's reached code,
// or on padding (0) or an undecodable word, is not one.
std::set<uint64_t> Program::PointerStarts(const std::set<uint64_t>& starts) const {
  std::set<uint64_t> out;
  for (uint64_t t : Constants(true)) {
    if (t % 4 || !InText(t) || starts.count(t) || !Word(t) ||
        ppc::Decode(t, Word(t), img_.cpu).op == Op::kInvalid)
      continue;
    auto it = funcs_.upper_bound(t);
    if (it != funcs_.begin()) {
      const Function& f = std::prev(it)->second;
      if (t < f.end && f.insns.lower_bound(t) != f.insns.end()) continue;
    }
    out.insert(t);
  }
  return out;
}

// Where an unwind may continue a resumable function: the addresses in it
// the image names (Image::resume_targets) and the constants the code
// builds (Constants) that land on an instruction it reaches, as the
// continuation after a catch block, whose address the catch block's code
// returns. Each becomes a start of its own (overlapping the function, whose
// code is unchanged): the code a GuestResume runs.
std::set<uint64_t> Program::ResumeStarts(const std::set<uint64_t>& starts) const {
  std::set<uint64_t> cands = img_.resume_targets, out;
  std::set<uint64_t> consts = Constants(false);
  cands.insert(consts.begin(), consts.end());
  for (uint64_t t : cands) {
    if (t % 4 || starts.count(t)) continue;
    auto r = img_.resumable.upper_bound(t);
    if (r == img_.resumable.begin()) continue;
    auto f = funcs_.find(*std::prev(r));
    if (f == funcs_.end() || t <= f->second.va || t >= f->second.end) continue;
    bool named = img_.resume_targets.count(t) != 0;
    if (named ? !Word(t) || ppc::Decode(t, Word(t), img_.cpu).op == Op::kInvalid
              : !f->second.insns.count(t))
      continue;
    out.insert(t);
  }
  return out;
}

void Program::Discover() {
  std::set<uint64_t> starts;
  for (const auto& [va, size] : img_.starts) {
    starts.insert(va);
    if (size) sized_.emplace(va, size);
  }
  for (const auto& [va, ref] : img_.imports) starts.insert(va);
  for (;;) {
    funcs_.clear();
    std::set<uint64_t> found;
    for (auto it = starts.begin(); it != starts.end(); ++it) {
      auto next = std::next(it);
      uint64_t code_end = CodeEnd(*it);
      uint64_t end = next == starts.end() ? code_end : std::min(code_end, *next);
      auto sz = sized_.find(*it);
      if (sz != sized_.end()) end = std::min(code_end, *it + sz->second);
      funcs_[*it] = Build(*it, end, found);
    }
    bool grew = false;
    for (uint64_t t : found) {
      if (starts.count(t)) continue;
      if (!InText(t) || t % 4) {
        char b[96];
        std::snprintf(b, sizeof b, "branch target %llx is outside the code",
                      static_cast<unsigned long long>(t));
        Warn(b);
        continue;
      }
      starts.insert(t);
      grew = true;
    }
    if (!grew) {
      std::set<uint64_t> more = PointerStarts(starts), resume = ResumeStarts(starts);
      more.insert(resume.begin(), resume.end());
      bool setjmp = SetjmpReturns();
      if (more.empty() && !setjmp) break;
      starts.insert(more.begin(), more.end());
    }
  }
  for (const auto& [va, f] : funcs_)
    for (const auto& [iva, i] : f.insns)
      if (i.op == Op::kBcctr && !i.lk && !f.tables.count(iva)) {
        char b[96];
        std::snprintf(b, sizeof b, "bcctr at %llx is not a recognised jump table (dispatched)",
                      static_cast<unsigned long long>(iva));
        Warn(b);
      }
}

}  // namespace lifter::ppc
