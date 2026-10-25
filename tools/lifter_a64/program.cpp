#include "program.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>

#include "a64/lifted.h"
#include "a64/text_hash.h"

namespace lifter::a64 {

using a64::Insn;
using a64::Op;

bool WritesReg(const Insn& i, int r) {
  auto dest = [&](bool sp_ok) { return i.rd == r && (r != 31 || sp_ok); };
  bool wb = (i.mode == a64::kPreIndex || i.mode == a64::kPostIndex) && i.rn == r;
  switch (i.op) {
    case Op::kAdr: case Op::kAddShift: case Op::kAdc: case Op::kLogShift: case Op::kMovWide:
    case Op::kBitfield: case Op::kExtr: case Op::kCondSel: case Op::kDp1: case Op::kDp2:
    case Op::kDp3: case Op::kMrs:
      return dest(false);
    case Op::kAddImm: case Op::kAddExt: case Op::kLogImm:
      return dest(!i.setf);
    case Op::kBl: case Op::kBlr: case Op::kSvc:
      return true;  // the callee may change anything
    case Op::kLdSt:
      return (i.load && dest(false)) || wb;
    case Op::kLdStPair:
      return (i.load && (dest(false) || (i.ra == r && r != 31))) || wb;
    case Op::kLdLit: case Op::kLdStOrder:
      return i.load && dest(false);
    case Op::kLdStExcl:
      return i.load ? dest(false) : i.rm == r && r != 31;
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
    if (r.Match(path)) return r.Load(img_, path, err);
  err = path + ": no image reader for this format (is its platform module enabled?)";
  return false;
}

uint64_t Program::text_hash() const {
  return recomp::a64::TextHash(img_.bytes.data() + (img_.text_lo - img_.base),
                               img_.text_hi - img_.text_lo);
}

uint32_t Program::Word(uint64_t va) const { return uint32_t(Read(va, 4, false)); }

uint64_t Program::Read(uint64_t va, int size, bool sign) const {
  uint64_t off = va - img_.base, v = 0;
  if (va < img_.base || off + size > img_.bytes.size()) return 0;
  std::memcpy(&v, img_.bytes.data() + off, size);
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

// The value of `reg` before the instruction at `va`, if the straight-line
// code above it (back to `lo`) computes a constant: ADR/ADRP, then ADD/SUB
// immediates, or MOVZ.
bool Program::ConstOf(uint64_t va, uint64_t lo, int reg, uint64_t& out, int depth) const {
  if (depth > 4) return false;
  for (uint64_t p = va; p > lo && va - p < 16 * 4;) {
    p -= 4;
    Insn i = a64::Decode(p, Word(p));
    if (!WritesReg(i, reg)) continue;
    if (i.op == Op::kAdr) {
      out = uint64_t(i.imm);
      return true;
    }
    if (i.op == Op::kMovWide && i.opc == 2 && i.sf) {
      out = uint64_t(i.imm) << i.amount;
      return true;
    }
    uint64_t v;
    if (i.op == Op::kAddImm && !i.setf && i.sf && i.rn != 31 && ConstOf(p, lo, i.rn, v, depth + 1)) {
      out = i.sub ? v - uint64_t(i.imm) : v + uint64_t(i.imm);
      return true;
    }
    return false;
  }
  return false;
}

// Compiler jump tables: `ldr{b,h,,sw} E, [TABLE, IDX, ...]`, `add T, BASE,
// E{, lsl/sxtw #n}`, `br T`, with TABLE and BASE constants and IDX bounded
// by a `cmp IDX, #n` + `b.hi`/`b.hs` or an `and IDX, X, #2^k-1` above it
// (following register copies). Each entry's target is computed exactly as
// the code computes it. The walk is over straight-line code above the BR,
// which is how compilers lay these out.
bool Program::JumpTable(const Function& f, uint64_t br_va, std::vector<uint64_t>& targets) {
  Insn br = a64::Decode(br_va, Word(br_va));
  auto writer = [&](uint64_t from, int reg, Insn& out) {
    for (uint64_t p = from; p > f.va && from - p < 24 * 4;) {
      p -= 4;
      Insn i = a64::Decode(p, Word(p));
      if (WritesReg(i, reg)) {
        out = i;
        return true;
      }
    }
    return false;
  };
  Insn add, ld;
  if (!writer(br_va, br.rn, add) || add.sub || add.setf || !add.sf) return false;
  if (!(add.op == Op::kAddShift && add.shift == a64::kLsl) && add.op != Op::kAddExt) return false;
  uint64_t base, table;
  if (!ConstOf(add.va, f.va, add.rn, base)) return false;
  if (!writer(add.va, add.rm, ld) || ld.op != Op::kLdSt || !ld.load || ld.mode != a64::kRegOffset)
    return false;
  if (!ConstOf(ld.va, f.va, ld.rn, table)) return false;

  std::set<int> aliases = {ld.rm};
  int cond = -1;
  uint64_t count = 0;
  for (uint64_t p = ld.va; p > f.va && ld.va - p < 24 * 4 && !count;) {
    p -= 4;
    Insn i = a64::Decode(p, Word(p));
    if (i.op == Op::kBCond) {
      if (cond < 0) cond = i.cond;
      continue;
    }
    if (i.op == Op::kAddImm && i.sub && i.setf && i.rd == 31 && aliases.count(i.rn)) {
      if (cond == 8) count = uint64_t(i.imm) + 1;       // b.hi
      else if (cond == 2) count = uint64_t(i.imm);      // b.hs
      else return false;
      break;
    }
    bool def = false;
    for (int r : aliases) def = def || WritesReg(i, r);
    if (!def) continue;
    if (i.op == Op::kLogImm && i.opc == 0 && aliases.count(i.rd) &&
        ((uint64_t(i.imm) + 1) & uint64_t(i.imm)) == 0) {
      count = uint64_t(i.imm) + 1;
      break;
    }
    if (i.op == Op::kLogShift && i.opc == 1 && !i.inv && i.rn == 31 && i.amount == 0 &&
        aliases.count(i.rd)) {
      aliases.erase(i.rd);
      aliases.insert(i.rm);
      continue;
    }
    return false;
  }
  if (!count || count > 4096) return false;

  std::set<uint64_t> seen;
  for (uint64_t k = 0; k < count; k++) {
    uint64_t e = Read(table + (k << ld.amount), ld.size, ld.sign);
    if (!ld.sf) e = uint32_t(e);
    uint64_t off = add.op == Op::kAddExt ? recomp::a64::lift::Extend(e, add.shift, add.amount)
                                         : e << add.amount;
    uint64_t t = base + off;
    if (!InText(t) || t % 4) return false;
    if (seen.insert(t).second) targets.push_back(t);
  }
  return true;
}

Function Program::Build(uint64_t start, uint64_t end, std::set<uint64_t>& found) {
  Function f;
  f.va = start;
  f.end = end;
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
    const Insn& i = f.insns[va] = a64::Decode(va, Word(va));
    switch (i.op) {
      case Op::kB:
        branch(uint64_t(i.imm));
        continue;
      case Op::kBCond: case Op::kCbz: case Op::kTbz:
        branch(uint64_t(i.imm));
        break;
      case Op::kBl:
        found.insert(uint64_t(i.imm));
        break;
      case Op::kBr: {
        std::vector<uint64_t> targets;
        if (JumpTable(f, va, targets)) {
          for (uint64_t t : targets) branch(t);
          f.tables[va] = std::move(targets);
        }
        continue;
      }
      default:
        if (a64::IsTerminator(i)) continue;
        break;
    }
    work.push_back(va + 4);
  }
  return f;
}

void Program::Discover() {
  uint64_t text_end = img_.text_hi;
  std::set<uint64_t> starts;
  for (const auto& [va, size] : img_.starts) {
    starts.insert(va);
    if (size) sized_.emplace(va, size);
  }
  for (;;) {
    funcs_.clear();
    std::set<uint64_t> found;
    for (auto it = starts.begin(); it != starts.end(); ++it) {
      auto next = std::next(it);
      uint64_t end = next == starts.end() ? text_end : *next;
      auto sz = sized_.find(*it);
      if (sz != sized_.end()) end = std::min(text_end, *it + sz->second);
      funcs_[*it] = Build(*it, end, found);
    }
    bool grew = false;
    for (uint64_t t : found) {
      if (starts.count(t)) continue;
      if (!InText(t) || t % 4) {
        char b[96];
        std::snprintf(b, sizeof b, "branch target %llx is outside .text",
                      static_cast<unsigned long long>(t));
        if (std::find(warnings_.begin(), warnings_.end(), b) == warnings_.end())
          warnings_.push_back(b);
        continue;
      }
      starts.insert(t);
      grew = true;
    }
    if (!grew) break;
  }
}

}  // namespace lifter::a64
