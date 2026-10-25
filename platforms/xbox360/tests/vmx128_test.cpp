// VMX128 where the form test's oracle cannot reach (it has only VMX: its
// twins of VMX128 forms are re-encoded from the decoder's own reading, and
// run on registers below 32).
// - Decoding: words with high registers, as Xenia's (BSD) opcode table and
//   instruction fields encode them, against the operation and operands
//   each one's definition gives (the multiply-adds and VSEL128 read vD).
// - The operations of VMX128's own without a VMX twin, the dot products
//   and the D3D formats, against vectors from Xenia's tests
//   (src/xenia/cpu/testing/pack_test.cc, unpack_test.cc and
//   ppc/testing/instr_*128.s) and a few of the same rules' edges.
// Built with planted bugs (RECOMP_PPC_MUTATION) that it must catch too.
//   xbox360_vmx128_test
//
// The test vectors from Xenia: Copyright (c) 2015, Ben Vanik. All rights
// reserved. BSD 3-Clause; the licence is in NOTICE.

#include <cstdint>
#include <cstdio>

#include "ppc.h"
#include "ppc/lifted.h"

namespace {

using lifter::ppc::Insn;
using lifter::ppc::Op;
using namespace recomp::ppc;

int failures = 0;

void Check(bool ok, const char* what, uint32_t raw) {
  if (ok) return;
  std::printf("FAIL %08x: %s\n", raw, what);
  failures++;
}

struct DecodeCase {
  uint32_t raw;
  const char* text;
  Op op;
  unsigned opc;
  int rd, ra, rb, rc_reg;
  int64_t imm;
  bool rc;
};

// Operands as the lifted operation reads them: Vmx(op, vA, vB, vC, imm).
const DecodeCase kDecode[] = {
    {0x130428CF, "lvx128 v120, r4, r5", Op::kVload, lifter::ppc::kVecQuad, 120, 4, 5, 0, 0, false},
    {0x11A6398B, "stvewx128 v77, r6, r7", Op::kVstore, lifter::ppc::kVecElement, 77, 6, 7, 0, 0, false},
    {0x10284E47, "lvrxl128 v33, r8, r9", Op::kVload, lifter::ppc::kVecRight, 33, 8, 9, 0, 0, false},
    {0x1081FE5F, "vsldoi128 v100, v65, v127, 9", Op::kVmx, kVsldoi, 100, 65, 127, 0, 9, false},
    {0x17E005CF, "vperm128 v127, v64, v96, v7", Op::kVmx, kVperm, 127, 64, 96, 7, 0, false},
    {0x1481FC1F, "vaddfp128 v100, v65, v127", Op::kVmx, kVaddfp, 100, 65, 127, 0, 0, false},
    {0x143E14B2, "vmulfp128 v1, v126, v66", Op::kVmx, kVmulfp128, 1, 126, 66, 0, 0, false},
    // vD = vA * vB + vD: VMADDFP's vA * vC + vB.
    {0x140100FE, "vmaddfp128 v96, v33, v64", Op::kVmx, kVmaddfp, 96, 33, 96, 64, 0, false},
    // vD = vA * vD + vB.
    {0x1401013E, "vmaddcfp128 v96, v33, v64", Op::kVmx, kVmaddfp, 96, 33, 64, 96, 0, false},
    // vD = -(vA * vB - vD): VNMSUBFP's -(vA * vC - vB).
    {0x1653A175, "vnmsubfp128 v50, v51, v52", Op::kVmx, kVnmsubfp, 50, 51, 50, 52, 0, false},
    // vD = vA where vD's bit is 0, vB where 1.
    {0x1611975A, "vsel128 v80, v81, v82", Op::kVmx, kVsel, 80, 81, 82, 80, 0, false},
    {0x14642DF3, "vmsum4fp128 v3, v100, v101", Op::kVmx, kVmsum4fp128, 3, 100, 101, 0, 0, false},
    {0x14C746CA, "vpkswus128 v70, v71, v72", Op::kVmx, kVpkswus, 70, 71, 72, 0, 0, false},
    {0x175BE7DA, "vsro128 v90, v91, v92", Op::kVmx, kVsro, 90, 91, 92, 0, 0, false},
    {0x181F05E9, "vcmpbfp128. v64, v127, v32", Op::kVcmp, kVcmpbfp, 64, 127, 32, 0, 0, true},
    {0x18221A03, "vcmpequw128 v1, v2, v99", Op::kVcmp, kVcmpequw, 1, 2, 99, 0, 0, false},
    {0x19095577, "vsraw128 v40, v105, v106", Op::kVmx, kVsraw, 40, 105, 106, 0, 0, false},
    {0x1B19D76F, "vmrglw128 v120, v121, v122", Op::kVmx, kVmrglw, 120, 121, 122, 0, 0, false},
    {0x18C0D3CA, "vupklsb128 v70, v90", Op::kVmx, kVupklsb, 70, 0, 90, 0, 0, false},
    // vA = 0x60: the halfword unpack.
    {0x18C0D7AA, "vupkhsb128 v70, v90 (vA 0x60)", Op::kVmx, kVupkhsh, 70, 0, 90, 0, 0, false},
    {0x19A9727A, "vcfpuxws128 v77, v78, 9", Op::kVmx, kVctuxs, 77, 0, 78, 0, 9, false},
    {0x189F0ABC, "vcsxwfp128 v100, v1, 31", Op::kVmx, kVcfsx, 100, 0, 1, 0, 31, false},
    {0x1870077C, "vspltisw128 v99, -16", Op::kVmx, kVspltisw, 99, 0, 0, 0, 16, false},
    {0x18431F3A, "vspltw128 v66, v67, 3", Op::kVmx, kVspltw, 66, 0, 67, 0, 3, false},
    {0x1BE0067B, "vrsqrtefp128 v95, v96", Op::kVmx, kVrsqrtefp, 95, 0, 96, 0, 0, false},
    {0x1873231F, "vpermwi128 v99, v100, 0x93", Op::kVmx, kVpermwi128, 99, 0, 100, 0, 0x93, false},
    // The mask, then the rotate in bits 4-5; vD is merged into.
    {0x186A479D, "vrlimi128 v99, v40, 0xA, 2", Op::kVmx, kVrlimi128, 99, 0, 40, 99, 0x2A, false},
    // The type, the size, where.
    {0x183616DA, "vpkd3d128 v65, v66, 5, 2, 3", Op::kVmx, kVpkd3d128, 65, 0, 66, 65, 0x5B, false},
    {0x183817FA, "vupkd3d128 v65, v66, 24", Op::kVmx, kVupkd3d128, 65, 0, 66, 0, 0x60, false},
};

// No operation: a size of 0, type 7, VRLIMI128's mask bit 4, an unpack's
// vA other than 0 or 0x60, an estimate's unused immediate.
const uint32_t kInvalid[] = {0x18201610, 0x183D1610, 0x183C17F0, 0x183F1710, 0x18211380, 0x18211630};

Vec V(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  Vec v;
  lift::VPut<uint32_t>(v, 0, a);
  lift::VPut<uint32_t>(v, 1, b);
  lift::VPut<uint32_t>(v, 2, c);
  lift::VPut<uint32_t>(v, 3, d);
  return v;
}

void Expect(const char* what, const Vec& got, const Vec& want) {
  bool same = true;
  for (int k = 0; k < 4; k++) same &= lift::VGet<uint32_t>(got, k) == lift::VGet<uint32_t>(want, k);
  if (same) return;
  std::printf("FAIL %s: got", what);
  for (int k = 0; k < 4; k++) std::printf(" %08x", lift::VGet<uint32_t>(got, k));
  std::printf(", want");
  for (int k = 0; k < 4; k++) std::printf(" %08x", lift::VGet<uint32_t>(want, k));
  std::printf("\n");
  failures++;
}

// vD = VPKD3D128 of b into d.
Vec Pack(unsigned type, unsigned size, unsigned where, const Vec& b, const Vec& d) {
  Ctx c{};
  return lift::Vmx(c, kVpkd3d128, Vec{}, b, d, type << 4 | size << 2 | where);
}
Vec Unpack(unsigned type, const Vec& b) {
  Ctx c{};
  return lift::Vmx(c, kVupkd3d128, Vec{}, b, Vec{}, type << 4);
}
Vec Dot(bool four, const Vec& a, const Vec& b, bool nj = true) {
  Ctx c{};
  c.vscr = nj ? lift::kVscrNj : 0;
  return lift::Vmx(c, four ? kVmsum4fp128 : kVmsum3fp128, a, b, Vec{}, 0);
}

}  // namespace

int main() {
  for (const DecodeCase& t : kDecode) {
    Insn i = lifter::ppc::Decode(0x82000000, t.raw);
    Check(lifter::ppc::IsVmx128(t.raw), "not VMX128", t.raw);
    bool ok = i.op == t.op && i.opc == t.opc && i.rd == t.rd && i.ra == t.ra && i.rb == t.rb && i.rc == t.rc;
    if (t.op == Op::kVmx || t.op == Op::kVcmp) ok = ok && i.rc_reg == t.rc_reg && i.imm == t.imm;
    if (!ok)
      std::printf("     decoded op %d opc %u vD %d vA %d vB %d vC %d imm %lld rc %d\n", int(i.op), unsigned(i.opc), i.rd,
                  i.ra, i.rb, i.rc_reg, static_cast<long long>(i.imm), int(i.rc));
    Check(ok, t.text, t.raw);
  }
  for (uint32_t raw : kInvalid) Check(lifter::ppc::Decode(0x82000000, raw).op == Op::kInvalid, "decodes", raw);

  const Vec cd = V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD);
  const Vec bytes = V(0x40400001, 0x40400002, 0x40400003, 0x40400004);
  // instr_vpkd3d128.s: D3DCOLOR (ARGB from XYZW), every size and place.
  Expect("pack d3dcolor 1 0", Pack(0, 1, 0, bytes, cd), V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x04010203));
  Expect("pack d3dcolor 1 1", Pack(0, 1, 1, bytes, cd), V(0xCDCDCDCD, 0xCDCDCDCD, 0x04010203, 0xCDCDCDCD));
  Expect("pack d3dcolor 1 3", Pack(0, 1, 3, bytes, cd), V(0x04010203, 0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD));
  Expect("pack d3dcolor 2 0", Pack(0, 2, 0, bytes, cd), V(0xCDCDCDCD, 0xCDCDCDCD, 0, 0x04010203));
  Expect("pack d3dcolor 2 1", Pack(0, 2, 1, bytes, cd), V(0xCDCDCDCD, 0, 0x04010203, 0xCDCDCDCD));
  Expect("pack d3dcolor 2 2", Pack(0, 2, 2, bytes, cd), V(0, 0x04010203, 0xCDCDCDCD, 0xCDCDCDCD));
  Expect("pack d3dcolor 2 3", Pack(0, 2, 3, bytes, cd), V(0x04010203, 0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD));
  Expect("pack d3dcolor 3 2", Pack(0, 3, 2, bytes, cd), V(0, 0x04010203, 0xCDCDCDCD, 0xCDCDCDCD));
  Expect("pack d3dcolor 3 3", Pack(0, 3, 3, bytes, cd), V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0));
  // Out of range, and a NaN (its bottom: 0).
  Expect("pack d3dcolor clamps", Pack(0, 1, 0, V(0x40800000, 0x40000000, 0xC2F60000, 0x4B3BDF83), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0xFFFF0000));
  Expect("pack d3dcolor nan", Pack(0, 1, 0, V(0x40800000, 0x40000000, 0xC2F60000, 0xFFFFFFFF), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x00FF0000));
  Expect("pack short2", Pack(1, 1, 0, V(0x40407FFF, 0x403F8001, 0, 0), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x7FFF8001));
  Expect("pack short2 clamps", Pack(1, 1, 0, V(0x43817E00, 0xC37CFC00, 0x42A23EC8, 0x403DB757), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x7FFF8001));
  Expect("pack short2 halves", Pack(1, 1, 0, V(0x40404000, 0x403FC000, 0x40400003, 0x403F8001), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x4000C000));
  Expect("pack short4", Pack(4, 2, 0, V(0x403F8001, 0x403FFFF8, 0x4040007F, 0x40400000), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0x8001FFF8, 0x007F0000));
  Expect("pack 2_10_10_10", Pack(2, 1, 0, V(0xB8FF8000, 0xB8FF8000, 0xC04001FF, 0x4E9A5A5A), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0xE0180601));
  Expect("pack 2_10_10_10 clamps", Pack(2, 1, 0, V(0x42C80000, 0xC2C80000, 0x40400000, 0x3F800000), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x000805FF));
  Expect("pack 4_20_20_20", Pack(6, 2, 0, V(0x40380001, 0x4047FFFF, 0x40400000, 0x4040000F), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xF000007F, 0xFFF80001));
  Expect("pack float16_2", Pack(3, 1, 0, V(0x3F000000, 0xBF000000, 0, 0), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x3800B800));
  Expect("pack float16_2 largest", Pack(3, 1, 0, V(0x47FFE000, 0xC7FFE000, 0, 0x3F800000), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x7FFFFFFF));
  Expect("pack float16_4", Pack(5, 2, 0, V(0x3F000000, 0xBF000000, 0x3F800000, 0xBF800000), cd),
         V(0xCDCDCDCD, 0xCDCDCDCD, 0x3800B800, 0x3C00BC00));
  // pack_test.cc: rounding to nearest even (0x45B17000 is a tie).
  Expect("pack float16_4 rounds", Pack(5, 2, 0, V(0x449A4000, 0x45B17000, 0x41103261, 0x40922B6B), Vec{}),
         V(0, 0, 0x64D26D8C, 0x48824491));

  // unpack_test.cc, and each format's most negative value.
  Expect("unpack d3dcolor", Unpack(0, V(0, 0, 0, 0x80506070)), V(0x3F800050, 0x3F800060, 0x3F800070, 0x3F800080));
  Expect("unpack d3dcolor 0", Unpack(0, V(0, 0, 0, 0)), V(0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000));
  Expect("unpack short2", Unpack(1, V(0x7004FD60, 0x8201C990, 0, 0x7FFF8001)),
         V(0x40407FFF, 0x403F8001, 0, 0x3F800000));
  Expect("unpack short2 b", Unpack(1, V(0, 0, 0, 0x12345678)), V(0x40401234, 0x40405678, 0, 0x3F800000));
  Expect("unpack short4", Unpack(4, V(0, 0, 0x80007FFF, 0x0001FFFF)),
         V(0x7FC00000, 0x40407FFF, 0x40400001, 0x403FFFFF));
  Expect("unpack 2_10_10_10", Unpack(2, V(0, 0, 0, 0xFFF7FE00)), V(0x7FC00000, 0x404001FF, 0x403FFFFF, 0x3F800003));
  Expect("unpack 4_20_20_20", Unpack(6, V(0, 0, 0xF000007F, 0xFFF80001)),
         V(0x40380001, 0x4047FFFF, 0x40400000, 0x3F80000F));
  Expect("unpack 4_20_20_20 bottom", Unpack(6, V(0, 0, 0, 0x00080000)),
         V(0x7FC00000, 0x40400000, 0x40400000, 0x3F800000));
  Expect("unpack float16_2", Unpack(3, V(0, 0, 0, 0x7FFFFFFF)), V(0x47FFE000, 0xC7FFE000, 0, 0x3F800000));
  Expect("unpack float16_2 b", Unpack(3, V(0, 0, 0, 0x55556666)), V(0x42AAA000, 0x44CCC000, 0, 0x3F800000));
  Expect("unpack float16_4", Unpack(5, V(0, 0, 0x64D26D8B, 0x48814491)),
         V(0x449A4000, 0x45B16000, 0x41102000, 0x40922000));
  Expect("unpack float16 denormal", Unpack(5, V(0, 0, 0x000183FF, 0)), V(0, 0x80000000, 0, 0));

  // instr_vmsum3fp128.s, instr_vmsum4fp128.s; denormal results flushed
  // (VSCR[NJ]), overflow to the default NaN, an infinite operand.
  Expect("vmsum3fp128", Dot(false, V(0x3F800000, 0x3FC00000, 0x3F8CCCCD, 0x01020304),
                             V(0x40000000, 0x40700000, 0x4013D70A, 0x01020304)),
         V(0x4122A7F0, 0x4122A7F0, 0x4122A7F0, 0x4122A7F0));
  Expect("vmsum4fp128", Dot(true, V(0x3F800000, 0x3FC00000, 0x3F8CCCCD, 0x3FF33333),
                            V(0x40000000, 0x40700000, 0x4013D70A, 0x40B051EB)),
         V(0x41A5147B, 0x41A5147B, 0x41A5147B, 0x41A5147B));
  Expect("vmsum3fp128 flush", Dot(false, V(0x80800000, 0, 0, 0), V(0x3F000000, 0, 0, 0)),
         V(0x80000000, 0x80000000, 0x80000000, 0x80000000));
  Expect("vmsum4fp128 denormal", Dot(true, V(0x00800000, 0, 0, 0), V(0x3F000000, 0, 0, 0), false),
         V(0x00400000, 0x00400000, 0x00400000, 0x00400000));
  Expect("vmsum4fp128 overflow", Dot(true, V(0x7F000000, 0x7F000000, 0, 0), V(0x40000000, 0x40000000, 0, 0)),
         V(0x7FC00000, 0x7FC00000, 0x7FC00000, 0x7FC00000));
  Expect("vmsum3fp128 infinity", Dot(false, V(0x7F800000, 0x3F800000, 0, 0), V(0x3F800000, 0x3F800000, 0, 0)),
         V(0x7F800000, 0x7F800000, 0x7F800000, 0x7F800000));
  Expect("vmsum3fp128 nan", Dot(false, V(0x3F800000, 0x7F800001, 0, 0), V(0xFFC00002, 0x3F800000, 0, 0)),
         V(0xFFC00002, 0xFFC00002, 0xFFC00002, 0xFFC00002));
  Expect("vmsum3fp128 invalid", Dot(false, V(0x7F800000, 0, 0, 0), V(0, 0, 0, 0)),
         V(0x7FC00000, 0x7FC00000, 0x7FC00000, 0x7FC00000));

  if (failures) std::printf("%d failures\n", failures);
  else std::printf("all VMX128 checks passed\n");
  return failures ? 1 : 0;
}
