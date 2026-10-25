#include "emit.h"

#include <cstdio>
#include <set>

#include "ppc/lifted.h"

namespace lifter::ppc {
namespace {

using ppc::Insn;
using ppc::Op;

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
std::string R(int r) { return "r" + N(r); }
// RA|0: register 0 reads as 0 in address and ADDI/ADDIS operands.
std::string R0(int r) { return r ? R(r) : "uint64_t(0)"; }

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

// What an instruction reads or writes of the cached state.
struct Uses {
  bool r[32] = {};
  bool cr = false, xer = false, lr = false, ctr = false, mem = false;
};

bool IsCall(const Insn& i) {
  switch (i.op) {
    case Op::kB: case Op::kBc: case Op::kBclr: case Op::kBcctr: return i.lk;
    default: return false;
  }
}

void Collect(const Insn& i, Uses& u) {
  u.r[i.rd] = u.r[i.ra] = u.r[i.rb] = true;
  switch (i.op) {
    case Op::kLmw: case Op::kStmw:
      for (int r = i.rd; r < 32; r++) u.r[r] = true;
      break;
    case Op::kAddic: case Op::kSubfic: case Op::kAddc: case Op::kAdde: case Op::kAddme:
    case Op::kAddze: case Op::kSubfc: case Op::kSubfe: case Op::kSubfme: case Op::kSubfze:
    case Op::kSraw: case Op::kSrawi: case Op::kSrad: case Op::kSradi: case Op::kMcrxr:
    case Op::kMfspr: case Op::kMtspr:
      u.xer = true;
      break;
    default:
      break;
  }
  if (i.rc || i.oe) u.cr = u.xer = true;
  switch (i.op) {
    case Op::kCmp: case Op::kCmpl: case Op::kCmpi: case Op::kCmpli: case Op::kStcx:
      u.cr = u.xer = true;
      break;
    case Op::kCrLogic: case Op::kMcrf: case Op::kMcrxr: case Op::kMfcr: case Op::kMtcrf:
    case Op::kBc: case Op::kBclr: case Op::kBcctr:
      u.cr = true;
      break;
    default:
      break;
  }
  if (i.op == Op::kBc || i.op == Op::kBclr || i.op == Op::kBcctr || i.op == Op::kMfspr ||
      i.op == Op::kMtspr)
    u.lr = u.ctr = true;
  if (i.lk) u.lr = true;
  if (i.op == Op::kLoad || i.op == Op::kStore || i.op == Op::kLmw || i.op == Op::kStmw ||
      i.op == Op::kDcbz)
    u.mem = true;
}

class Emitter {
 public:
  Emitter(const Program& p, const Function* f) : p_(p), f_(f) {}

  void UseAll() {
    for (bool& r : u_.r) r = true;
    u_.cr = u_.xer = u_.lr = u_.ctr = u_.mem = true;
  }
  void Use(const Insn& i) {
    Collect(i, u_);
    // A call can change any register; the code after it reads them back.
    if (IsCall(i) || i.op == Op::kSc) UseAll();
  }
  std::string Prologue() const {
    std::string s;
    for (int r = 0; r < 32; r++)
      if (u_.r[r]) s += "  uint64_t r" + N(r) + " = c.r[" + N(r) + "];\n";
    if (u_.cr) s += "  uint32_t cr = c.cr;\n";
    if (u_.xer) s += "  uint32_t xer = c.xer;\n";
    if (u_.lr) s += "  uint64_t lr = c.lr;\n";
    if (u_.ctr) s += "  uint64_t ctr = c.ctr;\n";
    if (u_.mem) s += "  uint8_t* const m = c.mem;\n";
    return s;
  }
  std::string Spill() const {
    std::string s;
    for (int r = 0; r < 32; r++)
      if (u_.r[r]) s += "c.r[" + N(r) + "] = r" + N(r) + "; ";
    if (u_.cr) s += "c.cr = cr; ";
    if (u_.xer) s += "c.xer = xer; ";
    if (u_.lr) s += "c.lr = lr; ";
    if (u_.ctr) s += "c.ctr = ctr; ";
    return s;
  }
  std::string Reload() const {
    std::string s;
    for (int r = 0; r < 32; r++)
      if (u_.r[r]) s += "r" + N(r) + " = c.r[" + N(r) + "]; ";
    if (u_.cr) s += "cr = c.cr; ";
    if (u_.xer) s += "xer = c.xer; ";
    if (u_.lr) s += "lr = c.lr; ";
    if (u_.ctr) s += "ctr = c.ctr; ";
    return s;
  }

  // Control transfer to `t` from the instruction at `va`: a label in this
  // function, or a tail call.
  std::string Branch(uint64_t va, uint64_t t) {
    if (f_ && f_->insns.count(t)) return "goto " + Label(t) + ";";
    if (p_.functions().count(t)) {
      callees_.insert(t);
      return "{ " + Spill() + FunctionName(t) + "(c); return; }";
    }
    return "{ " + Spill() + "BadJump(c, " + Hex(va) + ", " + Hex(t) + "); }";
  }
  // A call to `t` returning to the next instruction (LR is already set).
  std::string Call(uint64_t va, uint64_t t) {
    if (!p_.functions().count(t)) return Spill() + "BadJump(c, " + Hex(va) + ", " + Hex(t) + ");";
    callees_.insert(t);
    return Spill() + FunctionName(t) + "(c); " + Reload();
  }

  std::string Body(const Insn& i);
  const std::set<uint64_t>& callees() const { return callees_; }

 private:
  const Program& p_;
  const Function* f_;
  Uses u_;
  std::set<uint64_t> callees_;
};

// BO/BI: the condition a conditional branch tests, after decrementing CTR
// when BO says so (`dec` receives that statement).
std::string BranchCondition(const Insn& i, std::string& dec) {
  std::string cond;
  if (!(i.bo & 4)) {
    // 32-bit mode: the decrement is 64-bit, the test on the low word.
    dec = "ctr = ctr - 1; ";
    cond = i.bo & 2 ? "uint32_t(ctr) == 0" : "uint32_t(ctr) != 0";
  }
  if (!(i.bo & 0x10)) {
    std::string bit = "lift::CrBit(cr, " + N(i.bi) + ")";
    std::string c = i.bo & 8 ? bit : "!" + bit;
    cond = cond.empty() ? c : cond + " && " + c;
  }
  return cond.empty() ? "true" : cond;
}

std::string Emitter::Body(const Insn& i) {
  auto set = [&](int r, const std::string& v) { return R(r) + " = uint64_t(" + v + ");"; };
  // Record forms: CR0 from the result just written to `r`.
  auto rec = [&](int r) { return i.rc ? " cr = lift::Record(cr, " + R(r) + ", xer);" : std::string(); };
  // Effective addresses: 32-bit mode keeps the low word (update forms
  // write it back zero-extended).
  auto ea_d = [&]() { return "uint64_t ea = " + R0(i.ra) + " + " + Hex(uint64_t(i.imm)) + "; "; };
  auto ea_x = [&]() { return "uint64_t ea = " + R0(i.ra) + " + " + R(i.rb) + "; "; };
  // Add-family: lift::Add(a, b, carry) into RT, with CA, OV and CR0 as
  // the form asks.
  auto add = [&](const std::string& a, const std::string& b, const std::string& carry, bool sets_ca) {
    std::string s = "{ lift::AddResult s = lift::Add(" + a + ", " + b + ", " + carry + "); " +
                    set(i.rd, "s.r");
    if (sets_ca) s += " xer = lift::SetCa(xer, s.ca);";
    if (i.oe) s += " xer = lift::SetOv(xer, s.ov);";
    return s + rec(i.rd) + " }";
  };
  // Multiply and divide helpers that report OV through a pointer.
  auto ovop = [&](const char* fn) {
    if (!i.oe) return set(i.rd, std::string("lift::") + fn + "(" + R(i.ra) + ", " + R(i.rb) + ", nullptr)") + rec(i.rd);
    return "{ bool ov; " + set(i.rd, std::string("lift::") + fn + "(" + R(i.ra) + ", " + R(i.rb) + ", &ov)") +
           " xer = lift::SetOv(xer, ov);" + rec(i.rd) + " }";
  };
  auto logic = [&](const std::string& v) { return set(i.ra, v) + rec(i.ra); };
  const char* U = UType(i.size);

  switch (i.op) {
    case Op::kInvalid: {
      char b[16];
      std::snprintf(b, sizeof b, "%08X", i.raw);
      return Spill() + "Unimplemented(c, " + Hex(i.va) + ", \"instruction " + b + "\");";
    }
    case Op::kAddi:
      return set(i.rd, R0(i.ra) + " + " + Hex(uint64_t(i.imm)));
    case Op::kAddic:
      return add(R(i.ra), Hex(uint64_t(i.imm)), "false", true);
    case Op::kSubfic:
      return add("~" + R(i.ra), Hex(uint64_t(i.imm)), "true", true);
    case Op::kMulli:
      return set(i.rd, R(i.ra) + " * " + Hex(uint64_t(i.imm)));
    case Op::kAdd: return add(R(i.ra), R(i.rb), "false", false);
    case Op::kAddc: return add(R(i.ra), R(i.rb), "false", true);
    case Op::kAdde: return add(R(i.ra), R(i.rb), "lift::Ca(xer)", true);
    case Op::kAddme: return add(R(i.ra), "~uint64_t(0)", "lift::Ca(xer)", true);
    case Op::kAddze: return add(R(i.ra), "uint64_t(0)", "lift::Ca(xer)", true);
    case Op::kSubf: return add("~" + R(i.ra), R(i.rb), "true", false);
    case Op::kSubfc: return add("~" + R(i.ra), R(i.rb), "true", true);
    case Op::kSubfe: return add("~" + R(i.ra), R(i.rb), "lift::Ca(xer)", true);
    case Op::kSubfme: return add("~" + R(i.ra), "~uint64_t(0)", "lift::Ca(xer)", true);
    case Op::kSubfze: return add("~" + R(i.ra), "uint64_t(0)", "lift::Ca(xer)", true);
    case Op::kNeg: return add("~" + R(i.ra), "uint64_t(0)", "true", false);
    case Op::kMullw: return ovop("Mullw");
    case Op::kMulld: return ovop("Mulld");
    case Op::kDivw: return ovop("Divw");
    case Op::kDivwu: return ovop("Divwu");
    case Op::kDivd: return ovop("Divd");
    case Op::kDivdu: return ovop("Divdu");
    case Op::kMulhw: return set(i.rd, "lift::Mulhw(" + R(i.ra) + ", " + R(i.rb) + ")") + rec(i.rd);
    case Op::kMulhwu: return set(i.rd, "lift::Mulhwu(" + R(i.ra) + ", " + R(i.rb) + ")") + rec(i.rd);
    case Op::kMulhd: return set(i.rd, "lift::Mulhd(" + R(i.ra) + ", " + R(i.rb) + ")") + rec(i.rd);
    case Op::kMulhdu: return set(i.rd, "lift::Mulhdu(" + R(i.ra) + ", " + R(i.rb) + ")") + rec(i.rd);

    case Op::kAndi: return logic(R(i.rd) + " & " + Hex(uint64_t(i.imm)));
    case Op::kOri: return set(i.ra, R(i.rd) + " | " + Hex(uint64_t(i.imm)));
    case Op::kXori: return set(i.ra, R(i.rd) + " ^ " + Hex(uint64_t(i.imm)));
    case Op::kAnd: return logic(R(i.rd) + " & " + R(i.rb));
    case Op::kAndc: return logic(R(i.rd) + " & ~" + R(i.rb));
    case Op::kOr: return logic(R(i.rd) + " | " + R(i.rb));
    case Op::kOrc: return logic(R(i.rd) + " | ~" + R(i.rb));
    case Op::kXor: return logic(R(i.rd) + " ^ " + R(i.rb));
    case Op::kNand: return logic("~(" + R(i.rd) + " & " + R(i.rb) + ")");
    case Op::kNor: return logic("~(" + R(i.rd) + " | " + R(i.rb) + ")");
    case Op::kEqv: return logic("~(" + R(i.rd) + " ^ " + R(i.rb) + ")");
    case Op::kExtsb: return logic("int64_t(int8_t(" + R(i.rd) + "))");
    case Op::kExtsh: return logic("int64_t(int16_t(" + R(i.rd) + "))");
    case Op::kExtsw: return logic("int64_t(int32_t(" + R(i.rd) + "))");
    case Op::kCntlzw: return logic("lift::Cntlz32(" + R(i.rd) + ")");
    case Op::kCntlzd: return logic("lift::Cntlz64(" + R(i.rd) + ")");

    case Op::kRlwinm:
      return logic("lift::Rotl32(" + R(i.rd) + ", " + N(i.sh) + ") & " +
                   Hex(recomp::ppc::lift::Mask(i.mb + 32, i.me + 32)));
    case Op::kRlwnm:
      return logic("lift::Rotl32(" + R(i.rd) + ", unsigned(" + R(i.rb) + ")) & " +
                   Hex(recomp::ppc::lift::Mask(i.mb + 32, i.me + 32)));
    case Op::kRlwimi: {
      uint64_t mk = recomp::ppc::lift::Mask(i.mb + 32, i.me + 32);
      return logic("(lift::Rotl32(" + R(i.rd) + ", " + N(i.sh) + ") & " + Hex(mk) + ") | (" +
                   R(i.ra) + " & " + Hex(~mk) + ")");
    }
    case Op::kRldicl:
      return logic("lift::Rotl64(" + R(i.rd) + ", " + N(i.sh) + ") & " +
                   Hex(recomp::ppc::lift::Mask(i.mb, 63)));
    case Op::kRldicr:
      return logic("lift::Rotl64(" + R(i.rd) + ", " + N(i.sh) + ") & " +
                   Hex(recomp::ppc::lift::Mask(0, i.mb)));
    case Op::kRldic:
      return logic("lift::Rotl64(" + R(i.rd) + ", " + N(i.sh) + ") & " +
                   Hex(recomp::ppc::lift::Mask(i.mb, 63 - i.sh)));
    case Op::kRldimi: {
      uint64_t mk = recomp::ppc::lift::Mask(i.mb, 63 - i.sh);
      return logic("(lift::Rotl64(" + R(i.rd) + ", " + N(i.sh) + ") & " + Hex(mk) + ") | (" +
                   R(i.ra) + " & " + Hex(~mk) + ")");
    }
    case Op::kRldcl:
      return logic("lift::Rotl64(" + R(i.rd) + ", unsigned(" + R(i.rb) + ")) & " +
                   Hex(recomp::ppc::lift::Mask(i.mb, 63)));
    case Op::kRldcr:
      return logic("lift::Rotl64(" + R(i.rd) + ", unsigned(" + R(i.rb) + ")) & " +
                   Hex(recomp::ppc::lift::Mask(0, i.mb)));
    case Op::kSlw: return logic("lift::Slw(" + R(i.rd) + ", " + R(i.rb) + ")");
    case Op::kSrw: return logic("lift::Srw(" + R(i.rd) + ", " + R(i.rb) + ")");
    case Op::kSld: return logic("lift::Sld(" + R(i.rd) + ", " + R(i.rb) + ")");
    case Op::kSrd: return logic("lift::Srd(" + R(i.rd) + ", " + R(i.rb) + ")");
    case Op::kSraw: case Op::kSrawi: case Op::kSrad: case Op::kSradi: {
      bool w = i.op == Op::kSraw || i.op == Op::kSrawi;
      bool imm = i.op == Op::kSrawi || i.op == Op::kSradi;
      std::string n = imm ? "uint64_t(" + N(i.sh) + ")" : R(i.rb);
      return std::string("{ bool ca; ") +
             set(i.ra, std::string(w ? "lift::Sraw(" : "lift::Srad(") + R(i.rd) + ", " + n + ", &ca)") +
             " xer = lift::SetCa(xer, ca);" + rec(i.ra) + " }";
    }

    case Op::kCmp: case Op::kCmpl: case Op::kCmpi: case Op::kCmpli: {
      bool sgn = i.op == Op::kCmp || i.op == Op::kCmpi;
      bool imm = i.op == Op::kCmpi || i.op == Op::kCmpli;
      const char* T = i.l ? (sgn ? "int64_t" : "uint64_t") : (sgn ? "int32_t" : "uint32_t");
      std::string b = imm ? Hex(uint64_t(i.imm)) : R(i.rb);
      return "cr = lift::SetField(cr, " + N(i.crfd) + ", lift::Compare<" + T + ">(" + T + "(" +
             R(i.ra) + "), " + T + "(" + b + "), xer));";
    }
    case Op::kCrLogic: {
      std::string a = "lift::CrBit(cr, " + N(i.bi) + ")", b = "lift::CrBit(cr, " + N(i.rb) + ")";
      std::string v;
      switch (i.opc) {
        case 257: v = a + " && " + b; break;            // crand
        case 449: v = a + " || " + b; break;            // cror
        case 193: v = a + " != " + b; break;            // crxor
        case 225: v = "!(" + a + " && " + b + ")"; break;  // crnand
        case 33: v = "!(" + a + " || " + b + ")"; break;   // crnor
        case 289: v = a + " == " + b; break;            // creqv
        case 129: v = a + " && !" + b; break;           // crandc
        default: v = a + " || !" + b; break;            // crorc (417)
      }
      return "cr = lift::SetCrBit(cr, " + N(i.bo) + ", " + v + ");";
    }
    case Op::kMcrf:
      return "cr = lift::SetField(cr, " + N(i.crfd) + ", lift::Field(cr, " + N(i.crfs) + "));";
    case Op::kMcrxr:
      return "cr = lift::SetField(cr, " + N(i.crfd) + ", xer >> 28); xer &= 0x0FFFFFFFu;";
    case Op::kMfcr:
      return set(i.rd, "cr");
    case Op::kMtcrf: {
      uint32_t mk = 0;
      for (int k = 0; k < 8; k++)
        if (i.crm & (0x80 >> k)) mk |= 0xFu << (28 - 4 * k);
      char b[64];
      std::snprintf(b, sizeof b, "0x%08Xu", mk);
      return std::string("cr = (cr & ~") + b + ") | (uint32_t(" + R(i.rd) + ") & " + b + ");";
    }

    case Op::kB:
      if (i.lk) return "lr = " + Hex(i.va + 4) + "; " + Call(i.va, uint64_t(i.imm));
      return Branch(i.va, uint64_t(i.imm));
    case Op::kBc: {
      std::string dec, cond = BranchCondition(i, dec);
      if (i.lk) return dec + "lr = " + Hex(i.va + 4) + "; if (" + cond + ") { " + Call(i.va, uint64_t(i.imm)) + "}";
      return dec + "if (" + cond + ") " + Branch(i.va, uint64_t(i.imm));
    }
    case Op::kBclr: {
      std::string dec, cond = BranchCondition(i, dec);
      if (!i.lk) return dec + "if (" + cond + ") { " + Spill() + "return; }";
      return dec + "{ uint64_t t = uint32_t(lr) & ~3u; lr = " + Hex(i.va + 4) + "; if (" + cond + ") { " +
             Spill() + "Dispatch(c, t, " + Hex(i.va) + "); " + Reload() + "} }";
    }
    case Op::kBcctr: {
      std::string dec, cond = BranchCondition(i, dec);
      std::string s = dec + "if (" + cond + ") { uint64_t t = uint32_t(ctr) & ~3u; ";
      if (i.lk) return s + "lr = " + Hex(i.va + 4) + "; " + Spill() + "Dispatch(c, t, " + Hex(i.va) + "); " + Reload() + "}";
      if (f_ && f_->tables.count(i.va)) {
        s += "switch (t) {";
        for (uint64_t t : f_->tables.at(i.va)) s += " case " + Hex(t) + ": " + Branch(i.va, t);
        return s + " default: " + Spill() + "BadJump(c, " + Hex(i.va) + ", t); } }";
      }
      return s + Spill() + "Dispatch(c, t, " + Hex(i.va) + "); return; }";
    }

    case Op::kSc:
      return Spill() + "Sc(c, " + Hex(i.va) + "); " + Reload();
    case Op::kTw: case Op::kTwi: case Op::kTd: case Op::kTdi: {
      bool w = i.op == Op::kTw || i.op == Op::kTwi;
      bool imm = i.op == Op::kTwi || i.op == Op::kTdi;
      const char* T = w ? "int32_t" : "int64_t";
      std::string b = imm ? Hex(uint64_t(i.imm)) : R(i.rb);
      char raw[16];
      std::snprintf(raw, sizeof raw, "0x%08Xu", i.raw);
      return std::string("if (lift::TrapIf<") + T + ">(" + N(i.to) + ", " + T + "(" + R(i.ra) +
             "), " + T + "(" + b + "))) { " + Spill() + "Trap(c, " + raw + ", " + Hex(i.va) +
             "); " + Reload() + "}";
    }
    case Op::kMfspr:
      switch (i.spr) {
        case ppc::kXer: return set(i.rd, "xer");
        case ppc::kLr: return set(i.rd, "lr");
        case ppc::kCtr: return set(i.rd, "ctr");
        case ppc::kVrsave: return set(i.rd, "c.vrsave");
        default: return Spill() + "Unimplemented(c, " + Hex(i.va) + ", \"mfspr " + N(i.spr) + "\");";
      }
    case Op::kMtspr:
      switch (i.spr) {
        // Only SO, OV, CA and the byte count exist in XER.
        case ppc::kXer: return "xer = uint32_t(" + R(i.rd) + ") & 0xE000007Fu;";
        case ppc::kLr: return "lr = " + R(i.rd) + ";";
        case ppc::kCtr: return "ctr = " + R(i.rd) + ";";
        case ppc::kVrsave: return "c.vrsave = uint32_t(" + R(i.rd) + ");";
        default: return Spill() + "Unimplemented(c, " + Hex(i.va) + ", \"mtspr " + N(i.spr) + "\");";
      }
    case Op::kMftb:
      return set(i.rd, i.spr == ppc::kTbu ? "TimeBase(c) >> 32" : "TimeBase(c)");
    case Op::kMfmsr:
      return set(i.rd, "c.msr");
    case Op::kMtmsr: case Op::kMtmsrd:
      // L = 1 changes only EE and RI.
      if (i.l) return "c.msr = (c.msr & ~0x8002ull) | (" + R(i.rd) + " & 0x8002ull);";
      if (i.op == Op::kMtmsr) return "c.msr = (c.msr & ~0xFFFFFFFFull) | uint32_t(" + R(i.rd) + ");";
      return "c.msr = " + R(i.rd) + ";";
    case Op::kSync:
      return i.opc == 1 ? "lift::LwFence();" : i.opc == 3 ? "" : "lift::Fence();";
    case Op::kCacheHint:
      return "";
    case Op::kDcbz:
      return ea_x() + "lift::Dcbz(m, uint32_t(ea), " + N(i.size) + ");";

    case Op::kLoad: {
      std::string s = i.indexed ? ea_x() : ea_d();
      std::string v = std::string(i.byterev ? "lift::LdRev<" : "lift::Ld<") + U + ">(m, uint32_t(ea))";
      if (i.sign) v = std::string("int64_t(") + SType(i.size) + "(" + v + "))";
      s += set(i.rd, v);
      if (i.update) s += " " + R(i.ra) + " = uint32_t(ea);";
      return s;
    }
    case Op::kStore: {
      std::string s = i.indexed ? ea_x() : ea_d();
      s += std::string(i.byterev ? "lift::StRev<" : "lift::St<") + U + ">(m, uint32_t(ea), " + U +
           "(" + R(i.rd) + "));";
      if (i.update) s += " " + R(i.ra) + " = uint32_t(ea);";
      return s;
    }
    case Op::kLmw: case Op::kStmw: {
      std::string s = ea_d();
      for (int r = i.rd; r < 32; r++) {
        std::string a = "uint32_t(ea + " + N(4 * (r - i.rd)) + ")";
        s += i.op == Op::kLmw ? R(r) + " = lift::Ld<uint32_t>(m, " + a + "); "
                              : "lift::St<uint32_t>(m, " + a + ", uint32_t(" + R(r) + ")); ";
      }
      return s;
    }
    case Op::kLarx:
      return ea_x() + set(i.rd, std::string("lift::Larx<") + U + ">(c, uint32_t(ea))");
    case Op::kStcx:
      return ea_x() + "bool ok = lift::Stcx<" + U + ">(c, uint32_t(ea), " + U + "(" + R(i.rd) +
             ")); cr = lift::SetField(cr, 0, (ok ? lift::kEq : 0) | (xer >> 31));";
  }
  return "";
}

std::string Comment(const Insn& i) {
  char b[64];
  std::snprintf(b, sizeof b, "  // %llX: %08X %s\n", static_cast<unsigned long long>(i.va), i.raw,
                ppc::Name(i).c_str());
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
  std::string s;
  if (const std::string* name = p.NameOf(f.va)) s += "// " + *name + "\n";
  if (f.import) {
    return s + "void " + FunctionName(f.va) + "(Ctx& c) { Import(c, \"" + f.import->module +
           "\", " + N(int(f.import->ordinal)) + ", " + Hex(f.va) + "); }\n\n";
  }
  Emitter e(p, &f);
  for (const auto& [va, i] : f.insns) e.Use(i);
  // Labels: branch targets, and fall-through targets that do not follow
  // in address order.
  std::set<uint64_t> labels = f.labels;
  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    auto next = std::next(it);
    uint64_t ft = it->first + 4;
    if (!ppc::IsTerminator(it->second) && f.insns.count(ft) &&
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
    if (!ppc::IsTerminator(i) && (next == f.insns.end() || next->first != i.va + 4))
      body += "  " + e.Branch(i.va, i.va + 4) + "\n";
  }
  s += "void " + FunctionName(f.va) + "(Ctx& c) {\n" + e.Prologue() + body + "}\n\n";
  return Declarations(e.callees()) + s;
}

std::string EmitForm(const Program& p, const Insn& i, const std::string& name) {
  Emitter e(p, nullptr);
  e.UseAll();
  return "void " + name + "(Ctx& c) {\n" + e.Prologue() + Comment(i) + "  { " + e.Body(i) +
         " }\n  " + e.Spill() + "\n}\n";
}

}  // namespace lifter::ppc
