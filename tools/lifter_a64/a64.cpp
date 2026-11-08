#include "a64.h"

namespace lifter::a64 {
namespace {

uint32_t Bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((2u << (hi - lo)) - 1); }
bool Bit(uint32_t v, int b) { return (v >> b) & 1; }
int64_t Sext(uint64_t v, int bits) { return int64_t(v << (64 - bits)) >> (64 - bits); }

// Reads a 5-bit register field at bit `lo` and records it in reg_fields.
uint8_t Reg(Insn& i, int lo) {
  uint8_t r = uint8_t(Bits(i.raw, lo + 4, lo));
  if (r != 31) i.reg_fields |= 0x1Fu << lo;
  return r;
}

// DecodeBitMasks() with immediate = true (logical immediates).
bool LogicalImm(bool sf, uint32_t n, uint32_t imms, uint32_t immr, uint64_t& out) {
  uint32_t combined = (n << 6) | (~imms & 0x3F);
  if (!combined) return false;
  int len = 31 - __builtin_clz(combined);
  if (len < 1 || (!sf && len > 5)) return false;
  uint32_t levels = (1u << len) - 1;
  if ((imms & levels) == levels) return false;
  uint32_t s = imms & levels, r = immr & levels, esize = 1u << len;
  uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
  uint64_t welem = (1ull << (s + 1)) - 1;
  uint64_t elem = r ? ((welem >> r) | (welem << (esize - r))) & emask : welem;
  uint64_t v = 0;
  for (uint32_t b = 0; b < 64; b += esize) v |= elem << b;
  out = sf ? v : v & 0xFFFFFFFFull;
  return true;
}

bool DpImm(Insn& i) {
  uint32_t raw = i.raw;
  i.sf = Bit(raw, 31);
  switch (Bits(raw, 25, 23)) {
    case 0: case 1: {  // ADR, ADRP
      int64_t off = Sext((uint64_t(Bits(raw, 23, 5)) << 2) | Bits(raw, 30, 29), 21);
      i.op = Op::kAdr;
      i.imm = Bit(raw, 31) ? int64_t((i.va & ~0xFFFull) + (uint64_t(off) << 12))
                           : int64_t(i.va + uint64_t(off));
      i.sf = true;
      i.rd = Reg(i, 0);
      return true;
    }
    case 2:  // ADD/SUB (immediate)
      i.op = Op::kAddImm;
      i.sub = Bit(raw, 30);
      i.setf = Bit(raw, 29);
      i.imm = int64_t(Bits(raw, 21, 10)) << (Bit(raw, 22) ? 12 : 0);
      i.rn = Reg(i, 5);
      i.rd = Reg(i, 0);
      return true;
    case 4: {  // logical (immediate)
      uint64_t imm;
      if (!i.sf && Bit(raw, 22)) return false;
      if (!LogicalImm(i.sf, Bit(raw, 22), Bits(raw, 15, 10), Bits(raw, 21, 16), imm)) return false;
      i.op = Op::kLogImm;
      i.opc = uint8_t(Bits(raw, 30, 29));
      i.setf = i.opc == 3;
      i.imm = int64_t(imm);
      i.rn = Reg(i, 5);
      i.rd = Reg(i, 0);
      return true;
    }
    case 5: {  // move wide
      uint32_t opc = Bits(raw, 30, 29), hw = Bits(raw, 22, 21);
      if (opc == 1 || (!i.sf && hw >= 2)) return false;
      i.op = Op::kMovWide;
      i.opc = uint8_t(opc);
      i.imm = Bits(raw, 20, 5);
      i.amount = uint8_t(hw * 16);
      i.rd = Reg(i, 0);
      return true;
    }
    case 6: {  // bitfield
      uint32_t opc = Bits(raw, 30, 29);
      i.immr = uint8_t(Bits(raw, 21, 16));
      i.imms = uint8_t(Bits(raw, 15, 10));
      if (opc == 3 || Bit(raw, 22) != i.sf) return false;
      if (!i.sf && (i.immr >= 32 || i.imms >= 32)) return false;
      i.op = Op::kBitfield;
      i.opc = uint8_t(opc);
      i.rn = Reg(i, 5);
      i.rd = Reg(i, 0);
      return true;
    }
    case 7:  // EXTR
      if (Bits(raw, 30, 29) || Bit(raw, 22) != i.sf || Bit(raw, 21)) return false;
      if (!i.sf && Bit(raw, 15)) return false;
      i.op = Op::kExtr;
      i.imm = Bits(raw, 15, 10);
      i.rm = Reg(i, 16);
      i.rn = Reg(i, 5);
      i.rd = Reg(i, 0);
      return true;
    default:
      return false;
  }
}

bool BranchSys(Insn& i) {
  uint32_t raw = i.raw;
  if (Bits(raw, 30, 26) == 0b00101) {  // B, BL
    i.op = Bit(raw, 31) ? Op::kBl : Op::kB;
    i.imm = int64_t(i.va + uint64_t(Sext(Bits(raw, 25, 0), 26) * 4));
    return true;
  }
  if (Bits(raw, 30, 25) == 0b011010) {  // CBZ, CBNZ
    i.op = Op::kCbz;
    i.sf = Bit(raw, 31);
    i.opc = Bit(raw, 24);
    i.imm = int64_t(i.va + uint64_t(Sext(Bits(raw, 23, 5), 19) * 4));
    i.rd = Reg(i, 0);
    return true;
  }
  if (Bits(raw, 30, 25) == 0b011011) {  // TBZ, TBNZ
    i.op = Op::kTbz;
    i.sf = Bit(raw, 31);
    i.opc = Bit(raw, 24);
    i.amount = uint8_t((Bit(raw, 31) << 5) | Bits(raw, 23, 19));
    i.imm = int64_t(i.va + uint64_t(Sext(Bits(raw, 18, 5), 14) * 4));
    i.rd = Reg(i, 0);
    return true;
  }
  if (Bits(raw, 31, 24) == 0b01010100) {  // B.cond
    if (Bit(raw, 4)) return false;
    i.op = Op::kBCond;
    i.cond = uint8_t(Bits(raw, 3, 0));
    i.imm = int64_t(i.va + uint64_t(Sext(Bits(raw, 23, 5), 19) * 4));
    return true;
  }
  if (Bits(raw, 31, 24) == 0b11010100) {  // exception generation
    uint32_t opc = Bits(raw, 23, 21), op2 = Bits(raw, 4, 2), ll = Bits(raw, 1, 0);
    i.imm = Bits(raw, 20, 5);
    if (opc == 0 && op2 == 0 && ll == 1) i.op = Op::kSvc;
    else if (opc == 1 && op2 == 0 && ll == 0) i.op = Op::kBrk;
    else return false;
    return true;
  }
  if (Bits(raw, 31, 22) == 0b1101010100) {  // system
    bool l = Bit(raw, 21);
    uint32_t op0 = Bits(raw, 20, 19), op1 = Bits(raw, 18, 16), crn = Bits(raw, 15, 12),
             crm = Bits(raw, 11, 8), op2 = Bits(raw, 7, 5), rt = Bits(raw, 4, 0);
    if (op0 == 0 && !l && op1 == 3 && rt == 31 && crn == 2) {
      i.op = Op::kHint;  // unallocated hints execute as NOP
      return true;
    }
    if (op0 == 0 && !l && op1 == 3 && rt == 31 && crn == 3) {
      if (op2 == 2) i.op = Op::kClrex;
      else if (op2 >= 4 && op2 <= 6) i.op = Op::kBarrier;
      else return false;
      i.opc = uint8_t(op2);
      i.imm = crm;
      return true;
    }
    if (op0 >= 2) {
      i.sysreg = uint16_t((op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2);
      if (i.sysreg != kNzcv && i.sysreg != kTpidrEl0) return false;
      i.op = l ? Op::kMrs : Op::kMsr;
      i.sf = true;
      i.rd = Reg(i, 0);
      return true;
    }
    return false;
  }
  if (Bits(raw, 31, 25) == 0b1101011) {  // unconditional branch (register)
    if (Bits(raw, 20, 16) != 31 || Bits(raw, 15, 10) || Bits(raw, 4, 0)) return false;
    switch (Bits(raw, 24, 21)) {
      case 0: i.op = Op::kBr; break;
      case 1: i.op = Op::kBlr; break;
      case 2: i.op = Op::kRet; break;
      default: return false;
    }
    i.rn = Reg(i, 5);
    return true;
  }
  return false;
}

bool LdSt(Insn& i) {
  uint32_t raw = i.raw;
  if (Bit(raw, 26)) return false;  // SIMD&FP registers
  if (Bits(raw, 29, 24) == 0b001000) {  // exclusive and ordered
    uint32_t size = Bits(raw, 31, 30), rs = Bits(raw, 20, 16);
    bool o2 = Bit(raw, 23), l = Bit(raw, 22), o1 = Bit(raw, 21), o0 = Bit(raw, 15);
    if (o1 || Bits(raw, 14, 10) != 31) return false;  // pairs, CAS
    if (!o2) {
      if (l && rs != 31) return false;
      i.op = Op::kLdStExcl;
      i.acquire = l && o0;
      i.release = !l && o0;
      if (!l) i.rm = Reg(i, 16);
    } else {
      if (!o0 || rs != 31) return false;  // LDLAR/STLLR
      i.op = Op::kLdStOrder;
      i.acquire = l;
      i.release = !l;
    }
    i.load = l;
    i.size = uint8_t(1u << size);
    i.sf = size == 3;
    i.rn = Reg(i, 5);
    i.rd = Reg(i, 0);
    return true;
  }
  if (Bits(raw, 29, 27) == 0b011 && Bits(raw, 25, 24) == 0) {  // literal
    i.op = Op::kLdLit;
    i.imm = int64_t(i.va + uint64_t(Sext(Bits(raw, 23, 5), 19) * 4));
    i.load = true;
    switch (Bits(raw, 31, 30)) {
      case 0: i.size = 4; break;
      case 1: i.size = 8; i.sf = true; break;
      case 2: i.size = 4; i.sf = true; i.sign = true; break;
      case 3: i.prefetch = true; i.load = false; return true;
    }
    i.rd = Reg(i, 0);
    return true;
  }
  if (Bits(raw, 29, 27) == 0b101) {  // pair
    uint32_t opc = Bits(raw, 31, 30), m = Bits(raw, 24, 23);
    bool l = Bit(raw, 22);
    switch (opc) {
      case 0: i.size = 4; break;
      case 1:
        if (!l || m == 0) return false;  // LDPSW only (STGP is MTE)
        i.size = 4; i.sign = true; i.sf = true;
        break;
      case 2: i.size = 8; i.sf = true; break;
      default: return false;
    }
    static const uint8_t kModes[4] = {kOffset, kPostIndex, kOffset, kPreIndex};
    i.op = Op::kLdStPair;
    i.mode = kModes[m];
    i.load = l;
    i.imm = Sext(Bits(raw, 21, 15), 7) * i.size;
    i.ra = Reg(i, 10);
    i.rn = Reg(i, 5);
    i.rd = Reg(i, 0);
    return true;
  }
  if (Bits(raw, 29, 27) == 0b111) {  // single register
    uint32_t size = Bits(raw, 31, 30), opc = Bits(raw, 23, 22);
    switch (opc) {
      case 0: i.sf = size == 3; break;
      case 1: i.load = true; i.sf = size == 3; break;
      case 2:
        if (size == 3) i.prefetch = true;
        else i.load = i.sign = i.sf = true;
        break;
      case 3:
        if (size >= 2) return false;
        i.load = i.sign = true;
        break;
    }
    i.size = uint8_t(1u << size);
    if (Bits(raw, 25, 24) == 1) {
      i.mode = kOffset;
      i.imm = int64_t(Bits(raw, 21, 10)) << size;
    } else if (Bits(raw, 25, 24) == 0 && !Bit(raw, 21)) {
      i.imm = Sext(Bits(raw, 20, 12), 9);
      switch (Bits(raw, 11, 10)) {
        case 0: i.mode = kOffset; break;
        case 1: i.mode = kPostIndex; break;
        case 3: i.mode = kPreIndex; break;
        default: return false;  // unprivileged
      }
      if (i.prefetch && i.mode != kOffset) return false;
    } else if (Bits(raw, 25, 24) == 0 && Bits(raw, 11, 10) == 2) {
      uint32_t option = Bits(raw, 15, 13);
      if (!(option & 2)) return false;
      i.mode = kRegOffset;
      i.shift = uint8_t(option);
      i.amount = Bit(raw, 12) ? uint8_t(size) : 0;
      i.rm = Reg(i, 16);
    } else {
      return false;  // atomic memory operations (LSE), PAC loads
    }
    i.op = Op::kLdSt;
    i.rn = Reg(i, 5);
    if (!i.prefetch) i.rd = Reg(i, 0);
    return true;
  }
  return false;
}

bool DpReg(Insn& i) {
  uint32_t raw = i.raw;
  i.sf = Bit(raw, 31);
  if (!Bit(raw, 28)) {
    if (Bits(raw, 27, 24) != 0b1010 && Bits(raw, 27, 24) != 0b1011) return false;
    if (!Bit(raw, 24)) {  // logical (shifted register)
      i.op = Op::kLogShift;
      i.opc = uint8_t(Bits(raw, 30, 29));
      i.setf = i.opc == 3;
      i.inv = Bit(raw, 21);
      i.shift = uint8_t(Bits(raw, 23, 22));
      i.amount = uint8_t(Bits(raw, 15, 10));
      if (!i.sf && i.amount >= 32) return false;
    } else if (!Bit(raw, 21)) {  // add/sub (shifted register)
      i.op = Op::kAddShift;
      i.shift = uint8_t(Bits(raw, 23, 22));
      i.amount = uint8_t(Bits(raw, 15, 10));
      if (i.shift == kRor || (!i.sf && i.amount >= 32)) return false;
    } else {  // add/sub (extended register)
      if (Bits(raw, 23, 22) || Bits(raw, 12, 10) > 4) return false;
      i.op = Op::kAddExt;
      i.shift = uint8_t(Bits(raw, 15, 13));
      i.amount = uint8_t(Bits(raw, 12, 10));
    }
    if (i.op != Op::kLogShift) {
      i.sub = Bit(raw, 30);
      i.setf = Bit(raw, 29);
    }
    i.rm = Reg(i, 16);
    i.rn = Reg(i, 5);
    i.rd = Reg(i, 0);
    return true;
  }
  uint32_t top = Bits(raw, 28, 21);
  if (top == 0b11010000) {  // ADC/SBC
    if (Bits(raw, 15, 10)) return false;
    i.op = Op::kAdc;
    i.sub = Bit(raw, 30);
    i.setf = Bit(raw, 29);
  } else if (top == 0b11010010) {  // CCMN/CCMP
    if (!Bit(raw, 29) || Bit(raw, 10) || Bit(raw, 4)) return false;
    i.op = Op::kCondCmp;
    i.sub = Bit(raw, 30);
    i.setf = true;
    i.cond = uint8_t(Bits(raw, 15, 12));
    i.nzcv = uint8_t(Bits(raw, 3, 0));
    i.use_imm = Bit(raw, 11);
    if (i.use_imm) i.imm = Bits(raw, 20, 16);
    else i.rm = Reg(i, 16);
    i.rn = Reg(i, 5);
    return true;
  } else if (top == 0b11010100) {  // conditional select
    if (Bit(raw, 29) || Bit(raw, 11)) return false;
    i.op = Op::kCondSel;
    i.opc = uint8_t((Bit(raw, 30) << 1) | Bit(raw, 10));
    i.cond = uint8_t(Bits(raw, 15, 12));
  } else if (top == 0b11010110) {
    if (Bit(raw, 29)) return false;
    uint32_t opcode = Bits(raw, 15, 10);
    if (!Bit(raw, 30)) {  // two-source
      static const int8_t kOpc[12] = {-1, -1, 0, 1, -1, -1, -1, -1, 2, 3, 4, 5};
      if (opcode >= 12 || kOpc[opcode] < 0) return false;
      i.op = Op::kDp2;
      i.opc = uint8_t(kOpc[opcode]);
    } else {  // one-source: opc 0 RBIT 1 REV16 2 REV32 3 REV 4 CLZ 5 CLS
      if (Bits(raw, 20, 16) || opcode > 5) return false;
      if (opcode == 3 && !i.sf) return false;
      i.op = Op::kDp1;
      i.opc = uint8_t(opcode == 2 && !i.sf ? 3 : opcode);
      i.rn = Reg(i, 5);
      i.rd = Reg(i, 0);
      return true;
    }
  } else if (Bits(raw, 28, 24) == 0b11011) {  // three-source
    if (Bits(raw, 30, 29)) return false;
    uint32_t op31 = Bits(raw, 23, 21);
    bool o0 = Bit(raw, 15);
    // opc: 0 MADD 1 MSUB 2 SMADDL 3 SMSUBL 4 SMULH 5 UMADDL 6 UMSUBL 7 UMULH
    switch (op31) {
      case 0: i.opc = o0; break;
      case 1: i.opc = uint8_t(2 + o0); break;
      case 2: i.opc = 4; break;
      case 5: i.opc = uint8_t(5 + o0); break;
      case 6: i.opc = 7; break;
      default: return false;
    }
    if (op31 && !i.sf) return false;
    if ((i.opc == 4 || i.opc == 7) && (o0 || Bits(raw, 14, 10) != 31)) return false;
    i.op = Op::kDp3;
    if (i.opc != 4 && i.opc != 7) i.ra = Reg(i, 10);
  } else {
    return false;
  }
  i.rm = Reg(i, 16);
  i.rn = Reg(i, 5);
  i.rd = Reg(i, 0);
  return true;
}

}  // namespace

Insn Decode(uint64_t va, uint32_t raw) {
  Insn i;
  i.va = va;
  i.raw = raw;
  uint32_t op0 = Bits(raw, 28, 25);
  bool ok = false;
  if ((op0 & 0b1110) == 0b1000) ok = DpImm(i);
  else if ((op0 & 0b1110) == 0b1010) ok = BranchSys(i);
  else if ((op0 & 0b0101) == 0b0100) ok = LdSt(i);
  else if ((op0 & 0b0111) == 0b0101) ok = DpReg(i);
  if (!ok) {
    i = Insn{};
    i.va = va;
    i.raw = raw;
  }
  return i;
}

bool IsTerminator(const Insn& i) {
  switch (i.op) {
    case Op::kInvalid: case Op::kB: case Op::kBr: case Op::kRet: case Op::kBrk:
      return true;
    default:
      return false;
  }
}

std::string Name(const Insn& i) {
  static const char* kLog[4] = {"and", "orr", "eor", "ands"};
  static const char* kLogInv[4] = {"bic", "orn", "eon", "bics"};
  static const char* kMov[4] = {"movn", "?", "movz", "movk"};
  static const char* kBf[3] = {"sbfm", "bfm", "ubfm"};
  static const char* kSel[4] = {"csel", "csinc", "csinv", "csneg"};
  static const char* kDp1[6] = {"rbit", "rev16", "rev32", "rev", "clz", "cls"};
  static const char* kDp2[6] = {"udiv", "sdiv", "lslv", "lsrv", "asrv", "rorv"};
  static const char* kDp3[8] = {"madd", "msub", "smaddl", "smsubl",
                                "smulh", "umaddl", "umsubl", "umulh"};
  auto as = [&](const char* base) { return std::string(base) + (i.setf ? "s" : ""); };
  switch (i.op) {
    case Op::kInvalid: return "(invalid)";
    case Op::kAdr: return (i.raw >> 31) ? "adrp" : "adr";
    case Op::kAddImm: case Op::kAddShift: case Op::kAddExt: return as(i.sub ? "sub" : "add");
    case Op::kAdc: return as(i.sub ? "sbc" : "adc");
    case Op::kLogImm: return kLog[i.opc];
    case Op::kLogShift: return (i.inv ? kLogInv : kLog)[i.opc];
    case Op::kMovWide: return kMov[i.opc];
    case Op::kBitfield: return kBf[i.opc];
    case Op::kExtr: return "extr";
    case Op::kCondCmp: return i.sub ? "ccmp" : "ccmn";
    case Op::kCondSel: return kSel[i.opc];
    case Op::kDp1: return kDp1[i.opc];
    case Op::kDp2: return kDp2[i.opc];
    case Op::kDp3: return kDp3[i.opc];
    case Op::kB: return "b";
    case Op::kBl: return "bl";
    case Op::kBCond: return "b.cond";
    case Op::kCbz: return i.opc ? "cbnz" : "cbz";
    case Op::kTbz: return i.opc ? "tbnz" : "tbz";
    case Op::kBr: return "br";
    case Op::kBlr: return "blr";
    case Op::kRet: return "ret";
    case Op::kSvc: return "svc";
    case Op::kBrk: return "brk";
    case Op::kHint: return "hint";
    case Op::kBarrier: return i.opc == 4 ? "dsb" : i.opc == 5 ? "dmb" : "isb";
    case Op::kClrex: return "clrex";
    case Op::kMrs: return "mrs";
    case Op::kMsr: return "msr";
    case Op::kLdSt: return i.prefetch ? "prfm" : i.load ? "ldr" : "str";
    case Op::kLdStPair: return i.load ? "ldp" : "stp";
    case Op::kLdLit: return i.prefetch ? "prfm" : "ldr";
    case Op::kLdStExcl:
      return std::string(i.load ? (i.acquire ? "ldaxr" : "ldxr") : (i.release ? "stlxr" : "stxr"));
    case Op::kLdStOrder: return i.load ? "ldar" : "stlr";
  }
  return "?";
}

}  // namespace lifter::a64
