#include "function.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>

#include "runtime/unwind.h"

namespace lifter {

Decoder::Decoder(bool x64) {
  if (x64)
    ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  else
    ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
  ZydisFormatterInit(&fmt_, ZYDIS_FORMATTER_STYLE_INTEL);
}

bool Decoder::Decode(const Program& p, uint64_t va, Insn* out) const {
  out->va = va;
  out->ok = false;
  if (!p.InText(va)) return false;
  size_t n = p.BytesLeft(va);
  if (n > ZYDIS_MAX_INSTRUCTION_LENGTH) n = ZYDIS_MAX_INSTRUCTION_LENGTH;
  out->ok = ZYAN_SUCCESS(
      ZydisDecoderDecodeFull(&dec_, p.Bytes(va), n, &out->d, out->ops));
  return out->ok;
}

bool Decoder::DecodeRaw(const uint8_t* bytes, size_t n, uint64_t va, Insn* out) const {
  out->va = va;
  out->ok = ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec_, bytes, n, &out->d, out->ops));
  return out->ok;
}

std::string Decoder::Format(const Insn& in) const {
  if (!in.ok) return "(bad)";
  char buf[256];
  ZydisFormatterFormatInstruction(&fmt_, &in.d, in.ops,
                                  in.d.operand_count_visible, buf, sizeof buf,
                                  in.va, ZYAN_NULL);
  return buf;
}

uint64_t RelTarget(const Insn& in) {
  ZyanU64 t = 0;
  ZydisCalcAbsoluteAddress(&in.d, &in.ops[0], in.va, &t);
  // 32-bit code wraps around the 4 GiB address space.
  return in.d.machine_mode == ZYDIS_MACHINE_MODE_LONG_64 ? t : uint32_t(t);
}

namespace {

bool IsJcc(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ:
    case ZYDIS_MNEMONIC_JRCXZ:
    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool IsOtherEntry(const Program& p, const Function& f, uint64_t t) {
  return t != f.entry && p.Splits(t);
}

Flow Classify(const Program& p, const Function& f, const Insn& in,
              uint64_t* target) {
  *target = 0;
  if (!in.ok) return Flow::Stop;
  const auto& d = in.d;
  const auto& op0 = in.ops[0];
  switch (d.mnemonic) {
    case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_IRET:
    case ZYDIS_MNEMONIC_IRETD:
      return Flow::Return;
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_INT1:
      return d.mnemonic == ZYDIS_MNEMONIC_INT ? Flow::Fall : Flow::Stop;
    case ZYDIS_MNEMONIC_JMP:
      if (op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        *target = RelTarget(in);
        return IsOtherEntry(p, f, *target) ? Flow::TailCall : Flow::Jump;
      }
      if (op0.type == ZYDIS_OPERAND_TYPE_POINTER) return Flow::Stop;
      return p.jumptables.count(in.va) ? Flow::JumpTable : Flow::TailDispatch;
    case ZYDIS_MNEMONIC_CALL:
      if (op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        *target = RelTarget(in);
        if (p.noreturn.count(*target)) return Flow::Stop;
      } else if (op0.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 op0.mem.base == ZYDIS_REGISTER_NONE &&
                 op0.mem.index == ZYDIS_REGISTER_NONE &&
                 p.noreturn_iat.count(
                     static_cast<uint32_t>(op0.mem.disp.value))) {
        return Flow::Stop;
      } else if (op0.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 op0.mem.base == ZYDIS_REGISTER_RIP &&
                 p.noreturn_iat.count(in.next() + op0.mem.disp.value)) {
        return Flow::Stop;
      }
      return Flow::Fall;
    default:
      break;
  }
  if (IsJcc(d.mnemonic)) {
    *target = RelTarget(in);
    return Flow::Branch;
  }
  return Flow::Fall;
}

namespace {

// The 64-bit register enclosing r (rax for eax, ax, al and ah alike), so
// comparisons work for both 32-bit and x64 code.
ZydisRegister Full(ZydisRegister r) {
  return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
}

bool WritesReg(const Insn& in, ZydisRegister r) {
  for (int i = 0; i < in.d.operand_count; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        (in.ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
        Full(in.ops[i].reg.value) == Full(r))
      return true;
  return false;
}

bool WritesFlags(const Insn& in) {
  return in.d.cpu_flags &&
         (in.d.cpu_flags->modified | in.d.cpu_flags->set_0 |
          in.d.cpu_flags->set_1 | in.d.cpu_flags->undefined) != 0;
}

// The decoded instruction of f that ends at va, if any.
const Insn* Before(const Function& f, uint64_t va) {
  auto it = f.insns.lower_bound(va);
  if (it == f.insns.begin()) return nullptr;
  --it;
  return it->second.ok && it->second.next() == va ? &it->second : nullptr;
}

}  // namespace

namespace {

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

// x64 MSVC switches: a table of 32-bit RVAs indexed off the image base
// (which some register holds), sometimes through a byte index table, with
// the index widened between the bounds check and the load:
//   cmp eI, N; ja default; [movsxd rI, eJ | cdqe] [lea rB, [__ImageBase]]
//   [movzx eI, byte [rK + rB + index_rva]] mov eT, [rB + rI*4 + table_rva]
//   add rT, rB; jmp rT
// Without a bounds check (an index the compiler knows is in range, such as
// a state variable), the table runs while its entries target the jmp's own
// function, to the end of the .pdata range that holds it (MSVC places a
// function's tables inside its range, after the code).
// Clang's tables hold signed offsets from the table itself, which a
// RIP-relative lea put in a register:
//   lea rB, [table]; ... cmp eI, N; ja default; movsxd rT, [rB + rI*4]
//   add rT, rB; jmp rT
// Both are "target = B + entry": B and the load's base register are taken
// from the lea that sets them on every path to the jmp, or are the image
// base.
bool RecoverJumpTable64(const Program& p, const Function& f, const Insn& jmp,
                        std::vector<uint64_t>* targets) {
  const auto& j0 = jmp.ops[0];
  if (!jmp.ok || jmp.d.mnemonic != ZYDIS_MNEMONIC_JMP ||
      j0.type != ZYDIS_OPERAND_TYPE_REGISTER)
    return false;
  const Module* mod = p.TextOf(jmp.va);
  const uint64_t image = mod->base;
  auto in_text = [&](uint64_t a, uint64_t n) {
    if (!n) return false;
    if (p.TextOf(a) == mod && p.TextOf(a + n - 1) == mod) return true;
    for (const auto& [lo, hi] : mod->rodata)
      if (a >= lo && a + n <= hi) return true;
    return false;
  };
  auto reg = [](const ZydisDecodedOperand& o) {
    return o.type == ZYDIS_OPERAND_TYPE_REGISTER ? Full(o.reg.value) : ZYDIS_REGISTER_NONE;
  };
  const ZydisRegister t = Full(j0.reg.value);
  enum { kAdd, kLoad, kJa, kCmp, kAlias } state = kAdd;
  ZydisRegister idx = ZYDIS_REGISTER_NONE, base = ZYDIS_REGISTER_NONE;
  uint64_t table = 0, index = 0, added = image;
  bool two_level = false, sign = false;
  // The instruction before `in` on the paths to it: the one falling into
  // it, or where none does (a block the bounds check branches to), the
  // only branch to it (`taken`).
  auto prev = [&](const Insn* in, bool* taken) -> const Insn* {
    *taken = false;
    const Insn* b = Before(f, in->va);
    if (b && b->d.mnemonic != ZYDIS_MNEMONIC_JMP && b->d.mnemonic != ZYDIS_MNEMONIC_RET)
      return b;
    const Insn* only = nullptr;
    for (const auto& [va, i] : f.insns)
      if (i.ok && i.d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE &&
          i.d.mnemonic != ZYDIS_MNEMONIC_CALL && i.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
          RelTarget(i) == in->va) {
        if (only) return nullptr;
        only = &i;
      }
    *taken = only != nullptr;
    return only;
  };
  // Where control reaching an instruction of f can come from: the one
  // falling into it and the direct branches to it. False where something
  // else may reach it (the function entry, a switch case, a setjmp return).
  std::map<uint64_t, std::vector<const Insn*>> branches_to;
  std::set<uint64_t> cases;
  for (const auto& [va, i] : f.insns) {
    if (i.ok && i.d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE &&
        i.d.mnemonic != ZYDIS_MNEMONIC_CALL && i.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
      branches_to[RelTarget(i)].push_back(&i);
    if (auto jt = p.jumptables.find(va); jt != p.jumptables.end())
      cases.insert(jt->second.begin(), jt->second.end());
  }
  auto preds = [&](const Insn* in, std::vector<const Insn*>* out) {
    if (in->va == f.entry || cases.count(in->va) || f.setjmp_returns.count(in->va))
      return false;
    const Insn* b = Before(f, in->va);
    if (b && b->d.mnemonic != ZYDIS_MNEMONIC_JMP && b->d.mnemonic != ZYDIS_MNEMONIC_RET)
      out->push_back(b);
    if (auto it = branches_to.find(in->va); it != branches_to.end())
      out->insert(out->end(), it->second.begin(), it->second.end());
    return !out->empty();
  };
  // The value r holds on entry to `in` if a RIP-relative lea set it on
  // every path there (paths around a loop back to `in` do not count), or
  // the image base.
  constexpr uint64_t kAny = ~uint64_t(0), kUnknown = ~uint64_t(1);
  std::set<std::pair<uint64_t, ZydisRegister>> seen;
  int budget = 0;
  std::function<uint64_t(ZydisRegister, const Insn*)> reaching = [&](ZydisRegister r,
                                                                      const Insn* in) {
    if (!seen.insert({in->va, r}).second) return kAny;
    std::vector<const Insn*> ps;
    if (--budget < 0 || !preds(in, &ps)) return kUnknown;
    uint64_t v = kAny;
    for (const Insn* q : ps) {
      uint64_t w;
      if (!q->ok) {
        w = kUnknown;
      } else if (WritesReg(*q, r)) {
        const auto& src = q->ops[1];
        if (q->d.mnemonic == ZYDIS_MNEMONIC_LEA && src.mem.base == ZYDIS_REGISTER_RIP &&
            src.mem.index == ZYDIS_REGISTER_NONE)
          w = q->next() + uint64_t(src.mem.disp.value);
        else if (q->d.mnemonic == ZYDIS_MNEMONIC_MOV && q->ops[0].size == 64 && reg(src))
          w = reaching(reg(src), q);  // a copy
        else
          w = kUnknown;
      } else {
        w = reaching(r, q);
      }
      if (w == kAny) continue;
      if (w == kUnknown || (v != kAny && v != w)) return kUnknown;
      v = w;
    }
    return v;
  };
  auto lea_value = [&](ZydisRegister r, const Insn* in) {
    seen.clear();
    budget = 256;
    const uint64_t v = reaching(r, in);
    return v == kAny || v == kUnknown ? image : v;
  };
  auto entry = [&](uint64_t a) {
    return added + (sign ? uint64_t(int64_t(p.mem.read<int32_t>(a))) : p.mem.read<uint32_t>(a));
  };
  auto unbounded = [&]() {
    if (state < kJa || two_level) return false;
    const recomp::PeImage& img = mod->pe;
    const uint64_t home = PrimaryOf(p, img, PdataAt(img, jmp.va));
    const recomp::PeFunction* holder = PdataAt(img, table);
    if (!home || !holder || PrimaryOf(p, img, holder) != home) return false;
    targets->clear();
    for (uint64_t a = table; a + 4 <= holder->end; a += 4) {
      uint64_t tv = entry(a);
      if (p.TextOf(tv) != mod || PrimaryOf(p, img, PdataAt(img, tv)) != home) break;
      if (std::find(targets->begin(), targets->end(), tv) == targets->end())
        targets->push_back(tv);
    }
    return !targets->empty();
  };
  auto bounded = [&](uint32_t cases) {
    if (two_level) {
      if (!in_text(index, cases)) return false;
      uint32_t n = cases;
      cases = 0;
      for (uint32_t i = 0; i < n; i++)
        cases = std::max<uint32_t>(cases, p.mem.read<uint8_t>(index + i) + 1u);
    }
    if (!in_text(table, uint64_t(cases) * 4)) return false;
    targets->clear();
    for (uint32_t i = 0; i < cases; i++) {
      uint64_t tv = entry(table + 4 * i);
      if (p.TextOf(tv) != mod) return false;
      if (std::find(targets->begin(), targets->end(), tv) == targets->end())
        targets->push_back(tv);
    }
    return true;
  };
  // jcc on the path: the index is at most N past ja falling through or
  // jbe taken, below N past jae falling through or jb taken.
  bool below = false;
  ZydisRegister alias = ZYDIS_REGISTER_NONE;  // compared, then copied to idx
  uint32_t alias_cases = 0;
  const Insn* in = &jmp;
  for (int steps = 0; steps < 16; steps++) {
    bool taken;
    in = prev(in, &taken);
    if (!in) return unbounded();
    if (taken && in->d.mnemonic == ZYDIS_MNEMONIC_JMP) continue;
    const auto& d = in->d;
    const auto& o0 = in->ops[0];
    const auto& o1 = in->ops[1];
    switch (state) {
      case kAdd:
        if (d.mnemonic == ZYDIS_MNEMONIC_ADD && reg(o0) == t && o0.size == 64 &&
            o1.type == ZYDIS_OPERAND_TYPE_REGISTER) {
          added = lea_value(Full(o1.reg.value), in);
          state = kLoad;
        } else if (WritesReg(*in, t) || d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
          return false;
        }
        break;
      case kLoad:
        if ((d.mnemonic == ZYDIS_MNEMONIC_MOV || d.mnemonic == ZYDIS_MNEMONIC_MOVSXD) &&
            reg(o0) == t && o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 32 &&
            o1.mem.base != ZYDIS_REGISTER_NONE && o1.mem.base != ZYDIS_REGISTER_RIP &&
            o1.mem.index != ZYDIS_REGISTER_NONE && o1.mem.scale == 4) {
          idx = Full(o1.mem.index);
          base = Full(o1.mem.base);
          table = lea_value(base, in) + uint64_t(o1.mem.disp.value);
          sign = d.mnemonic == ZYDIS_MNEMONIC_MOVSXD;
          state = kJa;
        } else if (WritesReg(*in, t) || d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
          return false;
        }
        break;
      case kJa:
        if ((!taken && d.mnemonic == ZYDIS_MNEMONIC_JNBE) ||
            (taken && d.mnemonic == ZYDIS_MNEMONIC_JBE)) {
          state = kCmp;
        } else if ((!taken && d.mnemonic == ZYDIS_MNEMONIC_JNB) ||
                   (taken && d.mnemonic == ZYDIS_MNEMONIC_JB)) {
          below = true;
          state = kCmp;
        } else if (!two_level && d.mnemonic == ZYDIS_MNEMONIC_MOVZX && reg(o0) == idx &&
                   o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 8 &&
                   o1.mem.index != ZYDIS_REGISTER_NONE && o1.mem.scale == 1 &&
                   (Full(o1.mem.base) == base || Full(o1.mem.index) == base)) {
          // The image base register is either operand of the byte load.
          two_level = true;
          index = image + uint64_t(o1.mem.disp.value);
          idx = Full(o1.mem.base) == base ? Full(o1.mem.index) : Full(o1.mem.base);
        } else if ((d.mnemonic == ZYDIS_MNEMONIC_MOVSXD ||
                    (d.mnemonic == ZYDIS_MNEMONIC_MOV && o0.size == 32)) &&
                   reg(o0) == idx && o1.type == ZYDIS_OPERAND_TYPE_REGISTER) {
          idx = Full(o1.reg.value);  // widened from another register
        } else if (d.mnemonic == ZYDIS_MNEMONIC_CDQE && idx == ZYDIS_REGISTER_RAX) {
          // rax from eax: same register
        } else if (WritesReg(*in, idx) || d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
          return unbounded();
        }
        break;
      case kCmp:
        if (!WritesFlags(*in)) {
          if (WritesReg(*in, idx)) return unbounded();
          break;
        }
        if (d.mnemonic != ZYDIS_MNEMONIC_CMP || o1.type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
            o1.imm.value.u >= 4096 || reg(o0) == ZYDIS_REGISTER_NONE)
          return unbounded();
        if (reg(o0) == idx) return bounded(uint32_t(o1.imm.value.u) + !below);
        // Compared in another register that idx was copied from since.
        alias = reg(o0);
        alias_cases = uint32_t(o1.imm.value.u) + !below;
        state = kAlias;
        break;
      case kAlias:
        if ((d.mnemonic == ZYDIS_MNEMONIC_MOVSXD || d.mnemonic == ZYDIS_MNEMONIC_MOVZX ||
             (d.mnemonic == ZYDIS_MNEMONIC_MOV && o0.size == 32)) &&
            reg(o0) == idx && reg(o1) == alias)
          return bounded(alias_cases);
        if (WritesReg(*in, idx) || WritesReg(*in, alias) ||
            d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE)
          return unbounded();
        break;
    }
  }
  return unbounded();
}

}  // namespace

bool RecoverJumpTable(const Program& p, const Function& f, const Insn& jmp,
                      std::vector<uint64_t>* targets) {
  if (p.x64) return RecoverJumpTable64(p, f, jmp, targets);
  const auto& m = jmp.ops[0];
  if (!jmp.ok || jmp.d.mnemonic != ZYDIS_MNEMONIC_JMP ||
      m.type != ZYDIS_OPERAND_TYPE_MEMORY || m.mem.base != ZYDIS_REGISTER_NONE ||
      m.mem.index == ZYDIS_REGISTER_NONE || m.mem.scale != 4)
    return false;
  // MSVC places the tables in .text after the function.
  const Module* mod = p.TextOf(jmp.va);
  auto in_text = [&](uint64_t a, uint32_t n) {
    return n && p.TextOf(a) == mod && p.TextOf(a + n - 1) == mod;
  };
  // Walk back from the jmp through the straight-line code before it
  // (the compiler schedules unrelated instructions in between, e.g. a
  // run of x87 loads and stores): an optional movzx from the byte index
  // table, the ja, then the cmp that sets its flags.
  ZydisRegister r = m.mem.index;
  uint32_t index = 0;
  bool two_level = false, seen_ja = false;
  const Insn* in = &jmp;
  for (int steps = 0; steps < 16; steps++) {
    in = Before(f, in->va);
    if (!in) return false;
    const auto& o0 = in->ops[0];
    const auto& o1 = in->ops[1];
    if (!seen_ja) {
      if (in->d.mnemonic == ZYDIS_MNEMONIC_JNBE) {
        seen_ja = true;
      } else if (!two_level && in->d.mnemonic == ZYDIS_MNEMONIC_MOVZX &&
                 o0.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                 Full(o0.reg.value) == Full(r) &&
                 o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 8 &&
                 o1.mem.base != ZYDIS_REGISTER_NONE &&
                 o1.mem.index == ZYDIS_REGISTER_NONE) {
        two_level = true;
        index = static_cast<uint32_t>(o1.mem.disp.value);
        r = o1.mem.base;
      } else if (WritesReg(*in, r) || in->d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
        return false;
      }
      continue;
    }
    if (!WritesFlags(*in)) {
      if (WritesReg(*in, r) || in->d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) return false;
      continue;
    }
    if (in->d.mnemonic != ZYDIS_MNEMONIC_CMP ||
        o0.type != ZYDIS_OPERAND_TYPE_REGISTER || Full(o0.reg.value) != Full(r) ||
        o1.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || o1.imm.value.u >= 4096)
      return false;
    uint32_t cases = static_cast<uint32_t>(o1.imm.value.u) + 1;
    if (two_level) {
      // A byte index table picks the entry.
      if (!in_text(index, cases)) return false;
      uint32_t n = cases;
      cases = 0;
      for (uint32_t i = 0; i < n; i++)
        cases = std::max<uint32_t>(cases, p.mem.read<uint8_t>(index + i) + 1u);
    }
    if (!in_text(static_cast<uint32_t>(m.mem.disp.value), cases * 4)) return false;
    targets->clear();
    for (uint32_t i = 0; i < cases; i++) {
      uint64_t t = p.mem.read<uint32_t>(static_cast<uint32_t>(m.mem.disp.value) + 4 * i);
      if (p.TextOf(t) != mod) return false;
      // Cases often share a target; each becomes one switch case.
      if (std::find(targets->begin(), targets->end(), t) == targets->end())
        targets->push_back(t);
    }
    return true;
  }
  return false;
}

void BuildFunction(const Program& p, const Decoder& dec, uint64_t entry,
                   Function* out) {
  Function& f = *out;
  f.entry = entry;
  std::deque<uint64_t> work{entry};
  f.labels.insert(entry);

  auto reach = [&](uint64_t va) {
    if (!p.InText(va)) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "branch to %08llX outside .text",
                    (unsigned long long)va);
      f.warnings.push_back(buf);
      return;
    }
    // Falling or jumping into another function's entry is a tail call,
    // handled by the emitter; never lift that code into this function.
    if (IsOtherEntry(p, f, va)) {
      f.tail_calls.insert(va);
      return;
    }
    if (!f.insns.count(va)) work.push_back(va);
  };

  while (!work.empty()) {
    uint64_t va = work.front();
    work.pop_front();
    if (f.insns.count(va)) continue;
    Insn in;
    dec.Decode(p, va, &in);
    f.insns.emplace(va, in);
    if (!in.ok) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "undecodable bytes at %08llX",
                    (unsigned long long)va);
      f.warnings.push_back(buf);
      continue;
    }
    uint64_t t = 0;
    switch (Classify(p, f, in, &t)) {
      case Flow::Fall:
        if (in.d.mnemonic == ZYDIS_MNEMONIC_CALL &&
            in.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
          uint64_t ct = RelTarget(in);
          if (ct != in.next()) f.direct_calls.insert(ct);
          if (p.setjmps.count(ct)) f.setjmp_returns.insert(in.next());
        }
        reach(in.next());
        break;
      case Flow::Branch:
        if (IsOtherEntry(p, f, t)) {
          f.tail_calls.insert(t);
        } else {
          f.labels.insert(t);
          reach(t);
        }
        reach(in.next());
        break;
      case Flow::Jump:
        f.labels.insert(t);
        reach(t);
        break;
      case Flow::TailCall:
        f.tail_calls.insert(t);
        break;
      case Flow::JumpTable:
        for (uint64_t jt : p.jumptables.at(va)) {
          if (IsOtherEntry(p, f, jt)) {
            f.tail_calls.insert(jt);
          } else {
            f.labels.insert(jt);
            reach(jt);
          }
        }
        break;
      case Flow::TailDispatch:
      case Flow::Return:
      case Flow::Stop:
        break;
    }
  }
}

}  // namespace lifter
