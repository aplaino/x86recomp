#pragma once
// PowerPC decoder for the lifter: the 64-bit PowerPC user instruction set
// the Xbox 360's Xenon runs (Power ISA 2.02 Book I/II), decoded from the
// instruction encodings in IBM's Power ISA. So far the fixed-point,
// branch, condition register, storage control and special purpose
// register instructions; floating point, VMX and VMX128 decode as
// kInvalid until they are lifted.

#include <cstdint>
#include <string>

namespace lifter::ppc {

enum class Op : uint8_t {
  kInvalid,
  // Arithmetic, D form: rd = ra|0 + imm (addis: imm << 16).
  kAddi,       // ADDI ADDIS                        [imm]
  kAddic,      // ADDIC ADDIC.: sets CA             [rc]
  kSubfic,     // rd = imm - ra, sets CA
  kMulli,
  // Arithmetic, XO form                            [oe, rc]
  kAdd, kAddc, kAdde, kAddme, kAddze,
  kSubf, kSubfc, kSubfe, kSubfme, kSubfze,
  kNeg,
  kMullw, kMulld, kMulhw, kMulhwu, kMulhd, kMulhdu,
  kDivw, kDivwu, kDivd, kDivdu,
  // Logical. Immediate forms: rd = ra OP imm (andi./andis. always set CR0).
  kAndi, kOri, kXori,                     // imm already shifted for the -is forms
  kAnd, kAndc, kOr, kOrc, kXor, kNand, kNor, kEqv,   // ra = rs OP rb   [rc]
  kExtsb, kExtsh, kExtsw, kCntlzw, kCntlzd,          // ra = f(rs)      [rc]
  // Rotates and shifts (ra = f(rs)).                        [rc]
  kRlwinm, kRlwimi, kRlwnm,   // [sh (rlwnm: rb), mb, me]
  kRldicl, kRldicr, kRldic, kRldimi, kRldcl, kRldcr,  // [sh, mb (or me)]
  kSlw, kSrw, kSraw, kSrawi, kSld, kSrd, kSrad, kSradi,
  // Compares: crfd = ra ? rb|imm. l: 64-bit compare.
  kCmp, kCmpl, kCmpi, kCmpli,
  // Condition register logic: crbd = crba OP crbb.  [opc: XL extended op]
  kCrLogic,
  kMcrf,       // crfd = crfs
  kMcrxr,      // crfd = XER[SO OV CA 0], clears them
  kMfcr,       // rd = CR (mfocrf: the whole CR too, as the ISA allows)
  kMtcrf,      // CR fields selected by crm = rs
  // Branches. b: imm = target. bc: bo, bi, imm = target.  [lk]
  kB, kBc, kBclr, kBcctr,
  // System.
  kSc,
  kTw, kTwi, kTd, kTdi,       // [to]
  kMfspr, kMtspr,             // [spr]
  kMftb,                      // [spr: 268 TBL, 269 TBU]
  kMfmsr, kMtmsr, kMtmsrd,
  kSync,                      // SYNC (opc 0), LWSYNC (opc 1), EIEIO (2), ISYNC (3)
  kCacheHint,                 // DCBT DCBTST DCBF DCBST ICBI: no architectural effect
  kDcbz,                      // size = 32 (DCBZ) or 128 (DCBZ128)
  // Loads and stores of one register.
  kLoad,       // [size, sign, update, indexed, byterev]
  kStore,      // [size, update, indexed, byterev]
  kLmw, kStmw,
  kLarx,       // LWARX LDARX   [size]
  kStcx,       // STWCX. STDCX. [size]
};

// Special purpose registers by number.
enum Spr : uint16_t {
  kXer = 1,
  kLr = 8,
  kCtr = 9,
  kVrsave = 256,
  kTbl = 268,
  kTbu = 269,
};

struct Insn {
  uint64_t va = 0;
  uint32_t raw = 0;
  Op op = Op::kInvalid;
  uint8_t rd = 0, ra = 0, rb = 0;  // rd is RT or RS
  bool rc = false;   // record form: sets CR0 (stwcx.: always)
  bool oe = false;   // sets XER[OV, SO]
  bool lk = false;   // branch and link
  bool l = false;    // compares: 64-bit; mtmsrd: L field
  uint8_t crfd = 0, crfs = 0;  // condition register fields
  uint8_t bo = 0, bi = 0;      // branch: also crbd (bo), crba (bi), crbb (rb) for kCrLogic
  uint8_t sh = 0, mb = 0, me = 0;
  uint8_t crm = 0;
  uint8_t to = 0;
  uint16_t opc = 0;
  uint16_t spr = 0;
  int64_t imm = 0;
  // Memory accesses.
  uint8_t size = 0;    // bytes
  bool sign = false;   // LHA LWA: sign-extends
  bool update = false; // writes the effective address back to ra
  bool indexed = false;// address ra|0 + rb instead of ra|0 + imm
  bool byterev = false;// LHBRX LWBRX LDBRX STHBRX STWBRX STDBRX
};

Insn Decode(uint64_t va, uint32_t raw);

// True for instructions after which execution does not fall through.
bool IsTerminator(const Insn& i);

// Short mnemonic for comments and messages (not a disassembler).
std::string Name(const Insn& i);

}  // namespace lifter::ppc
