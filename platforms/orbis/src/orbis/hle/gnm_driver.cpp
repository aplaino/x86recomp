// libSceGnmDriver HLE: the packets its functions write into the game's
// command buffers, exactly as libSceGnmDriver 11.00 writes them (the same
// dwords; a padding NOP's body is left as it was), submission to the
// command processor (orbis/gpu/cp.h), compute queues and the GPU's
// events. Checks the driver reports on stderr are reported the same way
// (WarnF) and return its error codes.

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "orbis/gpu/cp.h"
#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "orbis/hle/videoout.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::gnm {
namespace {

constexpr uint32_t kErrSubmit = 0x80D11000;
constexpr uint32_t kErrNoFlipPacket = 0x80D11080;
constexpr uint32_t kErrFlipQueueFull = 0x80D11081;
constexpr uint32_t kErrFlipIndex = 0x80D11082;

// Type 3 packet headers.
constexpr uint32_t Pm4(uint32_t op, uint32_t body, bool compute = false) {
  return 0xC0000000 | (body - 1) << 16 | op << 8 | (compute ? 2 : 0);
}
constexpr uint32_t kNop = 0x10, kSetShReg = 0x76, kSetContextReg = 0x69;
constexpr uint32_t kNopContextRegs = 0xC01E0000;  // gpu/cp.cpp

// Writes dwords at a guest address.
struct Out {
  gaddr p;
  void operator()(uint32_t v) {
    Wr32(p, v);
    p += 4;
  }
  void operator()(std::initializer_list<uint32_t> vs) {
    for (uint32_t v : vs) (*this)(v);
  }
};

// The NOP that pads what a function reserved: its header, and a zero
// first body dword; the rest of the body is not written.
void PadNop(Out& o, uint32_t body) {
  o(Pm4(kNop, body));
  o(0);
}

// SET_CONTEXT_REG of n values at reg, or (update) the NOP form the
// Update*Shader functions use.
void ContextRegs(Out& o, bool update, uint32_t reg, std::initializer_list<uint32_t> vs) {
  const uint32_t n = uint32_t(vs.size());
  if (update) {
    o({Pm4(kNop, n + 1), kNopContextRegs | reg});
  } else {
    o({Pm4(kSetContextReg, n + 1), reg});
  }
  o(vs);
}

// The shader registers' structs (sce::Gnm::*StageRegisters) are read as
// dwords; [1] is the program address's high bits, which must be 0.
uint32_t Reg(gaddr regs, int i) { return Rd32(regs + 4 * uint64_t(i)); }

bool CheckShader(const char* fn, gaddr regs) {
  if (!regs) {
    WarnF("sceGnm%s: the shader registers must not be null\n", fn);
    return false;
  }
  if (Reg(regs, 1)) {
    WarnF("sceGnm%s: the shader must be below 1 TiB\n", fn);
    return false;
  }
  return true;
}

// PGM_RSRC1 with a shader modifier (allowed bits: mask's complement).
uint32_t Modified(uint32_t rsrc1, uint32_t modifier, uint32_t keep) {
  return modifier ? (rsrc1 & keep) | modifier : rsrc1;
}

uint64_t SetVsShader(gaddr cmd, uint32_t dw, gaddr r, uint32_t modifier, bool update) {
  const char* fn = update ? "UpdateVsShader" : "SetVsShader";
  if (!cmd || dw < 29) return 0xFFFFFFFF;
  if (r && (modifier & 0xFCFFFC3F)) {
    WarnF("sceGnm%s: invalid shader modifier %08X\n", fn, modifier);
    return 0xFFFFFFFF;
  }
  if (!CheckShader(fn, r)) return 0xFFFFFFFF;
  Out o{cmd};
  o({Pm4(kSetShReg, 3), 0x48, Reg(r, 0), 0});
  o({Pm4(kSetShReg, 3), 0x4A, Modified(Reg(r, 2), modifier, 0xFCFFFC3F), Reg(r, 3)});
  ContextRegs(o, update, 0x207, {Reg(r, 6)});
  ContextRegs(o, update, 0x1B1, {Reg(r, 4)});
  ContextRegs(o, update, 0x1C3, {Reg(r, 5)});
  PadNop(o, 11);
  return 0;
}

uint64_t SetPsShader(gaddr cmd, uint32_t dw, gaddr r, bool update) {
  const char* fn = update ? "UpdatePsShader" : "SetPsShader";
  if (!cmd || dw < 40) return 0xFFFFFFFF;
  Out o{cmd};
  if (!r) {  // no pixel shader
    o({Pm4(kSetShReg, 3), 0x08, 0, 0});
    ContextRegs(o, update, 0x203, {0});
    PadNop(o, 32);
    return 0;
  }
  if (!CheckShader(fn, r)) return 0xFFFFFFFF;
  o({Pm4(kSetShReg, 3), 0x08, Reg(r, 0), 0});
  o({Pm4(kSetShReg, 3), 0x0A, Reg(r, 2), Reg(r, 3)});
  ContextRegs(o, update, 0x1C4, {Reg(r, 4), Reg(r, 5)});
  ContextRegs(o, update, 0x1B3, {Reg(r, 6), Reg(r, 7)});
  ContextRegs(o, update, 0x1B6, {Reg(r, 8)});
  ContextRegs(o, update, 0x1B8, {Reg(r, 9)});
  ContextRegs(o, update, 0x203, {Reg(r, 10)});
  ContextRegs(o, update, 0x08F, {Reg(r, 11)});
  PadNop(o, 11);
  return 0;
}

uint64_t SetGsShader(gaddr cmd, uint32_t dw, gaddr r, bool update) {
  if (!cmd || dw < 29) return 0xFFFFFFFF;
  if (!CheckShader(update ? "UpdateGsShader" : "SetGsShader", r)) return 0xFFFFFFFF;
  Out o{cmd};
  o({Pm4(kSetShReg, 3), 0x88, Reg(r, 0), 0});
  o({Pm4(kSetShReg, 3), 0x8A, Reg(r, 2), Reg(r, 3)});
  ContextRegs(o, update, 0x2E5, {Reg(r, 4)});
  ContextRegs(o, update, 0x29B, {Reg(r, 5)});
  ContextRegs(o, update, 0x2E4, {Reg(r, 6)});
  PadNop(o, 11);
  return 0;
}

uint64_t SetHsShader(gaddr cmd, uint32_t dw, gaddr r, uint32_t ls_hs_config, bool update) {
  if (!cmd || dw < 30) return 0xFFFFFFFF;
  if (!CheckShader(update ? "UpdateHsShader" : "SetHsShader", r)) return 0xFFFFFFFF;
  Out o{cmd};
  o({Pm4(kSetShReg, 3), 0x108, Reg(r, 0), 0});
  o({Pm4(kSetShReg, 3), 0x10A, Reg(r, 2), Reg(r, 3)});
  ContextRegs(o, update, 0x286, {Reg(r, 5), Reg(r, 6)});
  ContextRegs(o, update, 0x2DB, {Reg(r, 4)});
  ContextRegs(o, update, 0x2D6, {ls_hs_config});
  PadNop(o, 11);
  return 0;
}

// The draw engine's default state (sceGnmDrawInitDefaultHardwareState200):
// packets as [header, body...].
const uint32_t kDrawDefaults[] = {
  Pm4(kSetShReg, 2), 0x216, 0xFFFFFFFF,
  Pm4(kSetShReg, 2), 0x217, 0xFFFFFFFF,
  Pm4(kSetShReg, 2), 0x215, 0x00000000,
  Pm4(kSetContextReg, 2), 0x2F9, 0x0000002D,
  Pm4(kSetContextReg, 2), 0x282, 0x00000008,
  Pm4(kSetContextReg, 2), 0x280, 0x00080008,
  Pm4(kSetContextReg, 2), 0x281, 0xFFFF0000,
  Pm4(kSetContextReg, 2), 0x204, 0x00000000,
  Pm4(kSetContextReg, 2), 0x206, 0x0000043F,
  Pm4(kSetContextReg, 2), 0x083, 0x0000FFFF,
  Pm4(kSetContextReg, 2), 0x317, 0x00000010,
  Pm4(kSetContextReg, 2), 0x2FA, 0x3F800000,
  Pm4(kSetContextReg, 2), 0x2FC, 0x3F800000,
  Pm4(kSetContextReg, 2), 0x2FB, 0x3F800000,
  Pm4(kSetContextReg, 2), 0x2FD, 0x3F800000,
  Pm4(kSetContextReg, 2), 0x202, 0x00CC0010,
  Pm4(kSetContextReg, 2), 0x30E, 0xFFFFFFFF,
  Pm4(kSetContextReg, 2), 0x30F, 0xFFFFFFFF,
  Pm4(0x2F, 1), 0x00000001,  // NUM_INSTANCES
  Pm4(kSetShReg, 2), 0x007, 0x001701FF,
  Pm4(kSetShReg, 2), 0x046, 0x001701FD,
  Pm4(kSetShReg, 2), 0x087, 0x001701FF,
  Pm4(kSetShReg, 2), 0x0C7, 0x001701FD,
  Pm4(kSetShReg, 2), 0x107, 0x00000017,
  Pm4(kSetShReg, 2), 0x147, 0x001701FD,
  Pm4(kSetShReg, 2), 0x047, 0x0000001C,
  Pm4(kSetContextReg, 2), 0x1B1, 0x00000002,
  Pm4(kSetContextReg, 2), 0x101, 0x00000000,
  Pm4(kSetContextReg, 2), 0x100, 0xFFFFFFFF,
  Pm4(kSetContextReg, 2), 0x103, 0x00000000,
  Pm4(kSetContextReg, 2), 0x284, 0x00000000,
  Pm4(kSetContextReg, 2), 0x290, 0x00000000,
  Pm4(kSetContextReg, 2), 0x2AE, 0x00000000,
  Pm4(kSetContextReg, 2), 0x292, 0x00000000,
  Pm4(kSetContextReg, 2), 0x293, 0x06020000,
  Pm4(kSetContextReg, 2), 0x2F8, 0x00000000,
  Pm4(kSetContextReg, 2), 0x2DE, 0x000001E9,
  Pm4(kSetContextReg, 4), 0x295, 0x00000100, 0x00000100, 0x00000004,
  Pm4(0x79, 2), 0x200, 0xE0000000,  // SET_UCONFIG_REG GRBM_GFX_INDEX
};

}  // namespace
}  // namespace recomp::orbis::gnm

using namespace recomp::orbis::gnm;
using recomp::gaddr;
using recomp::hle::Arg;
namespace gpu = recomp::orbis::gpu;

// int sceGnmSetVsShader(uint32_t* cmd, uint32_t dwords, const
// VsStageRegisters* regs, uint32_t shader_modifier)
ORBIS_HLE("libSceGnmDriver", sceGnmSetVsShader, 4) {
  return SetVsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), uint32_t(Arg(c, 3)), false);
}
ORBIS_HLE("libSceGnmDriver", sceGnmUpdateVsShader, 4) {
  return SetVsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), uint32_t(Arg(c, 3)), true);
}
ORBIS_HLE("libSceGnmDriver", sceGnmSetPsShader, 3) {
  return SetPsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), false);
}
ORBIS_HLE("libSceGnmDriver", sceGnmUpdatePsShader, 3) {
  return SetPsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), true);
}
ORBIS_HLE("libSceGnmDriver", sceGnmSetGsShader, 3) {
  return SetGsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), false);
}
ORBIS_HLE("libSceGnmDriver", sceGnmUpdateGsShader, 3) {
  return SetGsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), true);
}
// int sceGnmSetHsShader(uint32_t* cmd, uint32_t dwords, const
// HsStageRegisters* regs, uint32_t vgt_ls_hs_config)
ORBIS_HLE("libSceGnmDriver", sceGnmSetHsShader, 4) {
  return SetHsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), uint32_t(Arg(c, 3)), false);
}
ORBIS_HLE("libSceGnmDriver", sceGnmUpdateHsShader, 4) {
  return SetHsShader(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), uint32_t(Arg(c, 3)), true);
}

// int sceGnmSetEsShader(uint32_t* cmd, uint32_t dwords, const
// EsStageRegisters* regs, uint32_t shader_modifier)
ORBIS_HLE("libSceGnmDriver", sceGnmSetEsShader, 4) {
  const gaddr cmd = Arg(c, 0), r = Arg(c, 2);
  const uint32_t modifier = uint32_t(Arg(c, 3));
  if (!cmd || uint32_t(Arg(c, 1)) < 20) return 0xFFFFFFFF;
  if (r && (modifier & 0xFCFFFC3F)) {
    recomp::WarnF("sceGnmSetEsShader: invalid shader modifier %08X\n", modifier);
    return 0xFFFFFFFF;
  }
  if (!CheckShader("SetEsShader", r)) return 0xFFFFFFFF;
  Out o{cmd};
  o({Pm4(kSetShReg, 3), 0xC8, Reg(r, 0), 0});
  o({Pm4(kSetShReg, 3), 0xCA, Modified(Reg(r, 2), modifier, 0xFCFFFC3F), Reg(r, 3)});
  PadNop(o, 11);
  return 0;
}

// int sceGnmSetLsShader(uint32_t* cmd, uint32_t dwords, const
// LsStageRegisters* regs, uint32_t shader_modifier): a modifier with only
// bits 6-9 keeps PGM_RSRC1's bits 24-25.
ORBIS_HLE("libSceGnmDriver", sceGnmSetLsShader, 4) {
  const gaddr cmd = Arg(c, 0), r = Arg(c, 2);
  const uint32_t modifier = uint32_t(Arg(c, 3));
  if (!cmd || uint32_t(Arg(c, 1)) < 23) return 0xFFFFFFFF;
  const uint32_t keep = modifier & 0xFFFFFC3F ? 0xFCFFFC3F : 0xFFFFFC3F;
  if (r && (modifier & keep)) {
    recomp::WarnF("sceGnmSetLsShader: invalid shader modifier %08X\n", modifier);
    return 0xFFFFFFFF;
  }
  if (!CheckShader("SetLsShader", r)) return 0xFFFFFFFF;
  Out o{cmd};
  o({Pm4(kSetShReg, 3), 0x148, Reg(r, 0), 0});
  o({Pm4(kSetShReg, 2), 0x14B, Reg(r, 3)});
  o({Pm4(kSetShReg, 3), 0x14A, Modified(Reg(r, 2), modifier, keep), Reg(r, 3)});
  PadNop(o, 11);
  return 0;
}

// int sceGnmSetCsShader(uint32_t* cmd, uint32_t dwords, const
// CsStageRegisters* regs)
ORBIS_HLE("libSceGnmDriver", sceGnmSetCsShader, 3) {
  const gaddr cmd = Arg(c, 0), r = Arg(c, 2);
  if (!cmd || uint32_t(Arg(c, 1)) < 25) return 0xFFFFFFFF;
  if (!CheckShader("SetCsShader", r)) return 0xFFFFFFFF;
  Out o{cmd};
  o({Pm4(kSetShReg, 3, true), 0x20C, Reg(r, 0), 0});
  o({Pm4(kSetShReg, 3, true), 0x212, Reg(r, 2), Reg(r, 3)});
  o({Pm4(kSetShReg, 4, true), 0x207, Reg(r, 4), Reg(r, 5), Reg(r, 6)});
  PadNop(o, 11);
  return 0;
}

// uint32_t sceGnmDrawInitDefaultHardwareState200(uint32_t* cmd, uint32_t
// dwords): 256 dwords (the rest a NOP), or 0 if there is less room.
ORBIS_HLE("libSceGnmDriver", sceGnmDrawInitDefaultHardwareState200, 2) {
  if (uint32_t(Arg(c, 1)) < 0x100) return 0;
  Out o{Arg(c, 0)};
  o({Pm4(0x28, 2), 0x80000000, 0x80000000});  // CONTEXT_CONTROL
  o({Pm4(0x12, 1), 0});                       // CLEAR_STATE
  o({Pm4(0x58, 6), 0x2EC47FC0, 0xFFFFFFFF, 0, 0, 0, 0xA});  // ACQUIRE_MEM
  for (uint32_t v : kDrawDefaults) o(v);
  const uint32_t used = uint32_t((o.p - Arg(c, 0)) / 4);
  PadNop(o, 0x100 - used - 1);
  return 0x100;
}

// uint32_t sceGnmDispatchInitDefaultHardwareState(uint32_t* cmd, uint32_t
// dwords): as the draw engine's.
ORBIS_HLE("libSceGnmDriver", sceGnmDispatchInitDefaultHardwareState, 2) {
  if (uint32_t(Arg(c, 1)) < 0x100) return 0;
  Out o{Arg(c, 0)};
  o({Pm4(kSetShReg, 2, true), 0x216, 0xFFFFFFFF});
  o({Pm4(kSetShReg, 2, true), 0x217, 0xFFFFFFFF});
  o({Pm4(kSetShReg, 2, true), 0x215, 0x170});
  o({Pm4(0x58, 6), 0x28000000, 0, 0, 0, 0, 0xA});
  PadNop(o, 0x100 - 16 - 1);
  return 0x100;
}

// int sceGnmInsertWaitFlipDone(uint32_t* cmd, uint32_t dwords, int
// video_handle, uint32_t buffer): 7 dwords, a WAIT_REG_MEM for the
// buffer's label to be 0.
ORBIS_HLE("libSceGnmDriver", sceGnmInsertWaitFlipDone, 4) {
  if (uint32_t(Arg(c, 1)) != 7) return 0xFFFFFFFF;
  const gaddr label =
      recomp::orbis::videoout::LabelAddress(int(Arg(c, 2))) + 8 * uint64_t(uint32_t(Arg(c, 3)));
  Out o{Arg(c, 0)};
  o({Pm4(0x3C, 6), 0x13, uint32_t(label) & ~3u, uint32_t(label >> 32) & 0xFFFF, 0, 0xFFFFFFFF,
     0xA});
  return 0;
}

// Markers (for the GPU debugger): a NOP of 0x6875xxxx, the string and
// (colour markers) a colour.
static uint64_t Marker(gaddr cmd, uint32_t dw, gaddr str, uint32_t kind, bool colour,
                       uint32_t argb) {
  if (!cmd || !str) return 0xFFFFFFFF;
  const std::string s = recomp::GStr(str);
  const uint32_t len = uint32_t(s.size()), extra = colour ? 4 : 0;
  const uint32_t body = ((len + 8 + extra) >> 2) + 2 * ((len + 12 + extra) >> 3);
  if (dw != body + 2) return 0xFFFFFFFF;
  Out o{cmd};
  o({0xC0001000 | body << 16, kind});
  std::memcpy(recomp::GPtr(cmd + 8), s.c_str(), len + 1);
  if (colour) recomp::Wr32(cmd + 8 + len + 1, argb);
  std::memset(recomp::GPtr(cmd + 8 + len + 1 + extra), 0, 4 * body - len - 1 - extra);
  return 0;
}

ORBIS_HLE("libSceGnmDriver", sceGnmInsertPushMarker, 3) {
  return Marker(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), 0x68750001, false, 0);
}
ORBIS_HLE("libSceGnmDriver", sceGnmInsertSetMarker, 3) {
  return Marker(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), 0x68750003, false, 0);
}
ORBIS_HLE("libSceGnmDriver", sceGnmInsertPushColorMarker, 4) {
  return Marker(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), 0x6875000E, true, uint32_t(Arg(c, 3)));
}
ORBIS_HLE("libSceGnmDriver", sceGnmInsertSetColorMarker, 4) {
  return Marker(Arg(c, 0), uint32_t(Arg(c, 1)), Arg(c, 2), 0x6875000F, true, uint32_t(Arg(c, 3)));
}
ORBIS_HLE("libSceGnmDriver", sceGnmInsertPopMarker, 2) {
  if (!Arg(c, 0) || uint32_t(Arg(c, 1)) != 6) return 0xFFFFFFFF;
  Out o{Arg(c, 0)};
  o({0xC0041000, 0x68750002, 0, 0, 0, 0});
  return 0;
}

// ---- submission

// Reads count command buffers' addresses and sizes (bytes) into dcbs/ccbs
// with the driver's checks; 0 or its error.
static uint32_t ReadSubmission(uint32_t count, gaddr dcb_addrs, gaddr dcb_sizes, gaddr ccb_addrs,
                               gaddr ccb_sizes, std::vector<gpu::CmdBuffer>& dcbs,
                               std::vector<gpu::CmdBuffer>& ccbs) {
  if (count) {
    const char* err = !dcb_addrs   ? "dcbGpuAddrs must not be null"
                      : !dcb_sizes ? "dcbSizesInBytes must not be null"
                      : ccb_sizes && !ccb_addrs
                          ? "ccbGpuAddrs must not be null if ccbSizesInBytes is not"
                          : nullptr;
    if (err) {
      recomp::WarnF("sceGnmSubmitCommandBuffers: %s\n", err);
      return kErrSubmit;
    }
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t ds = recomp::Rd32(dcb_sizes + 4 * uint64_t(i));
    const uint32_t cs = ccb_sizes ? recomp::Rd32(ccb_sizes + 4 * uint64_t(i)) : 0;
    if (!ds || ds >= 0x3FFFFD || cs >= 0x3FFFFD) {
      recomp::WarnF("sceGnmSubmitCommandBuffers: buffer %u: DCB %u bytes, CCB %u bytes\n", i, ds,
                    cs);
      return kErrSubmit;
    }
    dcbs.push_back({recomp::Rd64(dcb_addrs + 8 * uint64_t(i)), ds / 4});
    ccbs.push_back({cs ? recomp::Rd64(ccb_addrs + 8 * uint64_t(i)) : 0, cs / 4});
  }
  return 0;
}

// int sceGnmSubmitCommandBuffers(uint32_t count, void* dcbs[], uint32_t
// dcb_sizes[], void* ccbs[], uint32_t ccb_sizes[])
ORBIS_HLE("libSceGnmDriver", sceGnmSubmitCommandBuffers, 5) {
  std::vector<gpu::CmdBuffer> dcbs, ccbs;
  if (uint32_t e = ReadSubmission(uint32_t(Arg(c, 0)), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4),
                                  dcbs, ccbs))
    return e;
  gpu::SubmitGraphics(std::move(dcbs), std::move(ccbs), nullptr);
  return 0;
}

// int sceGnmSubmitAndFlipCommandBuffers(count, dcbs, dcb_sizes, ccbs,
// ccb_sizes, int video_handle, int buffer, int flip_mode, int64_t
// flip_arg): the last DCB ends in the 64 dwords the game's Gnm library
// reserved for the flip (a NOP of 0x68750777, 778, 780 or 781 and its
// operands), which the driver rewrites: a WRITE_DATA of 1 to the buffer's
// label, then per kind nothing, a WRITE_DATA of the game's, or an
// end-of-pipe event. The flip runs once the GPU has finished the
// submission.
ORBIS_HLE("libSceGnmDriver", sceGnmSubmitAndFlipCommandBuffers, 9) {
  using recomp::Rd32;
  const uint32_t count = uint32_t(Arg(c, 0));
  const int handle = int(Arg(c, 5)), index = int(Arg(c, 6));
  const uint32_t mode = uint32_t(Arg(c, 7));
  const int64_t arg = int64_t(Arg(c, 8));
  std::vector<gpu::CmdBuffer> dcbs, ccbs;
  if (uint32_t e = ReadSubmission(count, Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), dcbs, ccbs))
    return e;
  if (!count) recomp::FatalF("sceGnmSubmitAndFlipCommandBuffers: no command buffers");
  const gaddr p = dcbs.back().addr + 4 * uint64_t(dcbs.back().dwords - 64);
  const uint32_t kind = Rd32(p + 4);
  const bool known = kind == 0x68750777 || kind == 0x68750778 || kind == 0x68750780 ||
                     kind == 0x68750781;
  if (Rd32(p) != 0xC03E1000 || !known || (kind == 0x68750778 && Rd32(p + 8) & 3)) {
    recomp::WarnF("sceGnmSubmitAndFlipCommandBuffers: no flip packet at the end of the DCB\n");
    return kErrNoFlipPacket;
  }
  if (index == -1) {
    recomp::WarnF("sceGnmSubmitAndFlipCommandBuffers: invalid buffer index\n");
    return kErrFlipIndex;
  }
  uint32_t op[5];
  for (int i = 0; i < 5; i++) op[i] = Rd32(p + 8 + 4 * uint64_t(i));
  const gaddr label = recomp::orbis::videoout::LabelAddress(handle) + 8 * uint64_t(uint32_t(index));
  Out o{p};
  o({Pm4(0x37, 4), 0x500, uint32_t(label) & ~3u, uint32_t(label >> 32), 1});  // WRITE_DATA
  switch (kind) {
    case 0x68750777:
      o({Pm4(kNop, 58), 0x68750776});
      break;
    case 0x68750778:
      o({Pm4(kNop, 53), 0x68750776});
      o = Out{p + 4 * 59};
      o({Pm4(0x37, 4), 0x500, op[0] & ~3u, op[1], op[2]});
      break;
    default:  // 780 and 781: an end-of-pipe event (781 with the game's data)
      o({Pm4(kNop, 52), 0x68750776});
      o = Out{p + 4 * 58};
      o({Pm4(0x47, 5), 0x500 + (op[3] & 0x3F) + ((op[4] & 0x3F) << 12)});
      if (kind == 0x68750780)
        o({0, 0x01000000, 0, 0});
      else
        o({op[0] & ~3u, (op[1] & 0xFFFF) | 0x22000000, op[2], 0});
      break;
  }
  gpu::SubmitGraphics(std::move(dcbs), std::move(ccbs), [handle, index, mode, arg] {
    const int32_t r = recomp::orbis::videoout::SubmitFlip(handle, index, mode, arg);
    if (r) recomp::FatalF("sceGnmSubmitAndFlipCommandBuffers: the flip failed (%08X)", uint32_t(r));
  });
  return 0;
}

// The frame's submissions are done (the system may suspend the GPU now).
ORBIS_HLE("libSceGnmDriver", sceGnmSubmitDone, 0) { return 0; }
// Submissions are always allowed (no system suspend).
ORBIS_HLE("libSceGnmDriver", sceGnmAreSubmitsAllowed, 0) { return 1; }

// ---- events

// int sceGnmAddEqEvent(SceKernelEqueue eq, int id, void* udata)
ORBIS_HLE("libSceGnmDriver", sceGnmAddEqEvent, 3) {
  using namespace recomp::orbis::kernel;
  const uint64_t id = uint64_t(int64_t(int32_t(Arg(c, 1))));
  if (id != gpu::kEqEventGfxEop) recomp::FatalF("sceGnmAddEqEvent: event %llX not implemented yet",
                                                (unsigned long long)id);
  return AddEvent(uint32_t(Arg(c, 0)), id, gpu::kFilterGraphicsCore, Arg(c, 2)) ? 0
                                                                                : SceError(kEBADF);
}
ORBIS_HLE("libSceGnmDriver", sceGnmDeleteEqEvent, 2) {
  using namespace recomp::orbis::kernel;
  const uint64_t id = uint64_t(int64_t(int32_t(Arg(c, 1))));
  return DeleteEvent(uint32_t(Arg(c, 0)), id, gpu::kFilterGraphicsCore) ? 0 : SceError(kENOENT);
}
// The event's data: its id; the timestamp (bits 16 up) is 0.
ORBIS_HLE("libSceGnmDriver", sceGnmGetEqEventType, 1) { return recomp::Rd64(Arg(c, 0) + 16); }
ORBIS_HLE("libSceGnmDriver", sceGnmGetEqTimeStamp, 1) {
  return uint64_t(int64_t(recomp::Rd64(Arg(c, 0) + 16)) >> 16);
}

// ---- compute queues

static uint64_t MapComputeQueue(uint32_t pipe, uint32_t queue, gaddr ring, uint32_t size_dw,
                                gaddr rptr) {
  if (pipe > 6) return 0x80D17000;
  if (queue > 7) return 0x80D17001;
  if (size_dw & (size_dw - 1)) return 0x80D17002;
  if (ring & 0xFF) return 0x80D17003;
  if (rptr & 3) return 0x80D17004;
  const uint32_t vqid = pipe * 8 + queue + 1;
  gpu::MapComputeRing(vqid, ring, size_dw, rptr);
  return vqid;
}

// int sceGnmMapComputeQueue(uint32_t pipe, uint32_t queue, void* ring,
// uint32_t ring_dwords, void* read_ptr): the queue's id.
ORBIS_HLE("libSceGnmDriver", sceGnmMapComputeQueue, 5) {
  return MapComputeQueue(uint32_t(Arg(c, 0)), uint32_t(Arg(c, 1)), Arg(c, 2), uint32_t(Arg(c, 3)),
                         Arg(c, 4));
}
// ... and a priority, which the GPU's queues here do not have.
ORBIS_HLE("libSceGnmDriver", sceGnmMapComputeQueueWithPriority, 6) {
  return MapComputeQueue(uint32_t(Arg(c, 0)), uint32_t(Arg(c, 1)), Arg(c, 2), uint32_t(Arg(c, 3)),
                         Arg(c, 4));
}
ORBIS_HLE("libSceGnmDriver", sceGnmUnmapComputeQueue, 1) {
  if (uint32_t(Arg(c, 0))) gpu::UnmapComputeRing(uint32_t(Arg(c, 0)));
  return 0;
}
// void sceGnmDingDong(uint32_t vqid, uint32_t next_offset_dwords)
ORBIS_HLE("libSceGnmDriver", sceGnmDingDong, 2) {
  if (uint32_t(Arg(c, 0))) gpu::KickComputeRing(uint32_t(Arg(c, 0)), uint32_t(Arg(c, 1)));
  return 0;
}

// ---- configuration and queries

// int sceGnmSetGsRingSizes(uint32_t esgs, uint32_t gsvs): 4 to 8 MiB each,
// in 1 MiB steps.
ORBIS_HLE("libSceGnmDriver", sceGnmSetGsRingSizes, 2) {
  auto ok = [](uint32_t s) { return s >= 0x400000 && s <= 0x800000 && !(s & 0xFFFFF); };
  return ok(uint32_t(Arg(c, 0))) && ok(uint32_t(Arg(c, 1))) ? 0 : 0x8000000;
}

// The tessellation factor ring's fixed GPU address.
ORBIS_HLE("libSceGnmDriver", sceGnmGetTheTessellationFactorRingBufferBaseAddress, 0) {
  return 0xFF0000000;
}

// The CU numbering: masks map to themselves; the index query answers -1
// for a valid shader engine (0-1) and index (0-8), as 11.00's driver does.
ORBIS_HLE("libSceGnmDriver", sceGnmLogicalCuMaskToPhysicalCuMask, 2) { return uint32_t(Arg(c, 1)); }
ORBIS_HLE("libSceGnmDriver", sceGnmLogicalCuIndexToPhysicalCuIndex, 2) {
  if (uint32_t(Arg(c, 0)) > 1 || uint32_t(Arg(c, 1)) >= 9) {
    recomp::WarnF("sceGnmLogicalCuIndexToPhysicalCuIndex: shader engine %u, index %u\n",
                  uint32_t(Arg(c, 0)), uint32_t(Arg(c, 1)));
    return 0;
  }
  return 0xFFFFFFFF;
}

// Mip statistics (the GPU's texture-fetch counters): accepted with the
// driver's checks and never written.
static uint64_t CheckMipStats(gaddr buffer, uint32_t size) {
  if (buffer & 0xFFFFFF000000003F) return 0x80000001;
  if (size < 0x1040 || (size & 0x7FF) != 0x40) return 0x80000002;
  return 0;
}
ORBIS_HLE("libSceGnmDriver", sceGnmSetupMipStatsReport, 4) {
  return CheckMipStats(Arg(c, 0), uint32_t(Arg(c, 1)));
}
ORBIS_HLE("libSceGnmDriver", sceGnmRequestMipStatsReportAndReset, 2) {
  return CheckMipStats(Arg(c, 0), uint32_t(Arg(c, 1)));
}
ORBIS_HLE("libSceGnmDriver", sceGnmDisableMipStatsReport, 0) { return 0; }

// Memory is coherent: nothing to flush.
ORBIS_HLE("libSceGnmDriver", sceGnmFlushGarlic, 0) { return 0; }
ORBIS_HLE("libSceGnmDriver", sceGnmDebugHardwareStatus, 1) { return 0; }
ORBIS_HLE("libSceGnmDriver", sceGnmIsUserPaEnabled, 0) { return 0; }
ORBIS_HLE("libSceGnmDriver", sceGnmValidateCommandBuffers, 0) { return 0x80D13FFF; }

// No GPU debugger (Razor) or capture.
ORBIS_HLE("libSceGnmDriver", sceRazorIsLoaded, 0) { return 0; }
ORBIS_HLE("libSceGnmDriver", sceGnmDriverCaptureInProgress, 0) { return 0; }
ORBIS_HLE("libSceGnmDriver", sceGnmDriverTriggerCapture, 1) { return 0x80D15001; }
ORBIS_HLE("libSceGnmDriver", sceRazorCaptureImmediate, 1) { return 0x80D1500F; }
ORBIS_HLE("libSceGnmDriver", sceRazorCaptureSinceLastFlip, 1) { return 0x80D1500F; }
ORBIS_HLE("libSceGnmDriver", sceRazorCaptureCommandBuffersOnlyImmediate, 1) { return 0x80D1500F; }
ORBIS_HLE("libSceGnmDriver", sceRazorCaptureCommandBuffersOnlySinceLastFlip, 1) {
  return 0x80D1500F;
}
