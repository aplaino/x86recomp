#include "function.h"

#include <cstdio>
#include <deque>

namespace lifter {

Decoder::Decoder() {
  ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
  ZydisFormatterInit(&fmt_, ZYDIS_FORMATTER_STYLE_INTEL);
}

bool Decoder::Decode(const Program& p, uint32_t va, Insn* out) const {
  out->va = va;
  out->ok = false;
  if (!p.InText(va)) return false;
  size_t n = p.BytesLeft(va);
  if (n > ZYDIS_MAX_INSTRUCTION_LENGTH) n = ZYDIS_MAX_INSTRUCTION_LENGTH;
  out->ok = ZYAN_SUCCESS(
      ZydisDecoderDecodeFull(&dec_, p.Bytes(va), n, &out->d, out->ops));
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

uint32_t RelTarget(const Insn& in) {
  ZyanU64 t = 0;
  ZydisCalcAbsoluteAddress(&in.d, &in.ops[0], in.va, &t);
  return static_cast<uint32_t>(t);
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
    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool IsOtherEntry(const Program& p, const Function& f, uint32_t t) {
  return t != f.entry && p.Splits(t);
}

Flow Classify(const Program& p, const Function& f, const Insn& in,
              uint32_t* target) {
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

void BuildFunction(const Program& p, const Decoder& dec, uint32_t entry,
                   Function* out) {
  Function& f = *out;
  f.entry = entry;
  std::deque<uint32_t> work{entry};
  f.labels.insert(entry);

  auto reach = [&](uint32_t va) {
    if (!p.InText(va)) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "branch to %08X outside .text", va);
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
    uint32_t va = work.front();
    work.pop_front();
    if (f.insns.count(va)) continue;
    Insn in;
    dec.Decode(p, va, &in);
    f.insns.emplace(va, in);
    if (!in.ok) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "undecodable bytes at %08X", va);
      f.warnings.push_back(buf);
      continue;
    }
    uint32_t t = 0;
    switch (Classify(p, f, in, &t)) {
      case Flow::Fall:
        if (in.d.mnemonic == ZYDIS_MNEMONIC_CALL &&
            in.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
          uint32_t ct = RelTarget(in);
          if (ct != in.next()) f.direct_calls.insert(ct);
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
        for (uint32_t jt : p.jumptables.at(va)) {
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
