#pragma once
// GCN instructions as the PS4's GPU runs them: AMD's Sea Islands (GFX7)
// encodings, from AMD's "Sea Islands Series Instruction Set Architecture"
// reference. Decoding only.

#include <cstddef>
#include <cstdint>
#include <string>

namespace recomp::orbis::gpu::gcn {

enum class Fmt : uint8_t {
  Sop2, Sopk, Sop1, Sopc, Sopp, Smrd, Vop2, Vop1, Vopc, Vop3, Vintrp, Ds, Mubuf, Mtbuf, Mimg,
  Exp,
};

// One decoded instruction: its encoding's words and, if it has one, the
// literal constant that follows them. VOP3 holds VOPC, VOP2 and VOP1
// opcodes too (0-255, 256 + VOP2's, 384 + VOP1's, the VOP3-only ones
// 320-383).
struct Instr {
  Fmt fmt;
  uint16_t op;
  uint8_t dwords;  // with the literal
  bool literal_used;
  uint32_t w0, w1, literal;
};

// Decodes the instruction at code (avail dwords readable); false if the
// words are no GFX7 encoding or the instruction runs past avail.
bool Decode(const uint32_t* code, size_t avail, Instr* out);

// The instruction's mnemonic, or null for an opcode GFX7 does not define.
const char* Name(const Instr& i);

// For logs: the mnemonic (or the format and opcode number) and the raw
// operand fields.
std::string Disassemble(const Instr& i);

// Operand codes of the 9-bit source fields (8-bit for scalar ones).
enum Operand : uint32_t {
  kVccLo = 106, kVccHi = 107, kM0 = 124, kExecLo = 126, kExecHi = 127, kZero = 128,
  kIntPos = 129,  // 129-192: 1-64
  kIntNeg = 193,  // 193-208: -1 to -16
  kFloatHalf = 240,  // 240-247: 0.5, -0.5, 1, -1, 2, -2, 4, -4
  kVccz = 251, kExecz = 252, kScc = 253, kLdsDirect = 254, kLiteral = 255, kVgpr = 256,
};

}  // namespace recomp::orbis::gpu::gcn
