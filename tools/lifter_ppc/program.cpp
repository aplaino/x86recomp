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
    case Op::kStore:
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
    Insn i = ppc::Decode(p, Word(p));
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
// `bgt crN` (or `bge` on n) above it. Each entry's target is computed as
// the code computes it. The walk is over straight-line code above the
// BCCTR, which is how compilers lay these out.
bool Program::JumpTable(const Function& f, uint64_t bcctr_va, std::vector<uint64_t>& targets) {
  auto writer = [&](uint64_t from, auto pred, Insn& out) {
    for (uint64_t p = from; p > f.va && from - p < 24 * 4;) {
      p -= 4;
      Insn i = ppc::Decode(p, Word(p));
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

  int crf = -1;
  bool gt = false;
  uint64_t count = 0;
  for (uint64_t p = from; p > f.va && from - p < 24 * 4;) {
    p -= 4;
    Insn i = ppc::Decode(p, Word(p));
    // bgt crN (BO 12, BI 4N+1) or bge crN (BO 4, BI 4N+0) to the default.
    if (i.op == Op::kBc && !i.lk && crf < 0) {
      if (i.bo == 12 && i.bi % 4 == 1) gt = true;
      else if (i.bo == 4 && i.bi % 4 == 0) gt = false;
      else return false;
      crf = i.bi / 4;
      continue;
    }
    if (i.op == Op::kCmpli && !i.l && i.crfd == crf && i.ra == x) {
      count = uint64_t(i.imm) + (gt ? 1 : 0);
      break;
    }
    if (WritesReg(i, x)) return false;
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
    const Insn& i = f.insns[va] = ppc::Decode(va, Word(va));
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
  return f;
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
    if (!grew) break;
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
