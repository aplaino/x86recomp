// orbis/gpu/gcn's decoder against hand-assembled GFX7 encodings: each
// format's identification, opcode, size and literal constants.

#include <cstdio>
#include <cstring>

#include "orbis/gpu/gcn.h"

namespace {

using namespace recomp::orbis::gpu::gcn;

int failures = 0;

void Expect(std::initializer_list<uint32_t> words, Fmt fmt, uint32_t op, uint32_t dwords,
            const char* name) {
  const uint32_t* w = words.begin();
  Instr i;
  if (!Decode(w, words.size(), &i)) {
    std::printf("FAIL: %08X does not decode\n", w[0]);
    failures++;
    return;
  }
  const char* n = Name(i);
  if (i.fmt != fmt || i.op != op || i.dwords != dwords || !n || std::strcmp(n, name) != 0) {
    std::printf("FAIL: %08X: %s (format %d op %u, %u dwords), want %s\n", w[0], n ? n : "?",
                int(i.fmt), i.op, i.dwords, name);
    failures++;
  }
}

void ExpectBad(std::initializer_list<uint32_t> words, const char* what) {
  Instr i;
  if (Decode(words.begin(), words.size(), &i)) {
    std::printf("FAIL: %s decodes\n", what);
    failures++;
  }
}

}  // namespace

int main() {
  // Scalar ALU and program control.
  Expect({0xBF810000}, Fmt::Sopp, 1, 1, "s_endpgm");
  Expect({0xBF8C0000}, Fmt::Sopp, 12, 1, "s_waitcnt");
  Expect({0xBE800301}, Fmt::Sop1, 3, 1, "s_mov_b32");
  Expect({0xBE8003FF, 0x3F800000}, Fmt::Sop1, 3, 2, "s_mov_b32");
  Expect({0xBE802100}, Fmt::Sop1, 33, 1, "s_swappc_b64");
  Expect({0x8000FF01, 0x12345678}, Fmt::Sop2, 0, 2, "s_add_u32");
  Expect({0xBF000100}, Fmt::Sopc, 0, 1, "s_cmp_eq_i32");
  Expect({0xB0001234}, Fmt::Sopk, 0, 1, "s_movk_i32");
  Expect({0xBA800000, 0xFFFF}, Fmt::Sopk, 21, 2, "s_setreg_imm32_b32");
  // Scalar memory: an immediate offset, and the literal offset (IMM 0,
  // OFFSET 255).
  Expect({0xC2800100}, Fmt::Smrd, 10, 1, "s_buffer_load_dwordx4");
  Expect({0xC08000FF, 0x400}, Fmt::Smrd, 2, 2, "s_load_dwordx4");
  // Vector ALU: VOP1 and VOPC words have bit 31 clear like VOP2's.
  Expect({0x7E000301}, Fmt::Vop1, 1, 1, "v_mov_b32");
  Expect({0x7E0002FF, 0x3F000000}, Fmt::Vop1, 1, 2, "v_mov_b32");
  Expect({0x7C080501}, Fmt::Vopc, 4, 1, "v_cmp_gt_f32");
  Expect({0x7D880501}, Fmt::Vopc, 0xC4, 1, "v_cmp_gt_u32");
  Expect({0x06000501}, Fmt::Vop2, 3, 1, "v_add_f32");
  Expect({0x40000501, 0x40000000}, Fmt::Vop2, 32, 2, "v_madmk_f32");
  Expect({0xD2820000, 0x04060501}, Fmt::Vop3, 321, 2, "v_mad_f32");
  Expect({0xD2060000, 0x00020501}, Fmt::Vop3, 259, 2, "v_add_f32");
  Expect({0xD3020000, 0x00000501}, Fmt::Vop3, 385, 2, "v_mov_b32");
  Expect({0xD0080000, 0x00020501}, Fmt::Vop3, 4, 2, "v_cmp_gt_f32");
  Expect({0xC8000000}, Fmt::Vintrp, 0, 1, "v_interp_p1_f32");
  Expect({0xC8010000}, Fmt::Vintrp, 1, 1, "v_interp_p2_f32");
  // Memory and export.
  Expect({0xD8DC0000, 0}, Fmt::Ds, 55, 2, "ds_read2_b32");
  Expect({0xE0700000, 0}, Fmt::Mubuf, 28, 2, "buffer_store_dword");
  Expect({0xE8030000, 0}, Fmt::Mtbuf, 3, 2, "tbuffer_load_format_xyzw");
  Expect({0xF0800000, 0}, Fmt::Mimg, 32, 2, "image_sample");
  Expect({0xF09C0000, 0}, Fmt::Mimg, 39, 2, "image_sample_lz");
  Expect({0xF800080F, 0x03020100}, Fmt::Exp, 0, 2, "exp");
  // A two-dword encoding cut short, a literal missing, and an unknown
  // encoding.
  ExpectBad({0xD2820000}, "a VOP3 word alone");
  ExpectBad({0x7E0002FF}, "a literal-using VOP1 word alone");
  ExpectBad({0xFC000000}, "encoding 0x3F");
  if (failures) return 1;
  std::printf("gcn decoder: ok\n");
  return 0;
}
