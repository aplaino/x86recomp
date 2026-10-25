#include "emit.h"

#include <cstdio>
#include <set>

namespace lifter::a64 {
namespace {

using a64::Insn;
using a64::Op;

std::string Hex(uint64_t v) {
  char b[32];
  std::snprintf(b, sizeof b, "0x%llXull", static_cast<unsigned long long>(v));
  return b;
}
std::string HexVa(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%llX", static_cast<unsigned long long>(v));
  return b;
}
std::string Label(uint64_t va) { return "L_" + HexVa(va); }
std::string N(int v) { return std::to_string(v); }

// Register operands. 31 is XZR unless the operand position allows SP.
std::string X(int r) { return r == 31 ? "uint64_t(0)" : "x" + N(r); }
std::string W(int r) { return r == 31 ? "uint32_t(0)" : "uint32_t(x" + N(r) + ")"; }
std::string XSp(int r) { return r == 31 ? "sp" : "x" + N(r); }

// Registers an instruction names (its non-31 register fields).
void UsedRegs(const Insn& i, bool* regs) {
  for (int lo : {0, 5, 10, 16})
    if (i.reg_fields & (0x1Fu << lo)) regs[(i.raw >> lo) & 0x1F] = true;
  if (i.op == Op::kBl || i.op == Op::kBlr) regs[30] = true;
}
bool UsesFlags(const Insn& i) {
  switch (i.op) {
    case Op::kBCond: case Op::kCondSel: case Op::kCondCmp: case Op::kAdc: return true;
    case Op::kMrs: case Op::kMsr: return i.sysreg == a64::kNzcv;
    default: return i.setf;
  }
}

const char* kCond[16] = {
    "f.z", "!f.z", "f.c", "!f.c", "f.n", "!f.n", "f.v", "!f.v",
    "f.c && !f.z", "!(f.c && !f.z)", "f.n == f.v", "f.n != f.v",
    "!f.z && f.n == f.v", "!(!f.z && f.n == f.v)", "true", "true"};

const char* UType(int size) {
  switch (size) {
    case 1: return "uint8_t";
    case 2: return "uint16_t";
    case 4: return "uint32_t";
    default: return "uint64_t";
  }
}
const char* SType(int size) {
  switch (size) {
    case 1: return "int8_t";
    case 2: return "int16_t";
    case 4: return "int32_t";
    default: return "int64_t";
  }
}

class Emitter {
 public:
  Emitter(const Program& p, const Function* f) : p_(p), f_(f) {}

  void UseAll() {
    for (bool& r : regs_) r = true;
    flags_ = true;
  }
  void Use(const Insn& i) {
    UsedRegs(i, regs_);
    flags_ = flags_ || UsesFlags(i);
  }
  std::string Prologue() const {
    std::string s;
    for (int r = 0; r < 31; r++)
      if (regs_[r]) s += "  uint64_t x" + N(r) + " = c.x[" + N(r) + "];\n";
    s += "  uint64_t sp = c.sp;\n";
    if (flags_) s += "  lift::Nzcv f = lift::Unpack(c.nzcv);\n";
    return s;
  }
  std::string Spill() const {
    std::string s;
    for (int r = 0; r < 31; r++)
      if (regs_[r]) s += "c.x[" + N(r) + "] = x" + N(r) + "; ";
    s += "c.sp = sp; ";
    if (flags_) s += "c.nzcv = lift::Pack(f); ";
    return s;
  }
  std::string Reload() const {
    std::string s;
    for (int r = 0; r < 31; r++)
      if (regs_[r]) s += "x" + N(r) + " = c.x[" + N(r) + "]; ";
    s += "sp = c.sp; ";
    if (flags_) s += "f = lift::Unpack(c.nzcv); ";
    return s;
  }

  // Control transfer to `t` from the instruction at `va`.
  std::string Branch(uint64_t va, uint64_t t) {
    if (f_ && f_->insns.count(t)) return "goto " + Label(t) + ";";
    if (p_.functions().count(t)) {
      callees_.insert(t);
      return "{ " + Spill() + FunctionName(t) + "(c); return; }";
    }
    return "{ " + Spill() + "BadJump(c, " + Hex(va) + ", " + Hex(t) + "); }";
  }

  std::string Body(const Insn& i);
  const std::set<uint64_t>& callees() const { return callees_; }

 private:
  const Program& p_;
  const Function* f_;
  bool regs_[31] = {};
  bool flags_ = false;
  std::set<uint64_t> callees_;
};

std::string Emitter::Body(const Insn& i) {
  const std::string T = i.sf ? "uint64_t" : "uint32_t";
  const int bits = i.sf ? 64 : 32;
  const uint64_t mask = i.sf ? ~0ull : 0xFFFFFFFFull;
  auto rd = [&](int r) { return i.sf ? X(r) : W(r); };
  auto rdsp = [&](int r) { return i.sf ? XSp(r) : "uint32_t(" + XSp(r) + ")"; };
  // Writes a W or X result (W results zero-extend).
  auto set = [&](int r, bool sf, const std::string& v, bool sp_ok = false) {
    std::string dst = r != 31 ? "x" + N(r) : sp_ok ? "sp" : "";
    if (dst.empty()) return "(void)(" + v + ");";
    return dst + " = " + (sf ? "uint64_t(" + v + ")" : "uint64_t(uint32_t(" + v + "))") + ";";
  };
  auto shifted = [&](const std::string& v) {
    static const char* kFn[4] = {"Lsl", "Lsr", "Asr", "Ror"};
    if (!i.amount) return v;
    return std::string("lift::") + kFn[i.shift] + "<" + T + ">(" + v + ", " + N(i.amount) + ")";
  };
  auto addsub = [&](const std::string& a, const std::string& b, const std::string& carry) {
    if (!i.setf && carry.empty()) return T + "(" + a + (i.sub ? " - " : " + ") + b + ")";
    std::string fp = i.setf ? "&f" : "nullptr";
    std::string c = carry.empty() ? (i.sub ? "true" : "false") : carry;
    return "lift::Adc<" + T + ">(" + fp + ", " + a + ", " + (i.sub ? T + "(~" + b + ")" : b) +
           ", " + c + ")";
  };
  // AND ORR EOR ANDS; only the immediate forms may write SP.
  auto logic = [&](const std::string& a, const std::string& b, bool sp_ok) {
    static const char* kOp[4] = {" & ", " | ", " ^ ", " & "};
    std::string v = T + "(" + a + kOp[i.opc] + b + ")";
    if (!i.setf) return set(i.rd, i.sf, v, sp_ok);
    return "{ " + T + " r = " + v + "; f.n = (r >> " + N(bits - 1) +
           ") != 0; f.z = r == 0; f.c = false; f.v = false; " + set(i.rd, i.sf, "r") + " }";
  };
  // Memory: the address, then the access, then base writeback.
  auto address = [&]() {
    std::string base = XSp(i.rn);
    if (i.mode == a64::kRegOffset)
      return base + " + lift::Extend(" + X(i.rm) + ", " + N(i.shift) + ", " + N(i.amount) + ")";
    if (i.mode == a64::kPostIndex || !i.imm) return base;
    return i.imm < 0 ? base + " - " + Hex(uint64_t(-i.imm)) : base + " + " + Hex(uint64_t(i.imm));
  };
  auto writeback = [&]() -> std::string {
    std::string rn = i.rn == 31 ? "sp" : "x" + N(i.rn);
    if (i.mode == a64::kPreIndex) return " " + rn + " = a;";
    if (i.mode == a64::kPostIndex)
      return " " + rn + " = a " + (i.imm < 0 ? "- " + Hex(uint64_t(-i.imm)) : "+ " + Hex(uint64_t(i.imm))) + ";";
    return "";
  };
  auto loaded = [&](const std::string& v) {
    if (!i.sign) return v;
    return std::string(i.sf ? "uint64_t(int64_t(" : "uint32_t(int32_t(") + SType(i.size) + "(" + v + ")))";
  };
  const char* U = UType(i.size);

  switch (i.op) {
    case Op::kInvalid: {
      char b[16];
      std::snprintf(b, sizeof b, "%08X", i.raw);
      return Spill() + "Unimplemented(c, " + Hex(i.va) + ", \"instruction " + b + "\");";
    }
    case Op::kAdr:
      return set(i.rd, true, Hex(uint64_t(i.imm)));
    case Op::kAddImm:
      return set(i.rd, i.sf, addsub(rdsp(i.rn), T + "(" + Hex(uint64_t(i.imm)) + ")", ""), !i.setf);
    case Op::kAddShift:
      return set(i.rd, i.sf, addsub(rd(i.rn), shifted(rd(i.rm)), ""));
    case Op::kAddExt:
      return set(i.rd, i.sf,
                 addsub(rdsp(i.rn),
                        T + "(lift::Extend(" + X(i.rm) + ", " + N(i.shift) + ", " + N(i.amount) + "))",
                        ""),
                 !i.setf);
    case Op::kAdc:
      return set(i.rd, i.sf, addsub(rd(i.rn), rd(i.rm), "f.c"));
    case Op::kLogImm:
      return logic(rd(i.rn), T + "(" + Hex(uint64_t(i.imm) & mask) + ")", true);
    case Op::kLogShift: {
      std::string b = shifted(rd(i.rm));
      if (i.inv) b = T + "(~" + b + ")";
      return logic(rd(i.rn), b, false);
    }
    case Op::kMovWide: {
      uint64_t v = uint64_t(i.imm) << i.amount;
      if (i.opc == 3) {
        uint64_t keep = ~(0xFFFFull << i.amount) & mask;
        return set(i.rd, i.sf, T + "((" + rd(i.rd) + " & " + Hex(keep) + ") | " + Hex(v) + ")");
      }
      if (i.opc == 0) v = ~v & mask;
      return set(i.rd, i.sf, Hex(v));
    }
    case Op::kBitfield: {
      std::string args = ", " + N(i.immr) + ", " + N(i.imms) + ")";
      if (i.opc == 1) return set(i.rd, i.sf, "lift::Bfm<" + T + ">(" + rd(i.rd) + ", " + rd(i.rn) + args);
      return set(i.rd, i.sf, std::string(i.opc ? "lift::Ubfm<" : "lift::Sbfm<") + T + ">(" + rd(i.rn) + args);
    }
    case Op::kExtr:
      if (!i.imm) return set(i.rd, i.sf, rd(i.rm));
      return set(i.rd, i.sf, T + "((" + rd(i.rm) + " >> " + N(int(i.imm)) + ") | (" + rd(i.rn) +
                                 " << " + N(bits - int(i.imm)) + "))");
    case Op::kCondCmp: {
      std::string b = i.use_imm ? T + "(" + N(int(i.imm)) + ")" : rd(i.rm);
      std::string cmp = "lift::Adc<" + T + ">(&f, " + rd(i.rn) + ", " +
                        (i.sub ? T + "(~" + b + "), true" : b + ", false") + ")";
      return std::string("if (") + kCond[i.cond] + ") " + cmp + "; else f = lift::Unpack(" +
             Hex(uint64_t(i.nzcv) << 28) + ");";
    }
    case Op::kCondSel: {
      std::string m = rd(i.rm);
      std::string alt[4] = {m, T + "(" + m + " + 1)", T + "(~" + m + ")", T + "(0 - " + m + ")"};
      return set(i.rd, i.sf, T + "(" + kCond[i.cond] + " ? " + rd(i.rn) + " : " + alt[i.opc] + ")");
    }
    case Op::kDp1: {
      std::string n = rd(i.rn);
      std::string v[6] = {"lift::Rbit<" + T + ">(" + n + ")", "lift::Rev<" + T + ">(" + n + ", 2)",
                          "lift::Rev<uint64_t>(" + n + ", 4)",
                          "lift::Rev<" + T + ">(" + n + ", " + N(bits / 8) + ")",
                          "lift::Clz<" + T + ">(" + n + ")", "lift::Cls<" + T + ">(" + n + ")"};
      return set(i.rd, i.sf, v[i.opc]);
    }
    case Op::kDp2: {
      std::string a = rd(i.rn), b = rd(i.rm);
      std::string amt = "unsigned(" + b + " & " + N(bits - 1) + ")";
      std::string v[6] = {"lift::Udiv<" + T + ">(" + a + ", " + b + ")",
                          "lift::Sdiv<" + T + ">(" + a + ", " + b + ")",
                          "lift::Lsl<" + T + ">(" + a + ", " + amt + ")",
                          "lift::Lsr<" + T + ">(" + a + ", " + amt + ")",
                          "lift::Asr<" + T + ">(" + a + ", " + amt + ")",
                          "lift::Ror<" + T + ">(" + a + ", " + amt + ")"};
      return set(i.rd, i.sf, v[i.opc]);
    }
    case Op::kDp3: {
      std::string sx = "int64_t(int32_t(" + W(i.rn) + ")) * int32_t(" + W(i.rm) + ")";
      std::string ux = "uint64_t(" + W(i.rn) + ") * " + W(i.rm);
      std::string v;
      switch (i.opc) {
        case 0: v = T + "(" + rd(i.ra) + " + " + rd(i.rn) + " * " + rd(i.rm) + ")"; break;
        case 1: v = T + "(" + rd(i.ra) + " - " + rd(i.rn) + " * " + rd(i.rm) + ")"; break;
        case 2: v = X(i.ra) + " + uint64_t(" + sx + ")"; break;
        case 3: v = X(i.ra) + " - uint64_t(" + sx + ")"; break;
        case 4: v = "lift::Smulh(" + X(i.rn) + ", " + X(i.rm) + ")"; break;
        case 5: v = X(i.ra) + " + " + ux; break;
        case 6: v = X(i.ra) + " - " + ux; break;
        default: v = "lift::Umulh(" + X(i.rn) + ", " + X(i.rm) + ")"; break;
      }
      return set(i.rd, i.sf, v);
    }
    case Op::kB:
      return Branch(i.va, uint64_t(i.imm));
    case Op::kBCond:
      return std::string("if (") + kCond[i.cond] + ") " + Branch(i.va, uint64_t(i.imm));
    case Op::kCbz:
      return "if (" + rd(i.rd) + (i.opc ? " != 0) " : " == 0) ") + Branch(i.va, uint64_t(i.imm));
    case Op::kTbz:
      return "if (((" + X(i.rd) + " >> " + N(i.amount) + ") & 1) " + (i.opc ? "!= 0) " : "== 0) ") +
             Branch(i.va, uint64_t(i.imm));
    case Op::kBl: {
      uint64_t t = uint64_t(i.imm);
      if (!p_.functions().count(t)) return Spill() + "BadJump(c, " + Hex(i.va) + ", " + Hex(t) + ");";
      callees_.insert(t);
      return "x30 = " + Hex(i.va + 4) + "; " + Spill() + FunctionName(t) + "(c); " + Reload();
    }
    case Op::kBr: {
      std::string s = "uint64_t t = " + X(i.rn) + "; ";
      if (f_ && f_->tables.count(i.va)) {
        s += "switch (t) {";
        for (uint64_t t : f_->tables.at(i.va)) s += " case " + Hex(t) + ": " + Branch(i.va, t);
        return s + " default: " + Spill() + "BadJump(c, " + Hex(i.va) + ", t); }";
      }
      return s + Spill() + "Dispatch(c, t, " + Hex(i.va) + "); return;";
    }
    case Op::kBlr:
      return "uint64_t t = " + X(i.rn) + "; x30 = " + Hex(i.va + 4) + "; " + Spill() +
             "Dispatch(c, t, " + Hex(i.va) + "); " + Reload();
    case Op::kRet:
      return Spill() + "return;";
    case Op::kSvc:
      return Spill() + "Svc(c, " + Hex(uint64_t(i.imm)) + ", " + Hex(i.va) + "); " + Reload();
    case Op::kBrk:
      return Spill() + "Brk(c, " + Hex(i.va) + ", " + N(int(i.imm)) + ");";
    case Op::kHint:
      return "";
    case Op::kBarrier:
      return i.opc == 6 ? "" : "lift::Fence();";
    case Op::kClrex:
      return "lift::Clrex(c);";
    case Op::kMrs:
      return set(i.rd, true, i.sysreg == a64::kNzcv ? "lift::Pack(f)" : "c.tpidr_el0");
    case Op::kMsr:
      return i.sysreg == a64::kNzcv ? "f = lift::Unpack(" + X(i.rd) + ");"
                                    : "c.tpidr_el0 = " + X(i.rd) + ";";
    case Op::kLdSt: {
      if (i.prefetch) return "";
      std::string s = "uint64_t a = " + address() + "; ";
      if (i.load) s += set(i.rd, i.sf, loaded(std::string("lift::Ld<") + U + ">(a)"));
      else s += std::string("lift::St<") + U + ">(a, " + U + "(" + X(i.rd) + "));";
      return s + writeback();
    }
    case Op::kLdStPair: {
      std::string s = "uint64_t a = " + address() + "; ";
      std::string second = "a + " + N(i.size);
      if (i.load) {
        s += std::string(U) + " v0 = lift::Ld<" + U + ">(a), v1 = lift::Ld<" + U + ">(" + second + "); ";
        s += set(i.rd, i.sf, loaded("v0")) + " " + set(i.ra, i.sf, loaded("v1"));
      } else {
        s += std::string("lift::St<") + U + ">(a, " + U + "(" + X(i.rd) + ")); lift::St<" + U +
             ">(" + second + ", " + U + "(" + X(i.ra) + "));";
      }
      return s + writeback();
    }
    case Op::kLdLit:
      if (i.prefetch) return "";
      return set(i.rd, i.sf, loaded(std::string("lift::Ld<") + U + ">(" + Hex(uint64_t(i.imm)) + ")"));
    case Op::kLdStExcl:
      if (i.load)
        return set(i.rd, i.sf, std::string("lift::Ldxr<") + U + ">(c, " + XSp(i.rn) + ", " +
                                   (i.acquire ? "true" : "false") + ")");
      return set(i.rm, false, std::string("lift::Stxr<") + U + ">(c, " + XSp(i.rn) + ", " + U +
                                  "(" + X(i.rd) + "))");
    case Op::kLdStOrder:
      if (i.load) return set(i.rd, i.sf, std::string("lift::LdAcq<") + U + ">(" + XSp(i.rn) + ")");
      return std::string("lift::StRel<") + U + ">(" + XSp(i.rn) + ", " + U + "(" + X(i.rd) + "));";
  }
  return "";
}

std::string Comment(const Insn& i) {
  char b[64];
  std::snprintf(b, sizeof b, "  // %llX: %08X %s\n", static_cast<unsigned long long>(i.va), i.raw,
                a64::Name(i).c_str());
  return b;
}

std::string Declarations(const std::set<uint64_t>& fns) {
  std::string s;
  for (uint64_t t : fns) s += "void " + FunctionName(t) + "(Ctx& c);\n";
  return s;
}

}  // namespace

std::string FunctionName(uint64_t va) { return "f_" + HexVa(va); }

std::string EmitFunction(const Program& p, const Function& f) {
  Emitter e(p, &f);
  for (const auto& [va, i] : f.insns) e.Use(i);
  // Labels: branch targets, and fall-through targets that do not follow
  // in address order.
  std::set<uint64_t> labels = f.labels;
  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    auto next = std::next(it);
    uint64_t ft = it->first + 4;
    if (!a64::IsTerminator(it->second) && f.insns.count(ft) &&
        (next == f.insns.end() || next->first != ft))
      labels.insert(ft);
  }
  std::string body;
  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    const Insn& i = it->second;
    if (labels.count(i.va)) body += Label(i.va) + ":\n";
    body += Comment(i);
    std::string b = e.Body(i);
    body += b.empty() ? "" : "  { " + b + " }\n";
    auto next = std::next(it);
    if (!a64::IsTerminator(i) && (next == f.insns.end() || next->first != i.va + 4))
      body += "  " + e.Branch(i.va, i.va + 4) + "\n";
  }
  std::string s;
  if (const std::string* name = p.NameOf(f.va)) s += "// " + *name + "\n";
  s += "void " + FunctionName(f.va) + "(Ctx& c) {\n" + e.Prologue() + body + "}\n\n";
  return Declarations(e.callees()) + s;
}

std::string EmitForm(const Program& p, const Insn& i, const std::string& name) {
  Emitter e(p, nullptr);
  e.UseAll();
  return "void " + name + "(Ctx& c) {\n" + e.Prologue() + Comment(i) + "  { " + e.Body(i) +
         " }\n  " + e.Spill() + "\n}\n";
}

}  // namespace lifter::a64
