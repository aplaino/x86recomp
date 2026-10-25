#include "ppc.h"

#include <cstdio>

namespace lifter::ppc {
namespace {

// Instruction fields in the ISA's numbering: bit 0 is the most
// significant bit of the word.
uint32_t F(uint32_t raw, int bit, int len) { return (raw >> (32 - bit - len)) & ((1u << len) - 1); }
int64_t Sext(uint64_t v, int bits) { return int64_t(v << (64 - bits)) >> (64 - bits); }

// The register fields, and Rc where the form has one (M, MD, MDS and the
// X/XO forms; ANDI. ANDIS. ADDIC. set it themselves; elsewhere bit 31 is
// part of an immediate or LK).
void Regs(Insn& i) {
  i.rd = uint8_t(F(i.raw, 6, 5));
  i.ra = uint8_t(F(i.raw, 11, 5));
  i.rb = uint8_t(F(i.raw, 16, 5));
  uint32_t primary = i.raw >> 26;
  i.rc = (primary == 20 || primary == 21 || primary == 23 || primary == 30 || primary == 31) && (i.raw & 1);
}

void Mem(Insn& i, Op op, int size, bool update, bool sign = false) {
  i.op = op;
  i.size = uint8_t(size);
  i.update = update;
  i.sign = sign;
  i.imm = Sext(i.raw & 0xFFFF, 16);
}
void MemX(Insn& i, Op op, int size, bool update, bool sign = false, bool byterev = false) {
  Mem(i, op, size, update, sign);
  i.imm = 0;
  i.indexed = true;
  i.byterev = byterev;
}

// MD and MDS forms: 6-bit shift and mask fields with their high bit apart.
uint8_t MdSh(uint32_t raw) { return uint8_t(F(raw, 16, 5) | F(raw, 30, 1) << 5); }
uint8_t MdMb(uint32_t raw) { return uint8_t(F(raw, 21, 5) | F(raw, 26, 1) << 5); }

void Primary31(Insn& i) {
  const uint32_t xo = F(i.raw, 21, 10);
  // XO form arithmetic: a 9-bit extended opcode and OE.
  const uint32_t xo9 = F(i.raw, 22, 9);
  const bool oe = F(i.raw, 21, 1);
  auto arith = [&](Op op, bool oe_allowed = true) {
    i.op = op;
    i.oe = oe_allowed && oe;
    return !(oe && !oe_allowed);
  };
  switch (xo9) {
    case 8: arith(Op::kSubfc); return;
    case 10: arith(Op::kAddc); return;
    case 40: arith(Op::kSubf); return;
    case 104: if (!i.rb) arith(Op::kNeg); return;
    case 136: arith(Op::kSubfe); return;
    case 138: arith(Op::kAdde); return;
    case 200: if (!i.rb) arith(Op::kSubfze); return;
    case 202: if (!i.rb) arith(Op::kAddze); return;
    case 232: if (!i.rb) arith(Op::kSubfme); return;
    case 233: arith(Op::kMulld); return;
    case 234: if (!i.rb) arith(Op::kAddme); return;
    case 235: arith(Op::kMullw); return;
    case 266: arith(Op::kAdd); return;
    case 457: arith(Op::kDivdu); return;
    case 459: arith(Op::kDivwu); return;
    case 489: arith(Op::kDivd); return;
    case 491: arith(Op::kDivw); return;
    default: break;
  }
  if (!oe) {
    switch (xo9) {
      case 9: i.op = Op::kMulhdu; return;
      case 11: i.op = Op::kMulhwu; return;
      case 73: i.op = Op::kMulhd; return;
      case 75: i.op = Op::kMulhw; return;
      default: break;
    }
  }
  // sradi is XS form: a 9-bit opcode in bits 21-29 and sh5 in bit 30.
  if (F(i.raw, 21, 9) == 413) {
    i.op = Op::kSradi;
    i.sh = MdSh(i.raw);
    return;
  }
  switch (xo) {
    case 0: case 32:
      if (F(i.raw, 9, 1)) return;
      i.op = xo ? Op::kCmpl : Op::kCmp;
      i.crfd = uint8_t(F(i.raw, 6, 3));
      i.l = F(i.raw, 10, 1);
      i.rc = false;
      return;
    case 4: case 68:
      i.op = xo == 4 ? Op::kTw : Op::kTd;
      i.to = i.rd;
      return;
    case 19:
      // MFCR, and MFOCRF with one field selected: the ISA lets MFOCRF set
      // the other fields to anything, so both copy the whole CR.
      if (F(i.raw, 11, 1) && __builtin_popcount(F(i.raw, 12, 8)) != 1) return;
      i.op = Op::kMfcr;
      return;
    // The larx forms' bit 31 is the EH hint, not Rc.
    case 20: i.op = Op::kLarx; i.size = 4; i.indexed = true; i.rc = false; return;
    case 84: i.op = Op::kLarx; i.size = 8; i.indexed = true; i.rc = false; return;
    case 150: if (i.rc) { i.op = Op::kStcx; i.size = 4; i.indexed = true; } return;
    case 214: if (i.rc) { i.op = Op::kStcx; i.size = 8; i.indexed = true; } return;
    case 24: i.op = Op::kSlw; return;
    case 26: if (!i.rb) i.op = Op::kCntlzw; return;
    case 27: i.op = Op::kSld; return;
    case 28: i.op = Op::kAnd; return;
    case 58: if (!i.rb) i.op = Op::kCntlzd; return;
    case 60: i.op = Op::kAndc; return;
    case 124: i.op = Op::kNor; return;
    case 284: i.op = Op::kEqv; return;
    case 316: i.op = Op::kXor; return;
    case 412: i.op = Op::kOrc; return;
    case 444: i.op = Op::kOr; return;
    case 476: i.op = Op::kNand; return;
    case 536: i.op = Op::kSrw; return;
    case 539: i.op = Op::kSrd; return;
    case 792: i.op = Op::kSraw; return;
    case 794: i.op = Op::kSrad; return;
    case 824: i.op = Op::kSrawi; i.sh = i.rb; return;
    case 922: if (!i.rb) i.op = Op::kExtsh; return;
    case 954: if (!i.rb) i.op = Op::kExtsb; return;
    case 986: if (!i.rb) i.op = Op::kExtsw; return;
    case 83: i.op = Op::kMfmsr; return;
    case 146: i.op = Op::kMtmsr; i.l = F(i.raw, 15, 1); return;
    case 178: i.op = Op::kMtmsrd; i.l = F(i.raw, 15, 1); return;
    case 144:
      // MTCRF, and MTOCRF with one field selected (the same operation).
      if (F(i.raw, 11, 1) && __builtin_popcount(F(i.raw, 12, 8)) != 1) return;
      i.op = Op::kMtcrf;
      i.crm = uint8_t(F(i.raw, 12, 8));
      return;
    case 339: case 467: case 371:
      i.spr = uint16_t(F(i.raw, 11, 5) | F(i.raw, 16, 5) << 5);
      i.op = xo == 339 ? Op::kMfspr : xo == 467 ? Op::kMtspr : Op::kMftb;
      if (i.op == Op::kMftb && i.spr != kTbl && i.spr != kTbu) i.op = Op::kInvalid;
      return;
    case 512:
      i.op = Op::kMcrxr;
      i.crfd = uint8_t(F(i.raw, 6, 3));
      return;
    case 598:
      if (F(i.raw, 9, 2) > 1) return;  // PTESYNC is privileged
      i.op = Op::kSync;
      i.opc = uint16_t(F(i.raw, 9, 2));
      return;
    case 854: i.op = Op::kSync; i.opc = 2; return;
    case 54: case 86: case 246: case 278: case 982:
      i.op = Op::kCacheHint;
      return;
    case 1014:
      // DCBZ, and the Xenon's DCBZ128 (the L bit, bit 10, set).
      if (i.rd > 1) return;
      i.op = Op::kDcbz;
      i.size = i.rd ? 128 : 32;
      return;
    // Indexed loads and stores.
    case 23: MemX(i, Op::kLoad, 4, false); return;
    case 55: MemX(i, Op::kLoad, 4, true); return;
    case 87: MemX(i, Op::kLoad, 1, false); return;
    case 119: MemX(i, Op::kLoad, 1, true); return;
    case 279: MemX(i, Op::kLoad, 2, false); return;
    case 311: MemX(i, Op::kLoad, 2, true); return;
    case 343: MemX(i, Op::kLoad, 2, false, true); return;
    case 375: MemX(i, Op::kLoad, 2, true, true); return;
    case 341: MemX(i, Op::kLoad, 4, false, true); return;
    case 373: MemX(i, Op::kLoad, 4, true, true); return;
    case 21: MemX(i, Op::kLoad, 8, false); return;
    case 53: MemX(i, Op::kLoad, 8, true); return;
    case 790: MemX(i, Op::kLoad, 2, false, false, true); return;
    case 534: MemX(i, Op::kLoad, 4, false, false, true); return;
    case 532: MemX(i, Op::kLoad, 8, false, false, true); return;
    case 151: MemX(i, Op::kStore, 4, false); return;
    case 183: MemX(i, Op::kStore, 4, true); return;
    case 215: MemX(i, Op::kStore, 1, false); return;
    case 247: MemX(i, Op::kStore, 1, true); return;
    case 407: MemX(i, Op::kStore, 2, false); return;
    case 439: MemX(i, Op::kStore, 2, true); return;
    case 149: MemX(i, Op::kStore, 8, false); return;
    case 181: MemX(i, Op::kStore, 8, true); return;
    case 918: MemX(i, Op::kStore, 2, false, false, true); return;
    case 662: MemX(i, Op::kStore, 4, false, false, true); return;
    case 660: MemX(i, Op::kStore, 8, false, false, true); return;
    default: return;
  }
}

// Primary opcode 31 forms that have no record form: Rc = 1 is invalid.
bool NoRecordForm(Op op) {
  switch (op) {
    case Op::kCmp: case Op::kCmpl: case Op::kTw: case Op::kTd: case Op::kMfcr: case Op::kMtcrf:
    case Op::kMfspr: case Op::kMtspr: case Op::kMftb: case Op::kMfmsr: case Op::kMtmsr:
    case Op::kMtmsrd: case Op::kMcrxr: case Op::kSync: case Op::kCacheHint: case Op::kDcbz:
    case Op::kLoad: case Op::kStore:
      return true;
    default:
      return false;
  }
}

void Primary19(Insn& i) {
  const uint32_t xo = F(i.raw, 21, 10);
  i.bo = uint8_t(F(i.raw, 6, 5));
  i.bi = uint8_t(F(i.raw, 11, 5));
  i.lk = i.raw & 1;
  switch (xo) {
    case 0:
      if (i.raw & 0x0063F801) return;
      i.op = Op::kMcrf;
      i.crfd = uint8_t(F(i.raw, 6, 3));
      i.crfs = uint8_t(F(i.raw, 11, 3));
      return;
    case 16:
      if (F(i.raw, 16, 3)) return;  // BH hints other than 0
      i.op = Op::kBclr;
      return;
    case 528:
      // BCCTR may not decrement CTR (BO bit 2 clear).
      if (!(i.bo & 4) || F(i.raw, 16, 3)) return;
      i.op = Op::kBcctr;
      return;
    case 150:
      i.op = Op::kSync;
      i.opc = 3;
      i.lk = false;
      return;
    case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
      if (i.raw & 1) return;
      i.op = Op::kCrLogic;
      i.opc = uint16_t(xo);
      i.rb = uint8_t(F(i.raw, 16, 5));
      i.lk = false;
      return;
    default:
      i.lk = false;
      return;
  }
}

}  // namespace

Insn Decode(uint64_t va, uint32_t raw) {
  Insn i;
  i.va = va;
  i.raw = raw;
  Regs(i);
  const uint32_t simm = raw & 0xFFFF;
  switch (raw >> 26) {
    case 2: case 3:
      i.op = raw >> 26 == 2 ? Op::kTdi : Op::kTwi;
      i.to = i.rd;
      i.imm = Sext(simm, 16);
      break;
    case 7: i.op = Op::kMulli; i.imm = Sext(simm, 16); break;
    case 8: i.op = Op::kSubfic; i.imm = Sext(simm, 16); break;
    case 10: case 11:
      if (F(raw, 9, 1)) break;
      i.op = raw >> 26 == 10 ? Op::kCmpli : Op::kCmpi;
      i.crfd = uint8_t(F(raw, 6, 3));
      i.l = F(raw, 10, 1);
      i.imm = raw >> 26 == 10 ? int64_t(simm) : Sext(simm, 16);
      i.rc = false;
      break;
    case 12: case 13:
      i.op = Op::kAddic;
      i.imm = Sext(simm, 16);
      i.rc = raw >> 26 == 13;
      break;
    case 14: i.op = Op::kAddi; i.imm = Sext(simm, 16); break;
    case 15: i.op = Op::kAddi; i.imm = Sext(simm, 16) * 65536; break;
    case 16:
      i.op = Op::kBc;
      i.bo = i.rd;
      i.bi = i.ra;
      i.lk = raw & 1;
      i.imm = Sext(raw & 0xFFFC, 16) + ((raw & 2) ? 0 : int64_t(va));
      break;
    case 17:
      if ((raw & 0x03FFFFFF) != 2) break;  // LEV 0: no hypervisor calls
      i.op = Op::kSc;
      break;
    case 18:
      i.op = Op::kB;
      i.lk = raw & 1;
      i.imm = Sext(raw & 0x03FFFFFC, 26) + ((raw & 2) ? 0 : int64_t(va));
      break;
    case 19: Primary19(i); break;
    case 20: case 21: case 23:
      i.op = raw >> 26 == 20 ? Op::kRlwimi : raw >> 26 == 21 ? Op::kRlwinm : Op::kRlwnm;
      i.sh = uint8_t(F(raw, 16, 5));
      i.mb = uint8_t(F(raw, 21, 5));
      i.me = uint8_t(F(raw, 26, 5));
      break;
    case 24: i.op = Op::kOri; i.imm = simm; break;
    case 25: i.op = Op::kOri; i.imm = int64_t(simm) << 16; break;
    case 26: i.op = Op::kXori; i.imm = simm; break;
    case 27: i.op = Op::kXori; i.imm = int64_t(simm) << 16; break;
    case 28: i.op = Op::kAndi; i.imm = simm; i.rc = true; break;
    case 29: i.op = Op::kAndi; i.imm = int64_t(simm) << 16; i.rc = true; break;
    case 30:
      i.mb = MdMb(raw);
      i.sh = MdSh(raw);
      switch (F(raw, 27, 3)) {
        case 0: i.op = Op::kRldicl; break;
        case 1: i.op = Op::kRldicr; break;
        case 2: i.op = Op::kRldic; break;
        case 3: i.op = Op::kRldimi; break;
        default:
          if (F(raw, 27, 4) == 8) i.op = Op::kRldcl;
          if (F(raw, 27, 4) == 9) i.op = Op::kRldcr;
          i.sh = 0;
          break;
      }
      break;
    case 31:
      Primary31(i);
      if (i.rc && NoRecordForm(i.op)) i.op = Op::kInvalid;
      break;
    case 32: Mem(i, Op::kLoad, 4, false); break;
    case 33: Mem(i, Op::kLoad, 4, true); break;
    case 34: Mem(i, Op::kLoad, 1, false); break;
    case 35: Mem(i, Op::kLoad, 1, true); break;
    case 36: Mem(i, Op::kStore, 4, false); break;
    case 37: Mem(i, Op::kStore, 4, true); break;
    case 38: Mem(i, Op::kStore, 1, false); break;
    case 39: Mem(i, Op::kStore, 1, true); break;
    case 40: Mem(i, Op::kLoad, 2, false); break;
    case 41: Mem(i, Op::kLoad, 2, true); break;
    case 42: Mem(i, Op::kLoad, 2, false, true); break;
    case 43: Mem(i, Op::kLoad, 2, true, true); break;
    case 44: Mem(i, Op::kStore, 2, false); break;
    case 45: Mem(i, Op::kStore, 2, true); break;
    case 46: i.op = Op::kLmw; i.imm = Sext(simm, 16); break;
    case 47: i.op = Op::kStmw; i.imm = Sext(simm, 16); break;
    case 58:
      switch (raw & 3) {
        case 0: Mem(i, Op::kLoad, 8, false); break;
        case 1: Mem(i, Op::kLoad, 8, true); break;
        case 2: Mem(i, Op::kLoad, 4, false, true); break;
        default: break;
      }
      i.imm = Sext(raw & 0xFFFC, 16);
      break;
    case 62:
      switch (raw & 3) {
        case 0: Mem(i, Op::kStore, 8, false); break;
        case 1: Mem(i, Op::kStore, 8, true); break;
        default: break;
      }
      i.imm = Sext(raw & 0xFFFC, 16);
      break;
    default:
      break;
  }
  // Invalid forms the ISA calls out: an update form whose base is r0, or
  // (loads) the target register.
  if ((i.op == Op::kLoad || i.op == Op::kStore) && i.update &&
      (i.ra == 0 || (i.op == Op::kLoad && i.ra == i.rd)))
    i.op = Op::kInvalid;
  if (i.op == Op::kLmw && i.ra >= i.rd && i.ra != 0) i.op = Op::kInvalid;
  return i;
}

bool IsTerminator(const Insn& i) {
  switch (i.op) {
    case Op::kB:
      return !i.lk;
    case Op::kBc: case Op::kBclr: case Op::kBcctr:
      // Branch always (BO = 1z1zz) without link.
      return !i.lk && (i.bo & 0x14) == 0x14;
    case Op::kInvalid:
      return true;
    default:
      return false;
  }
}

std::string Name(const Insn& i) {
  switch (i.op) {
    case Op::kInvalid: return "(invalid)";
    case Op::kAddi: return (i.raw >> 26) == 15 ? "addis" : "addi";
    case Op::kAddic: return i.rc ? "addic." : "addic";
    case Op::kSubfic: return "subfic";
    case Op::kMulli: return "mulli";
    case Op::kAdd: return "add";
    case Op::kAddc: return "addc";
    case Op::kAdde: return "adde";
    case Op::kAddme: return "addme";
    case Op::kAddze: return "addze";
    case Op::kSubf: return "subf";
    case Op::kSubfc: return "subfc";
    case Op::kSubfe: return "subfe";
    case Op::kSubfme: return "subfme";
    case Op::kSubfze: return "subfze";
    case Op::kNeg: return "neg";
    case Op::kMullw: return "mullw";
    case Op::kMulld: return "mulld";
    case Op::kMulhw: return "mulhw";
    case Op::kMulhwu: return "mulhwu";
    case Op::kMulhd: return "mulhd";
    case Op::kMulhdu: return "mulhdu";
    case Op::kDivw: return "divw";
    case Op::kDivwu: return "divwu";
    case Op::kDivd: return "divd";
    case Op::kDivdu: return "divdu";
    case Op::kAndi: return (i.raw >> 26) == 29 ? "andis." : "andi.";
    case Op::kOri: return (i.raw >> 26) == 25 ? "oris" : "ori";
    case Op::kXori: return (i.raw >> 26) == 27 ? "xoris" : "xori";
    case Op::kAnd: return "and";
    case Op::kAndc: return "andc";
    case Op::kOr: return "or";
    case Op::kOrc: return "orc";
    case Op::kXor: return "xor";
    case Op::kNand: return "nand";
    case Op::kNor: return "nor";
    case Op::kEqv: return "eqv";
    case Op::kExtsb: return "extsb";
    case Op::kExtsh: return "extsh";
    case Op::kExtsw: return "extsw";
    case Op::kCntlzw: return "cntlzw";
    case Op::kCntlzd: return "cntlzd";
    case Op::kRlwinm: return "rlwinm";
    case Op::kRlwimi: return "rlwimi";
    case Op::kRlwnm: return "rlwnm";
    case Op::kRldicl: return "rldicl";
    case Op::kRldicr: return "rldicr";
    case Op::kRldic: return "rldic";
    case Op::kRldimi: return "rldimi";
    case Op::kRldcl: return "rldcl";
    case Op::kRldcr: return "rldcr";
    case Op::kSlw: return "slw";
    case Op::kSrw: return "srw";
    case Op::kSraw: return "sraw";
    case Op::kSrawi: return "srawi";
    case Op::kSld: return "sld";
    case Op::kSrd: return "srd";
    case Op::kSrad: return "srad";
    case Op::kSradi: return "sradi";
    case Op::kCmp: return "cmp";
    case Op::kCmpl: return "cmpl";
    case Op::kCmpi: return "cmpi";
    case Op::kCmpli: return "cmpli";
    case Op::kCrLogic: return "crlogic";
    case Op::kMcrf: return "mcrf";
    case Op::kMcrxr: return "mcrxr";
    case Op::kMfcr: return "mfcr";
    case Op::kMtcrf: return "mtcrf";
    case Op::kB: return i.lk ? "bl" : "b";
    case Op::kBc: return i.lk ? "bcl" : "bc";
    case Op::kBclr: return i.lk ? "bclrl" : "bclr";
    case Op::kBcctr: return i.lk ? "bcctrl" : "bcctr";
    case Op::kSc: return "sc";
    case Op::kTw: return "tw";
    case Op::kTwi: return "twi";
    case Op::kTd: return "td";
    case Op::kTdi: return "tdi";
    case Op::kMfspr: return "mfspr";
    case Op::kMtspr: return "mtspr";
    case Op::kMftb: return "mftb";
    case Op::kMfmsr: return "mfmsr";
    case Op::kMtmsr: return "mtmsr";
    case Op::kMtmsrd: return "mtmsrd";
    case Op::kSync: {
      static const char* kNames[4] = {"sync", "lwsync", "eieio", "isync"};
      return kNames[i.opc & 3];
    }
    case Op::kCacheHint: return "dcb/icb hint";
    case Op::kDcbz: return i.size == 128 ? "dcbz128" : "dcbz";
    case Op::kLoad: case Op::kStore: {
      static const char kSize[9] = {0, 'b', 'h', 0, 'w', 0, 0, 0, 'd'};
      std::string s = i.op == Op::kLoad ? "l" : "st";
      s += kSize[i.size];
      if (i.byterev) return s + "brx";
      if (i.op == Op::kLoad) s += i.sign ? "a" : (i.size == 8 ? "" : "z");
      if (i.update) s += "u";
      if (i.indexed) s += "x";
      return s;
    }
    case Op::kLmw: return "lmw";
    case Op::kStmw: return "stmw";
    case Op::kLarx: return i.size == 8 ? "ldarx" : "lwarx";
    case Op::kStcx: return i.size == 8 ? "stdcx." : "stwcx.";
  }
  return "?";
}

}  // namespace lifter::ppc
