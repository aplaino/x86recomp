#pragma once
// AArch64 decoder for the lifter: ARMv8.0 A64 integer, memory, branch and
// system instructions (the Switch's Cortex-A57 without FP/SIMD so far),
// decoded from the encoding tables of the Arm Architecture Reference
// Manual (DDI 0487), section C4. Anything else decodes as kInvalid.

#include <cstdint>
#include <string>

namespace lifter::a64 {

enum class Op : uint8_t {
  kInvalid,
  kAdr,        // rd = imm (the target address)
  kAddImm,     // rd(sp) = rn(sp) +/- imm           [sub, setf]
  kAddShift,   // rd = rn +/- shift(rm, amount)     [sub, setf]
  kAddExt,     // rd(sp) = rn(sp) +/- extend(rm)    [sub, setf]
  kAdc,        // rd = rn +/- rm + C                [sub, setf]
  kLogImm,     // rd(sp) = rn OP imm                [opc, setf]
  kLogShift,   // rd = rn OP (~)shift(rm, amount)   [opc, inv, setf]
  kMovWide,    // MOVN/MOVZ/MOVK: imm16 << amount   [opc]
  kBitfield,   // SBFM/BFM/UBFM                     [opc, immr, imms]
  kExtr,       // rd = (rn:rm) >> imm
  kCondCmp,    // CCMN/CCMP, rm or imm              [sub, cond, nzcv, use_imm]
  kCondSel,    // CSEL/CSINC/CSINV/CSNEG            [opc, cond]
  kDp1,        // RBIT REV16 REV32 REV CLZ CLS      [opc]
  kDp2,        // UDIV SDIV LSLV LSRV ASRV RORV     [opc]
  kDp3,        // MADD MSUB SMADDL SMSUBL SMULH UMADDL UMSUBL UMULH [opc]
  kB,          // imm = target
  kBl,
  kBCond,      // [cond]
  kCbz,        // [opc: 0 CBZ, 1 CBNZ]
  kTbz,        // [opc: 0 TBZ, 1 TBNZ, amount = bit]
  kBr,
  kBlr,
  kRet,
  kSvc,        // imm
  kBrk,        // imm
  kHint,       // NOP and the rest of the hint space: no effect
  kBarrier,    // DMB DSB ISB
  kClrex,
  kMrs,        // rd = sysreg
  kMsr,        // sysreg = rd
  kLdSt,       // single register, every addressing mode     [mem]
  kLdStPair,   // LDP STP LDPSW LDNP STNP                     [mem]
  kLdLit,      // LDR LDRSW PRFM (literal): imm = address     [mem]
  kLdStExcl,   // LDXR LDAXR STXR STLXR: rm = status register [mem]
  kLdStOrder,  // LDAR STLR                                   [mem]
};

enum Shift : uint8_t { kLsl, kLsr, kAsr, kRor };
enum AddrMode : uint8_t { kOffset, kPreIndex, kPostIndex, kRegOffset };

// System registers by op0:op1:CRn:CRm:op2.
enum SysReg : uint16_t {
  kNzcv = (3 << 14) | (3 << 11) | (4 << 7) | (2 << 3) | 0,
  kTpidrEl0 = (3 << 14) | (3 << 11) | (13 << 7) | (0 << 3) | 2,
};

struct Insn {
  uint64_t va = 0;
  uint32_t raw = 0;
  Op op = Op::kInvalid;
  bool sf = false;       // 64-bit operation (or 64-bit destination of a load)
  bool setf = false;     // sets NZCV
  bool sub = false;      // subtracts (add/sub, adc/sbc, ccmn/ccmp)
  bool inv = false;      // inverts the second operand (BIC ORN EON BICS)
  bool use_imm = false;  // CCMP/CCMN immediate form
  uint8_t opc = 0;
  uint8_t rd = 0, rn = 0, rm = 0, ra = 0;  // memory: rd = Rt, ra = Rt2
  uint8_t shift = 0;     // Shift, or extend option for kAddExt/kRegOffset
  uint8_t amount = 0;    // shift amount; MOVZ hw*16; TBZ bit number
  uint8_t immr = 0, imms = 0;
  uint8_t cond = 0;
  uint8_t nzcv = 0;
  uint16_t sysreg = 0;
  int64_t imm = 0;
  // Memory accesses.
  uint8_t size = 0;      // bytes per register
  bool load = false;
  bool sign = false;     // sign-extends to the destination (sf says 32 or 64)
  bool prefetch = false; // PRFM: no access at all
  bool acquire = false, release = false;
  uint8_t mode = kOffset;
  // Bit positions of this encoding's register fields (Rd Rn Rm Ra Rt Rt2
  // Rs) whose value is not 31: the parts of the encoding the instruction
  // form test lets vary.
  uint32_t reg_fields = 0;
};

Insn Decode(uint64_t va, uint32_t raw);

// True for instructions after which execution does not fall through.
bool IsTerminator(const Insn& i);

// Short mnemonic for comments and messages (not a disassembler).
std::string Name(const Insn& i);

}  // namespace lifter::a64
