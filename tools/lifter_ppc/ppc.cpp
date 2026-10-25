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
      // MFSPR from TBL or TBU is MFTB (the ISA's later spelling of it).
      if (i.op == Op::kMfspr && (i.spr == kTbl || i.spr == kTbu)) i.op = Op::kMftb;
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
    case 535: MemX(i, Op::kFpLoad, 4, false); return;
    case 567: MemX(i, Op::kFpLoad, 4, true); return;
    case 599: MemX(i, Op::kFpLoad, 8, false); return;
    case 631: MemX(i, Op::kFpLoad, 8, true); return;
    case 663: MemX(i, Op::kFpStore, 4, false); return;
    case 695: MemX(i, Op::kFpStore, 4, true); return;
    case 727: MemX(i, Op::kFpStore, 8, false); return;
    case 759: MemX(i, Op::kFpStore, 8, true); return;
    case 983: MemX(i, Op::kFpStore, 4, false); i.opc = 1; return;
    // VMX loads and stores (and the Xenon's LVLX family).
    case 103: case 359: MemX(i, Op::kVload, 16, false); i.opc = kVecQuad; return;
    case 231: case 487: MemX(i, Op::kVstore, 16, false); i.opc = kVecQuad; return;
    case 7: MemX(i, Op::kVload, 1, false); i.opc = kVecElement; return;
    case 39: MemX(i, Op::kVload, 2, false); i.opc = kVecElement; return;
    case 71: MemX(i, Op::kVload, 4, false); i.opc = kVecElement; return;
    case 135: MemX(i, Op::kVstore, 1, false); i.opc = kVecElement; return;
    case 167: MemX(i, Op::kVstore, 2, false); i.opc = kVecElement; return;
    case 199: MemX(i, Op::kVstore, 4, false); i.opc = kVecElement; return;
    case 6: MemX(i, Op::kVload, 0, false); i.opc = kVecShiftLeft; return;
    case 38: MemX(i, Op::kVload, 0, false); i.opc = kVecShiftRight; return;
    case 519: case 775: MemX(i, Op::kVload, 16, false); i.opc = kVecLeft; return;
    case 551: case 807: MemX(i, Op::kVload, 16, false); i.opc = kVecRight; return;
    case 647: case 903: MemX(i, Op::kVstore, 16, false); i.opc = kVecLeft; return;
    case 679: case 935: MemX(i, Op::kVstore, 16, false); i.opc = kVecRight; return;
    // DST DSTST DSS: data stream hints.
    case 342: case 374: case 822: i.op = Op::kCacheHint; return;
    default: return;
  }
}

// Primary opcode 31 forms that have no record form: Rc = 1 is invalid.
bool NoRecordForm(Op op) {
  switch (op) {
    case Op::kCmp: case Op::kCmpl: case Op::kTw: case Op::kTd: case Op::kMfcr: case Op::kMtcrf:
    case Op::kMfspr: case Op::kMtspr: case Op::kMftb: case Op::kMfmsr: case Op::kMtmsr:
    case Op::kMtmsrd: case Op::kMcrxr: case Op::kSync: case Op::kCacheHint: case Op::kDcbz:
    case Op::kLoad: case Op::kStore: case Op::kFpLoad: case Op::kFpStore: case Op::kVload:
    case Op::kVstore:
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

// Primary opcodes 59 (single precision) and 63: A forms by their 5-bit
// extended opcode, the rest X forms by their 10-bit one.
void PrimaryFp(Insn& i, bool single) {
  i.rc = i.raw & 1;
  i.rc_reg = uint8_t(F(i.raw, 21, 5));
  i.single = single;
  auto arith = [&](FpOp op, bool uses_a, bool uses_b, bool uses_c) {
    // Fields an operation does not use must be 0.
    if ((!uses_a && i.ra) || (!uses_b && i.rb) || (!uses_c && i.rc_reg)) return;
    i.op = Op::kFpArith;
    i.opc = op;
  };
  switch (F(i.raw, 26, 5)) {
    case 18: arith(kFDiv, true, true, false); return;
    case 20: arith(kFSub, true, true, false); return;
    case 21: arith(kFAdd, true, true, false); return;
    case 22: arith(kFSqrt, false, true, false); return;
    case 25: arith(kFMul, true, false, true); return;
    case 28: arith(kFMsub, true, true, true); return;
    case 29: arith(kFMadd, true, true, true); return;
    case 30: arith(kFNmsub, true, true, true); return;
    case 31: arith(kFNmadd, true, true, true); return;
    case 24: if (single) arith(kFRes, false, true, false); return;
    case 26: if (!single) arith(kFRsqrte, false, true, false); return;
    case 23: if (!single) i.op = Op::kFsel; return;
    default: break;
  }
  if (single) return;
  i.single = false;
  i.rc_reg = 0;
  const uint32_t xo = F(i.raw, 21, 10);
  auto noa = [&](Op op) {
    if (!i.ra) i.op = op;
  };
  switch (xo) {
    case 0: case 32:
      if (F(i.raw, 9, 2) || (i.raw & 1)) return;
      i.op = Op::kFcmp;
      i.opc = xo ? 1 : 0;
      i.crfd = uint8_t(F(i.raw, 6, 3));
      i.rc = false;
      return;
    case 12: noa(Op::kFrsp); return;
    case 14: noa(Op::kFctiw); i.opc = 0; return;
    case 15: noa(Op::kFctiw); i.opc = 1; return;
    case 814: noa(Op::kFctiw); i.opc = 2; return;
    case 815: noa(Op::kFctiw); i.opc = 3; return;
    case 846: noa(Op::kFcfid); return;
    case 72: noa(Op::kFpMove); i.opc = 0; return;
    case 40: noa(Op::kFpMove); i.opc = 1; return;
    case 264: noa(Op::kFpMove); i.opc = 2; return;
    case 136: noa(Op::kFpMove); i.opc = 3; return;
    case 583: if (!i.ra && !i.rb) i.op = Op::kMffs; return;
    case 711:
      if (F(i.raw, 6, 1) || F(i.raw, 15, 1)) return;
      i.op = Op::kMtfsf;
      i.crm = uint8_t(F(i.raw, 7, 8));
      return;
    case 134:
      if (F(i.raw, 9, 7) || F(i.raw, 20, 1)) return;
      i.op = Op::kMtfsfi;
      i.crfd = uint8_t(F(i.raw, 6, 3));
      i.imm = F(i.raw, 16, 4);
      return;
    case 38: case 70:
      if (i.ra || i.rb) return;
      i.op = Op::kMtfsb;
      i.bo = i.rd;
      i.opc = xo == 38 ? 1 : 0;
      return;
    case 64:
      if (F(i.raw, 9, 2) || F(i.raw, 14, 7) || (i.raw & 1)) return;
      i.op = Op::kMcrfs;
      i.crfd = uint8_t(F(i.raw, 6, 3));
      i.crfs = uint8_t(F(i.raw, 11, 3));
      i.rc = false;
      return;
    default:
      return;
  }
}

// VMX's operations by encoding, for decoding and for VmxEncoding: the VA
// forms by their 6-bit extended opcode (32-47), the VC forms (compares) by
// their 10-bit one with Rc in bit 21, the VX forms by the 11-bit one.
// kOne: the operation reads only vB (vA must be 0); kUimm, kSimm: the vA
// field is an immediate.
enum VxForm : uint8_t { kTwo, kOne, kUimm, kSimm };
const struct { uint8_t xo; VmxOp op; } kVa[] = {
    {32, kVmhaddshs}, {33, kVmhraddshs}, {34, kVmladduhm}, {36, kVmsumubm}, {37, kVmsummbm},
    {38, kVmsumuhm}, {39, kVmsumuhs}, {40, kVmsumshm}, {41, kVmsumshs}, {42, kVsel}, {43, kVperm},
    {44, kVsldoi}, {46, kVmaddfp}, {47, kVnmsubfp}};
const struct { uint16_t xo; VmxOp op; } kVc[] = {
    {6, kVcmpequb}, {70, kVcmpequh}, {134, kVcmpequw}, {198, kVcmpeqfp}, {454, kVcmpgefp},
    {518, kVcmpgtub}, {582, kVcmpgtuh}, {646, kVcmpgtuw}, {710, kVcmpgtfp}, {774, kVcmpgtsb},
    {838, kVcmpgtsh}, {902, kVcmpgtsw}, {966, kVcmpbfp}};
const struct { uint16_t xo; VmxOp op; uint8_t form; } kVx[] = {
    {0, kVaddubm, kTwo}, {64, kVadduhm, kTwo}, {128, kVadduwm, kTwo}, {384, kVaddcuw, kTwo},
    {512, kVaddubs, kTwo}, {576, kVadduhs, kTwo}, {640, kVadduws, kTwo}, {768, kVaddsbs, kTwo},
    {832, kVaddshs, kTwo}, {896, kVaddsws, kTwo},
    {1024, kVsububm, kTwo}, {1088, kVsubuhm, kTwo}, {1152, kVsubuwm, kTwo}, {1408, kVsubcuw, kTwo},
    {1536, kVsububs, kTwo}, {1600, kVsubuhs, kTwo}, {1664, kVsubuws, kTwo}, {1792, kVsubsbs, kTwo},
    {1856, kVsubshs, kTwo}, {1920, kVsubsws, kTwo},
    {1026, kVavgub, kTwo}, {1090, kVavguh, kTwo}, {1154, kVavguw, kTwo}, {1282, kVavgsb, kTwo},
    {1346, kVavgsh, kTwo}, {1410, kVavgsw, kTwo},
    {2, kVmaxub, kTwo}, {66, kVmaxuh, kTwo}, {130, kVmaxuw, kTwo}, {258, kVmaxsb, kTwo},
    {322, kVmaxsh, kTwo}, {386, kVmaxsw, kTwo},
    {514, kVminub, kTwo}, {578, kVminuh, kTwo}, {642, kVminuw, kTwo}, {770, kVminsb, kTwo},
    {834, kVminsh, kTwo}, {898, kVminsw, kTwo},
    {520, kVmuleub, kTwo}, {584, kVmuleuh, kTwo}, {776, kVmulesb, kTwo}, {840, kVmulesh, kTwo},
    {8, kVmuloub, kTwo}, {72, kVmulouh, kTwo}, {264, kVmulosb, kTwo}, {328, kVmulosh, kTwo},
    {1544, kVsum4ubs, kTwo}, {1800, kVsum4sbs, kTwo}, {1608, kVsum4shs, kTwo},
    {1672, kVsum2sws, kTwo}, {1928, kVsumsws, kTwo},
    {1028, kVand, kTwo}, {1092, kVandc, kTwo}, {1156, kVor, kTwo}, {1284, kVnor, kTwo},
    {1220, kVxor, kTwo},
    {4, kVrlb, kTwo}, {68, kVrlh, kTwo}, {132, kVrlw, kTwo}, {260, kVslb, kTwo},
    {324, kVslh, kTwo}, {388, kVslw, kTwo}, {516, kVsrb, kTwo}, {580, kVsrh, kTwo},
    {644, kVsrw, kTwo}, {772, kVsrab, kTwo}, {836, kVsrah, kTwo}, {900, kVsraw, kTwo},
    {452, kVsl, kTwo}, {708, kVsr, kTwo}, {1036, kVslo, kTwo}, {1100, kVsro, kTwo},
    {12, kVmrghb, kTwo}, {76, kVmrghh, kTwo}, {140, kVmrghw, kTwo}, {268, kVmrglb, kTwo},
    {332, kVmrglh, kTwo}, {396, kVmrglw, kTwo},
    {524, kVspltb, kUimm}, {588, kVsplth, kUimm}, {652, kVspltw, kUimm},
    {780, kVspltisb, kSimm}, {844, kVspltish, kSimm}, {908, kVspltisw, kSimm},
    {14, kVpkuhum, kTwo}, {78, kVpkuwum, kTwo}, {142, kVpkuhus, kTwo}, {206, kVpkuwus, kTwo},
    {270, kVpkshus, kTwo}, {334, kVpkswus, kTwo}, {398, kVpkshss, kTwo}, {462, kVpkswss, kTwo},
    {782, kVpkpx, kTwo},
    {526, kVupkhsb, kOne}, {590, kVupkhsh, kOne}, {654, kVupklsb, kOne}, {718, kVupklsh, kOne},
    {846, kVupkhpx, kOne}, {974, kVupklpx, kOne},
    {10, kVaddfp, kTwo}, {74, kVsubfp, kTwo}, {1034, kVmaxfp, kTwo}, {1098, kVminfp, kTwo},
    {266, kVrefp, kOne}, {330, kVrsqrtefp, kOne}, {394, kVexptefp, kOne}, {458, kVlogefp, kOne},
    {522, kVrfin, kOne}, {586, kVrfiz, kOne}, {650, kVrfip, kOne}, {714, kVrfim, kOne},
    {778, kVcfux, kUimm}, {842, kVcfsx, kUimm}, {906, kVctuxs, kUimm}, {970, kVctsxs, kUimm}};

// VMX128, the Xenon's extension of VMX to 128 vector registers, in
// primary opcodes 4 (loads, stores and VSLDOI128), 5 and 6. Encodings as
// Xenia's (BSD) opcode table has them (ppc_opcode_table_gen.cc). The 7-bit
// register numbers are split across the word: vD in bits 6-10 and 28-29,
// vA in 11-15, 26 and 21, vB in 16-20 and 30-31.
uint8_t Vd128(uint32_t raw) { return uint8_t(F(raw, 6, 5) | F(raw, 28, 2) << 5); }
uint8_t Va128(uint32_t raw) { return uint8_t(F(raw, 11, 5) | F(raw, 26, 1) << 5 | F(raw, 21, 1) << 6); }
uint8_t Vb128(uint32_t raw) { return uint8_t(F(raw, 16, 5) | F(raw, 30, 2) << 5); }

// A VMX128 operation as the VMX one it is (vA, vB, vC as VMX names them).
void Vmx128(Insn& i, VmxOp op, uint8_t a, uint8_t b, uint8_t c = 0, int64_t imm = 0) {
  i.op = Op::kVmx;
  i.opc = op;
  i.rd = Vd128(i.raw);
  i.ra = a;
  i.rb = b;
  i.rc_reg = c;
  i.imm = imm;
}

// Loads and stores (the low two bits 11, bits 21-27 the extended opcode):
// VMX's, and the Xenon's LVLX family, all with their LRU-hint forms.
void Vmx128Memory(Insn& i) {
  static const struct { uint8_t xo; bool store; uint8_t kind, size; } kMem[] = {
      {0, false, kVecShiftLeft, 0}, {4, false, kVecShiftRight, 0}, {8, false, kVecElement, 4},
      {12, false, kVecQuad, 16}, {24, true, kVecElement, 4}, {28, true, kVecQuad, 16},
      {44, false, kVecQuad, 16}, {60, true, kVecQuad, 16},
      {64, false, kVecLeft, 16}, {68, false, kVecRight, 16}, {80, true, kVecLeft, 16},
      {84, true, kVecRight, 16}, {96, false, kVecLeft, 16}, {100, false, kVecRight, 16},
      {112, true, kVecLeft, 16}, {116, true, kVecRight, 16}};
  for (const auto& m : kMem)
    if (F(i.raw, 21, 7) == m.xo) {
      MemX(i, m.store ? Op::kVstore : Op::kVload, m.size, false);
      i.opc = m.kind;
      i.rd = Vd128(i.raw);
      return;
    }
}

// Primary opcode 4: VMX, and VMX128's loads, stores and VSLDOI128 (bit 27;
// no VMX encoding has it).
void Primary4(Insn& i) {
  const uint32_t raw = i.raw;
  i.rc = false;
  if (raw & 0x10) {
    Vmx128(i, kVsldoi, Va128(raw), Vb128(raw), 0, F(raw, 22, 4));
    return;
  }
  if (!(raw & 0x20) && (raw & 3) == 3) {
    Vmx128Memory(i);
    return;
  }
  i.rc_reg = uint8_t(F(raw, 21, 5));
  if (raw & 0x20) {
    for (const auto& v : kVa) {
      if (v.xo != (raw & 0x3F)) continue;
      if (v.op == kVsldoi) {
        if (F(raw, 21, 1)) return;
        i.imm = F(raw, 22, 4);
        i.rc_reg = 0;
      }
      i.op = Op::kVmx;
      i.opc = v.op;
      return;
    }
    return;
  }
  i.rc_reg = 0;
  if ((raw & 0x3F) == 6) {
    for (const auto& c : kVc)
      if ((raw & 0x3FF) == c.xo) {
        i.op = Op::kVcmp;
        i.opc = c.op;
        i.rc = raw & 0x400;
      }
    return;
  }
  const uint32_t xo = raw & 0x7FF;
  if (xo == 1540) {
    if (!i.ra && !i.rb) i.op = Op::kMfvscr;
    return;
  }
  if (xo == 1604) {
    if (!i.rd && !i.ra) i.op = Op::kMtvscr;
    return;
  }
  for (const auto& v : kVx) {
    if (v.xo != xo) continue;
    if (v.form == kOne && i.ra) return;
    if (v.form == kSimm && i.rb) return;
    if (v.form == kUimm || v.form == kSimm) i.imm = i.ra;
    i.op = Op::kVmx;
    i.opc = v.op;
    return;
  }
}

// Primary opcode 5, VMX128: bits 22-25 and 27 select the operation; with
// bit 27 clear and bit 22 clear, VPERM128 (vC in bits 23-25, v0-v7).
void Primary5(Insn& i) {
  const uint32_t raw = i.raw;
  const uint8_t a = Va128(raw), b = Vb128(raw), d = Vd128(raw);
  const uint32_t x = F(raw, 22, 4);
  if (!F(raw, 27, 1)) {
    static const VmxOp kPack[8] = {kVpkshss, kVpkshus, kVpkswss, kVpkswus,
                                   kVpkuhum, kVpkuhus, kVpkuwum, kVpkuwus};
    if (x < 8) Vmx128(i, kVperm, a, b, uint8_t(x));
    else Vmx128(i, kPack[x - 8], a, b);
    return;
  }
  static const VmxOp kOps[16] = {kVaddfp, kVsubfp, kVmulfp128, kVmaddfp, kVmaddfp, kVnmsubfp,
                                 kVmsum3fp128, kVmsum4fp128, kVand, kVandc, kVnor, kVor, kVxor,
                                 kVsel, kVslo, kVsro};
  switch (x) {
    // The multiply-adds name vD as a source: vD = vA * vB + vD (VMADDFP128),
    // vA * vD + vB (VMADDCFP128), -(vA * vB - vD) (VNMSUBFP128); VSEL128
    // takes its mask from vD.
    case 3: case 5: Vmx128(i, kOps[x], a, d, b); return;
    case 4: case 13: Vmx128(i, kOps[x], a, b, d); return;
    default: Vmx128(i, kOps[x], a, b); return;
  }
}

// Primary opcode 6, VMX128: compares (Rc in bit 25), conversions, splats,
// estimates and roundings (vB and a 5-bit immediate in bits 11-15),
// VPERMWI128, VRLIMI128, the D3D formats, merges, shifts and unpacks.
void Primary6(Insn& i) {
  const uint32_t raw = i.raw;
  const uint8_t a = Va128(raw), b = Vb128(raw);
  const uint32_t imm = F(raw, 11, 5);
  if (F(raw, 21, 2) == 1 && F(raw, 26, 2) == 1) {  // the permute's high bits in 23-25
    Vmx128(i, kVpermwi128, 0, b, 0, imm | F(raw, 23, 3) << 5);
    return;
  }
  if (F(raw, 21, 2) == 3 && F(raw, 26, 2) == 1) {
    // VRLIMI128 (bit 23 set): the word mask (bit 3: element 0) and the
    // rotate (bits 24-25). VPKD3D128: the type and the size, then where.
    const uint32_t z = F(raw, 24, 2);
    if (F(raw, 23, 1)) {
      if (!(imm & 16)) Vmx128(i, kVrlimi128, 0, b, Vd128(raw), imm | z << 4);
    } else if (imm >> 2 != 7 && (imm & 3)) {
      Vmx128(i, kVpkd3d128, 0, b, Vd128(raw), (imm >> 2) << 4 | (imm & 3) << 2 | z);
    }
    return;
  }
  // (Bits 26-27 both set; the shifts below have bit 26 for vA.)
  if (F(raw, 26, 2) == 3) {
    static const struct { uint8_t xo; VmxOp op; uint8_t form; } kOne128[] = {
        {0x23, kVctsxs, kUimm}, {0x27, kVctuxs, kUimm}, {0x2B, kVcfsx, kUimm}, {0x2F, kVcfux, kUimm},
        {0x33, kVrfim, kOne}, {0x37, kVrfin, kOne}, {0x3B, kVrfip, kOne}, {0x3F, kVrfiz, kOne},
        {0x63, kVrefp, kOne}, {0x67, kVrsqrtefp, kOne}, {0x6B, kVexptefp, kOne}, {0x6F, kVlogefp, kOne},
        {0x73, kVspltw, kUimm}, {0x77, kVspltisw, kSimm}, {0x7F, kVupkd3d128, kUimm}};
    for (const auto& v : kOne128) {
      if (v.xo != F(raw, 21, 7)) continue;
      if ((v.form == kOne && imm) || (v.form == kSimm && b)) return;
      if (v.op == kVupkd3d128) {  // the type in bits 11-13
        if (imm >> 2 == 7 || (imm & 3)) return;
        Vmx128(i, v.op, 0, b, 0, (imm >> 2) << 4);
        return;
      }
      Vmx128(i, v.op, 0, v.form == kSimm ? 0 : b, 0, v.form == kOne ? 0 : imm);
      return;
    }
  }
  if (!F(raw, 27, 1) && F(raw, 22, 3) <= 4) {
    static const VmxOp kCmp128[5] = {kVcmpeqfp, kVcmpgefp, kVcmpgtfp, kVcmpbfp, kVcmpequw};
    Vmx128(i, kCmp128[F(raw, 22, 3)], a, b);
    i.op = Op::kVcmp;
    i.rc = F(raw, 25, 1);
    return;
  }
  const uint32_t x = F(raw, 22, 4);
  if (F(raw, 27, 1)) {
    switch (x) {
      case 1: Vmx128(i, kVrlw, a, b); return;
      case 3: Vmx128(i, kVslw, a, b); return;
      case 5: Vmx128(i, kVsraw, a, b); return;
      case 7: Vmx128(i, kVsrw, a, b); return;
      default: return;
    }
  }
  switch (x) {
    case 10: Vmx128(i, kVmaxfp, a, b); return;
    case 11: Vmx128(i, kVminfp, a, b); return;
    case 12: Vmx128(i, kVmrghw, a, b); return;
    case 13: Vmx128(i, kVmrglw, a, b); return;
    // VUPKHSB128 and VUPKLSB128; vA 0x60 makes them the halfword unpacks.
    case 14: case 15:
      if (a == 0) Vmx128(i, x == 14 ? kVupkhsb : kVupklsb, 0, b);
      if (a == 0x60) Vmx128(i, x == 14 ? kVupkhsh : kVupklsh, 0, b);
      return;
    default: return;
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
    case 4: Primary4(i); break;
    case 5: Primary5(i); break;
    case 6: Primary6(i); break;
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
    case 48: Mem(i, Op::kFpLoad, 4, false); break;
    case 49: Mem(i, Op::kFpLoad, 4, true); break;
    case 50: Mem(i, Op::kFpLoad, 8, false); break;
    case 51: Mem(i, Op::kFpLoad, 8, true); break;
    case 52: Mem(i, Op::kFpStore, 4, false); break;
    case 53: Mem(i, Op::kFpStore, 4, true); break;
    case 54: Mem(i, Op::kFpStore, 8, false); break;
    case 55: Mem(i, Op::kFpStore, 8, true); break;
    case 59: PrimaryFp(i, true); break;
    case 63: PrimaryFp(i, false); break;
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
  // (integer loads) the target register.
  if ((i.op == Op::kLoad || i.op == Op::kStore || i.op == Op::kFpLoad || i.op == Op::kFpStore) &&
      i.update && (i.ra == 0 || (i.op == Op::kLoad && i.ra == i.rd)))
    i.op = Op::kInvalid;
  if (i.op == Op::kLmw && i.ra >= i.rd && i.ra != 0) i.op = Op::kInvalid;
  return i;
}

bool IsVmx128(uint32_t raw) {
  uint32_t primary = raw >> 26;
  return primary == 5 || primary == 6 ||
         (primary == 4 && ((raw & 0x10) || (!(raw & 0x20) && (raw & 3) == 3)));
}

uint32_t VmxEncoding(const Insn& i) {
  if (i.rd > 31 || i.ra > 31 || i.rb > 31 || i.rc_reg > 31) return 0;
  const uint32_t regs = uint32_t(i.rd) << 21 | uint32_t(i.ra) << 16 | uint32_t(i.rb) << 11;
  switch (i.op) {
    case Op::kVload: case Op::kVstore: {
      bool load = i.op == Op::kVload;
      uint32_t xo = 0;
      switch (i.opc) {
        case kVecQuad: xo = load ? 103 : 231; break;
        case kVecElement: xo = (load ? 7 : 135) + 32 * (i.size == 2 ? 1 : i.size == 4 ? 2 : 0); break;
        case kVecShiftLeft: xo = 6; break;
        case kVecShiftRight: xo = 38; break;
        case kVecLeft: xo = load ? 519 : 647; break;
        default: xo = load ? 551 : 679; break;
      }
      return 31u << 26 | regs | xo << 1;
    }
    case Op::kVcmp:
      for (const auto& c : kVc)
        if (c.op == i.opc) return 4u << 26 | regs | (i.rc ? 0x400u : 0u) | c.xo;
      return 0;
    case Op::kVmx:
      for (const auto& v : kVa)
        if (v.op == i.opc)
          return 4u << 26 | regs | uint32_t(v.op == kVsldoi ? i.imm : i.rc_reg) << 6 | v.xo;
      for (const auto& v : kVx) {
        if (v.op != i.opc) continue;
        if (v.form == kUimm || v.form == kSimm)
          return 4u << 26 | uint32_t(i.rd) << 21 | uint32_t(i.imm & 31) << 16 | uint32_t(i.rb) << 11 | v.xo;
        return 4u << 26 | regs | v.xo;
      }
      return 0;
    default:
      return 0;
  }
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
    case Op::kFpLoad: case Op::kFpStore: {
      std::string s = std::string(i.op == Op::kFpLoad ? "lf" : "stf") + (i.size == 8 ? "d" : "s");
      if (i.op == Op::kFpStore && i.opc == 1) return "stfiwx";
      if (i.update) s += "u";
      if (i.indexed) s += "x";
      return s;
    }
    case Op::kFpArith: {
      static const char* kNames[] = {"fadd", "fsub", "fmul", "fdiv", "fsqrt", "fres", "frsqrte",
                                     "fmadd", "fmsub", "fnmadd", "fnmsub"};
      return std::string(kNames[i.opc]) + (i.single ? "s" : "");
    }
    case Op::kFpMove: {
      static const char* kNames[] = {"fmr", "fneg", "fabs", "fnabs"};
      return kNames[i.opc & 3];
    }
    case Op::kFsel: return "fsel";
    case Op::kFcmp: return i.opc ? "fcmpo" : "fcmpu";
    case Op::kFrsp: return "frsp";
    case Op::kFctiw: {
      static const char* kNames[] = {"fctiw", "fctiwz", "fctid", "fctidz"};
      return kNames[i.opc & 3];
    }
    case Op::kFcfid: return "fcfid";
    case Op::kMffs: return "mffs";
    case Op::kMtfsf: return "mtfsf";
    case Op::kMtfsfi: return "mtfsfi";
    case Op::kMtfsb: return i.opc ? "mtfsb1" : "mtfsb0";
    case Op::kMcrfs: return "mcrfs";
    case Op::kVmx: case Op::kVcmp: {
      char b[32];
      std::snprintf(b, sizeof b, "vmx op %u%s", unsigned(i.opc), i.rc ? "." : "");
      return b;
    }
    case Op::kVload: case Op::kVstore: {
      static const char* kLoads[] = {"lvx", "lve*x", "lvsl", "lvsr", "lvlx", "lvrx"};
      static const char* kStores[] = {"stvx", "stve*x", "?", "?", "stvlx", "stvrx"};
      return (i.op == Op::kVload ? kLoads : kStores)[i.opc % 6];
    }
    case Op::kMfvscr: return "mfvscr";
    case Op::kMtvscr: return "mtvscr";
    case Op::kLarx: return i.size == 8 ? "ldarx" : "lwarx";
    case Op::kStcx: return i.size == 8 ? "stdcx." : "stwcx.";
  }
  return "?";
}

}  // namespace lifter::ppc
