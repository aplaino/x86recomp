#include "win32/dxbc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <strings.h>
#include <set>
#include <tuple>

// The token format is d3d11TokenizedProgramFormat.hpp's (the Windows
// SDK's description of SHDR/SHEX), the containers and signatures as the
// D3D compiler writes them.

namespace recomp::hle::d3d11::dxbc {
namespace {

// ---- container

uint32_t Rd32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
constexpr uint32_t FourCC(const char* s) {
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
         uint32_t(uint8_t(s[3])) << 24;
}

struct Chunk {
  uint32_t fourcc = 0;
  const uint8_t* data = nullptr;
  uint32_t size = 0;
};

bool Chunks(const uint8_t* p, size_t size, std::vector<Chunk>* out, std::string* error) {
  if (size < 32 || Rd32(p) != FourCC("DXBC")) {
    *error = "not a DXBC container";
    return false;
  }
  const uint32_t total = Rd32(p + 24), count = Rd32(p + 28);
  if (total > size || total < 32 || count > (total - 32) / 4) {
    *error = "a DXBC container's size or chunk count is out of range";
    return false;
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t at = Rd32(p + 32 + 4 * i);
    if (at > total - 8 || Rd32(p + at + 4) > total - at - 8) {
      *error = "a DXBC chunk is out of range";
      return false;
    }
    out->push_back({Rd32(p + at), p + at + 8, Rd32(p + at + 4)});
  }
  return true;
}

// ISGN/OSGN/PCSG (24-byte elements), OSG5 (28: a stream first),
// ISG1/OSG1/PSG1 (32: a stream first, a minimum precision last).
bool Signature(const Chunk& c, std::vector<SignatureElement>* out, std::string* error) {
  const uint32_t stride = c.fourcc == FourCC("OSG5") ? 28
                          : c.fourcc == FourCC("ISG1") || c.fourcc == FourCC("OSG1") ? 32 : 24;
  const bool stream = stride != 24;
  if (c.size < 8 || Rd32(c.data) > (c.size - 8) / stride) {
    *error = "a signature chunk is out of range";
    return false;
  }
  const uint32_t n = Rd32(c.data);
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t* e = c.data + 8 + i * stride;
    SignatureElement el;
    if (stream) {
      el.stream = Rd32(e);
      e += 4;
    }
    const uint32_t name = Rd32(e);
    if (name >= c.size) {
      *error = "a signature element's name is out of range";
      return false;
    }
    const uint8_t* s = c.data + name;
    const void* nul = std::memchr(s, 0, c.size - name);
    if (!nul) {
      *error = "a signature element's name is not terminated";
      return false;
    }
    el.semantic.assign(reinterpret_cast<const char*>(s), static_cast<const uint8_t*>(nul) - s);
    el.index = Rd32(e + 4);
    el.system_value = Rd32(e + 8);
    el.component_type = Rd32(e + 12);
    el.reg = Rd32(e + 16);
    el.mask = e[20] & 0xF;
    el.rw_mask = e[21] & 0xF;
    // Pixel outputs carry no system value in the signature: their D3D_NAME
    // from the semantic.
    if (el.system_value == 0) {
      static const std::pair<const char*, uint32_t> kNames[] = {
          {"SV_TARGET", 64}, {"SV_DEPTH", 65}, {"SV_COVERAGE", 66}, {"SV_DEPTHGREATEREQUAL", 67},
          {"SV_DEPTHLESSEQUAL", 68}, {"SV_STENCILREF", 69}};
      for (const auto& [n, v] : kNames)
        if (strcasecmp(el.semantic.c_str(), n) == 0) el.system_value = v;
    }
    out->push_back(el);
  }
  return true;
}

// ---- tokens

enum Op : uint32_t {
  kAdd = 0, kAnd = 1, kBreak = 2, kBreakc = 3, kCall = 4, kCallc = 5, kCase = 6, kContinue = 7,
  kContinuec = 8, kCut = 9, kDefault = 10, kDerivRtx = 11, kDerivRty = 12, kDiscard = 13, kDiv = 14,
  kDp2 = 15, kDp3 = 16, kDp4 = 17, kElse = 18, kEmit = 19, kEmitThenCut = 20, kEndif = 21,
  kEndloop = 22, kEndswitch = 23, kEq = 24, kExp = 25, kFrc = 26, kFtoi = 27, kFtou = 28, kGe = 29,
  kIadd = 30, kIf = 31, kIeq = 32, kIge = 33, kIlt = 34, kImad = 35, kImax = 36, kImin = 37,
  kImul = 38, kIne = 39, kIneg = 40, kIshl = 41, kIshr = 42, kItof = 43, kLabel = 44, kLd = 45,
  kLdMs = 46, kLog = 47, kLoop = 48, kLt = 49, kMad = 50, kMin = 51, kMax = 52, kCustomData = 53,
  kMov = 54, kMovc = 55, kMul = 56, kNe = 57, kNop = 58, kNot = 59, kOr = 60, kResinfo = 61,
  kRet = 62, kRetc = 63, kRoundNe = 64, kRoundNi = 65, kRoundPi = 66, kRoundZ = 67, kRsq = 68,
  kSample = 69, kSampleC = 70, kSampleCLz = 71, kSampleL = 72, kSampleD = 73, kSampleB = 74,
  kSqrt = 75, kSwitch = 76, kSincos = 77, kUdiv = 78, kUlt = 79, kUge = 80, kUmul = 81, kUmad = 82,
  kUmax = 83, kUmin = 84, kUshr = 85, kUtof = 86, kXor = 87,
  kDclResource = 88, kDclConstantBuffer = 89, kDclSampler = 90, kDclIndexRange = 91,
  kDclGsOutputTopology = 92, kDclGsInputPrimitive = 93, kDclMaxOutputVertexCount = 94,
  kDclInput = 95, kDclInputSgv = 96, kDclInputSiv = 97, kDclInputPs = 98, kDclInputPsSgv = 99,
  kDclInputPsSiv = 100, kDclOutput = 101, kDclOutputSgv = 102, kDclOutputSiv = 103, kDclTemps = 104,
  kDclIndexableTemp = 105, kDclGlobalFlags = 106,
  kLod = 108, kGather4 = 109, kSamplePos = 110, kSampleInfo = 111,
  kHsDecls = 113, kHsControlPointPhase = 114, kHsForkPhase = 115, kHsJoinPhase = 116,
  kEmitStream = 117, kCutStream = 118, kEmitThenCutStream = 119, kInterfaceCall = 120,
  kBufinfo = 121, kDerivRtxCoarse = 122, kDerivRtxFine = 123, kDerivRtyCoarse = 124,
  kDerivRtyFine = 125, kGather4C = 126, kGather4Po = 127, kGather4PoC = 128, kRcp = 129,
  kF32ToF16 = 130, kF16ToF32 = 131, kUaddc = 132, kUsubb = 133, kCountbits = 134,
  kFirstbitHi = 135, kFirstbitLo = 136, kFirstbitShi = 137, kUbfe = 138, kIbfe = 139, kBfi = 140,
  kBfrev = 141, kSwapc = 142, kDclStream = 143, kDclFunctionBody = 144, kDclFunctionTable = 145,
  kDclInterface = 146, kDclInputControlPointCount = 147, kDclOutputControlPointCount = 148,
  kDclTessDomain = 149, kDclTessPartitioning = 150, kDclTessOutputPrimitive = 151,
  kDclHsMaxTessFactor = 152, kDclHsForkPhaseInstanceCount = 153,
  kDclHsJoinPhaseInstanceCount = 154, kDclThreadGroup = 155, kDclUavTyped = 156,
  kDclUavRaw = 157, kDclUavStructured = 158, kDclTgsmRaw = 159, kDclTgsmStructured = 160,
  kDclResourceRaw = 161, kDclResourceStructured = 162, kLdUavTyped = 163, kStoreUavTyped = 164,
  kLdRaw = 165, kStoreRaw = 166, kLdStructured = 167, kStoreStructured = 168, kAtomicAnd = 169,
  kAtomicOr = 170, kAtomicXor = 171, kAtomicCmpStore = 172, kAtomicIadd = 173, kAtomicImax = 174,
  kAtomicImin = 175, kAtomicUmax = 176, kAtomicUmin = 177, kImmAtomicAlloc = 178,
  kImmAtomicConsume = 179, kImmAtomicIadd = 180, kImmAtomicAnd = 181, kImmAtomicOr = 182,
  kImmAtomicXor = 183, kImmAtomicExch = 184, kImmAtomicCmpExch = 185, kImmAtomicImax = 186,
  kImmAtomicImin = 187, kImmAtomicUmax = 188, kImmAtomicUmin = 189, kSync = 190,
  kEvalSnapped = 203, kEvalSampleIndex = 204, kEvalCentroid = 205, kDclGsInstanceCount = 206,
  kOpCount = 218,
};

const char* const kOpNames[kOpCount] = {
    "add", "and", "break", "breakc", "call", "callc", "case", "continue", "continuec", "cut",
    "default", "deriv_rtx", "deriv_rty", "discard", "div", "dp2", "dp3", "dp4", "else", "emit",
    "emitthencut", "endif", "endloop", "endswitch", "eq", "exp", "frc", "ftoi", "ftou", "ge",
    "iadd", "if", "ieq", "ige", "ilt", "imad", "imax", "imin", "imul", "ine", "ineg", "ishl",
    "ishr", "itof", "label", "ld", "ld_ms", "log", "loop", "lt", "mad", "min", "max",
    "customdata", "mov", "movc", "mul", "ne", "nop", "not", "or", "resinfo", "ret", "retc",
    "round_ne", "round_ni", "round_pi", "round_z", "rsq", "sample", "sample_c", "sample_c_lz",
    "sample_l", "sample_d", "sample_b", "sqrt", "switch", "sincos", "udiv", "ult", "uge", "umul",
    "umad", "umax", "umin", "ushr", "utof", "xor", "dcl_resource", "dcl_constantbuffer",
    "dcl_sampler", "dcl_indexrange", "dcl_outputtopology", "dcl_inputprimitive",
    "dcl_maxout", "dcl_input", "dcl_input_sgv", "dcl_input_siv", "dcl_input_ps",
    "dcl_input_ps_sgv", "dcl_input_ps_siv", "dcl_output", "dcl_output_sgv", "dcl_output_siv",
    "dcl_temps", "dcl_indexableTemp", "dcl_globalFlags", "reserved107", "lod", "gather4",
    "sample_pos", "sample_info", "reserved112", "hs_decls", "hs_control_point_phase",
    "hs_fork_phase", "hs_join_phase", "emit_stream", "cut_stream", "emitthencut_stream",
    "fcall", "bufinfo", "deriv_rtx_coarse", "deriv_rtx_fine", "deriv_rty_coarse",
    "deriv_rty_fine", "gather4_c", "gather4_po", "gather4_po_c", "rcp", "f32tof16", "f16tof32",
    "uaddc", "usubb", "countbits", "firstbit_hi", "firstbit_lo", "firstbit_shi", "ubfe", "ibfe",
    "bfi", "bfrev", "swapc", "dcl_stream", "dcl_function_body", "dcl_function_table",
    "dcl_interface", "dcl_input_control_point_count", "dcl_output_control_point_count",
    "dcl_tessellator_domain", "dcl_tessellator_partitioning",
    "dcl_tessellator_output_primitive", "dcl_hs_max_tessfactor",
    "dcl_hs_fork_phase_instance_count", "dcl_hs_join_phase_instance_count", "dcl_thread_group",
    "dcl_uav_typed", "dcl_uav_raw", "dcl_uav_structured", "dcl_tgsm_raw", "dcl_tgsm_structured",
    "dcl_resource_raw", "dcl_resource_structured", "ld_uav_typed", "store_uav_typed", "ld_raw",
    "store_raw", "ld_structured", "store_structured", "atomic_and", "atomic_or", "atomic_xor",
    "atomic_cmp_store", "atomic_iadd", "atomic_imax", "atomic_imin", "atomic_umax", "atomic_umin",
    "imm_atomic_alloc", "imm_atomic_consume", "imm_atomic_iadd", "imm_atomic_and",
    "imm_atomic_or", "imm_atomic_xor", "imm_atomic_exch", "imm_atomic_cmp_exch",
    "imm_atomic_imax", "imm_atomic_imin", "imm_atomic_umax", "imm_atomic_umin", "sync", "dadd",
    "dmax", "dmin", "dmul", "deq", "dge", "dlt", "dne", "dmov", "dmovc", "dtof", "ftod",
    "eval_snapped", "eval_sample_index", "eval_centroid", "dcl_gsinstances", "abort",
    "debug_break", "reserved209", "ddiv", "dfma", "drcp", "msad", "dtoi", "dtou", "itod", "utod",
};

// Operand types (D3D10_SB_OPERAND_TYPE).
enum OperandType : uint32_t {
  kTemp = 0, kInput = 1, kOutput = 2, kIndexableTemp = 3, kImm32 = 4, kImm64 = 5, kSamplerReg = 6,
  kResource = 7, kConstantBuffer = 8, kImmConstantBuffer = 9, kLabelReg = 10, kInputPrimitiveId = 11,
  kOutputDepth = 12, kNull = 13, kRasterizer = 14, kOutputCoverageMask = 15, kStream = 16,
  kFunctionBody = 17, kFunctionTable = 18, kInterface = 19, kFunctionInput = 20,
  kFunctionOutput = 21, kOutputControlPointId = 22, kInputForkInstanceId = 23,
  kInputJoinInstanceId = 24, kInputControlPoint = 25, kOutputControlPoint = 26,
  kInputPatchConstant = 27, kInputDomainPoint = 28, kThisPointer = 29, kUav = 30, kTgsm = 31,
  kInputThreadId = 32, kInputThreadGroupId = 33, kInputThreadIdInGroup = 34, kInputCoverageMask = 35,
  kInputThreadIdInGroupFlattened = 36, kInputGsInstanceId = 37, kOutputDepthGe = 38,
  kOutputDepthLe = 39, kCycleCounter = 40, kOutputStencilRef = 41, kInnerCoverage = 42,
};

struct Operand {
  uint32_t type = 0;
  uint32_t ncomp = 0;  // 0, 1 or 4
  uint32_t mode = 0;   // four components: 0 mask, 1 swizzle, 2 select one
  uint8_t mask = 0xF;  // mask mode
  uint8_t swz[4] = {0, 1, 2, 3};
  uint32_t mod = 0;  // 1 neg, 2 abs, 3 both
  uint32_t ndim = 0;
  uint32_t imm[4] = {};
  struct Index {
    uint32_t imm = 0;
    int rel = -1;  // the operand in `rel` added to imm
  } index[3];
  std::vector<Operand> rel;
};

struct Instr {
  uint32_t at = 0;  // dword offset in the program
  uint32_t op = 0, len = 0, token = 0;
  bool sat = false, nz = false;
  int off[3] = {};  // aoffimmi
  bool has_off = false;
  uint32_t res_dim = 0, stride = 0, ret[4] = {};
  std::vector<Operand> ops;
  std::vector<uint32_t> lits;  // declarations' literals; customdata's payload
};

// Declarations' tokens after the opcode: o an operand, L a literal.
const char* DeclLayout(uint32_t op) {
  switch (op) {
    case kDclResource: case kDclIndexRange: case kDclInputSgv: case kDclInputSiv:
    case kDclInputPsSgv: case kDclInputPsSiv: case kDclOutputSgv: case kDclOutputSiv:
    case kDclUavTyped: case kDclUavStructured: case kDclTgsmRaw: case kDclResourceStructured:
      return "oL";
    case kDclConstantBuffer: case kDclSampler: case kDclInput: case kDclInputPs: case kDclOutput:
    case kDclStream: case kDclUavRaw: case kDclResourceRaw:
      return "o";
    case kDclTgsmStructured: return "oLL";
    case kDclMaxOutputVertexCount: case kDclTemps: case kDclFunctionBody:
    case kDclHsMaxTessFactor: case kDclHsForkPhaseInstanceCount:
    case kDclHsJoinPhaseInstanceCount: case kDclGsInstanceCount:
      return "L";
    case kDclIndexableTemp: case kDclThreadGroup: return "LLL";
    case kDclGsOutputTopology: case kDclGsInputPrimitive: case kDclGlobalFlags:
    case kDclInputControlPointCount: case kDclOutputControlPointCount: case kDclTessDomain:
    case kDclTessPartitioning: case kDclTessOutputPrimitive: case kHsDecls:
    case kHsControlPointPhase: case kHsForkPhase: case kHsJoinPhase:
      return "";
    case kDclFunctionTable: case kDclInterface: case kInterfaceCall: return "*";
    default: return nullptr;  // operands only
  }
}

// The operands an instruction the translation knows has (at least).
size_t MinOperands(uint32_t op) {
  switch (op) {
    case kBreakc: case kContinuec: case kRetc: case kDiscard: case kIf: case kSwitch: case kCase:
      return 1;
    case kMov: case kFrc: case kExp: case kLog: case kRsq: case kSqrt: case kRcp: case kRoundNe:
    case kRoundNi: case kRoundPi: case kRoundZ: case kDerivRtx: case kDerivRty: case kDerivRtxCoarse:
    case kDerivRtxFine: case kDerivRtyCoarse: case kDerivRtyFine: case kItof: case kUtof: case kFtoi:
    case kFtou: case kF32ToF16: case kF16ToF32: case kCountbits: case kFirstbitHi: case kFirstbitLo:
    case kFirstbitShi: case kBfrev: case kNot: case kIneg: case kBufinfo: case kSampleInfo:
    case kImmAtomicAlloc: case kImmAtomicConsume:
      return 2;
    case kAdd: case kAnd: case kDiv: case kDp2: case kDp3: case kDp4: case kEq: case kGe: case kLt:
    case kNe: case kIadd: case kIeq: case kIge: case kIlt: case kImax: case kImin: case kIne: case kIshl:
    case kIshr: case kMax: case kMin: case kMul: case kOr: case kUshr: case kXor: case kUlt: case kUge:
    case kUmax: case kUmin: case kLd: case kLdUavTyped: case kResinfo: case kLdRaw: case kSincos:
    case kStoreUavTyped: case kStoreRaw: case kAtomicAnd: case kAtomicOr: case kAtomicXor:
    case kAtomicIadd: case kAtomicImax: case kAtomicImin: case kAtomicUmax: case kAtomicUmin:
      return 3;
    case kMad: case kMovc: case kImad: case kUmad: case kImul: case kUmul: case kUdiv: case kUaddc:
    case kUsubb: case kSample: case kGather4: case kLod: case kLdMs: case kLdStructured:
    case kStoreStructured: case kUbfe: case kIbfe: case kAtomicCmpStore: case kImmAtomicIadd:
    case kImmAtomicAnd: case kImmAtomicOr: case kImmAtomicXor: case kImmAtomicExch: case kImmAtomicImax:
    case kImmAtomicImin: case kImmAtomicUmax: case kImmAtomicUmin:
      return 4;
    case kBfi: case kSwapc: case kSampleB: case kSampleL: case kSampleC: case kSampleCLz: case kGather4C:
    case kGather4Po: case kImmAtomicCmpExch:
      return 5;
    case kSampleD: case kGather4PoC: return 6;
    default: return 0;
  }
}

bool IsDeclOp(uint32_t op) {
  return (op >= kDclResource && op <= kDclGlobalFlags) || op == kCustomData ||
         (op >= kDclStream && op <= kDclResourceStructured) || op == kDclGsInstanceCount || op == kHsDecls;
}

class Decoder {
 public:
  Decoder(const uint32_t* code, uint32_t n) : code_(code), n_(n) {}

  bool Operand_(uint32_t end, uint32_t& pos, Operand& o) {
    if (pos >= end) return Fail("an operand past its instruction");
    const uint32_t t = code_[pos++];
    const uint32_t nc = t & 3;
    o.ncomp = nc == 0 ? 0 : nc == 1 ? 1 : nc == 2 ? 4 : 0;
    if (nc == 3) return Fail("an operand of N components");
    if (o.ncomp == 4) {
      o.mode = (t >> 2) & 3;
      if (o.mode == 0) {
        o.mask = (t >> 4) & 0xF;
      } else if (o.mode == 1) {
        for (int i = 0; i < 4; i++) o.swz[i] = (t >> (4 + 2 * i)) & 3;
      } else {
        for (int i = 0; i < 4; i++) o.swz[i] = (t >> 4) & 3;
      }
    }
    o.type = (t >> 12) & 0xFF;
    o.ndim = (t >> 20) & 3;
    uint32_t ext = t;
    while (ext >> 31) {
      if (pos >= end) return Fail("an extended operand token past its instruction");
      ext = code_[pos++];
      if ((ext & 0x3F) == 1) o.mod = (ext >> 6) & 0xFF;
    }
    if (o.type == kImm32 || o.type == kImm64) {
      const uint32_t words = (o.type == kImm64 ? 2 : 1) * (o.ncomp == 4 ? 4 : 1);
      if (o.type == kImm64 && o.ncomp == 4) return Fail("a 64-bit immediate of four components");
      if (pos + words > end) return Fail("an immediate past its instruction");
      for (uint32_t i = 0; i < words; i++) o.imm[i] = code_[pos++];
    }
    for (uint32_t d = 0; d < o.ndim; d++) {
      const uint32_t rep = (t >> (22 + 3 * d)) & 7;
      if (rep == 0 || rep == 3) {
        if (pos >= end) return Fail("an operand index past its instruction");
        o.index[d].imm = code_[pos++];
      } else if (rep == 1 || rep == 4) {
        if (pos + 2 > end) return Fail("an operand index past its instruction");
        o.index[d].imm = code_[pos];
        pos += 2;
      } else if (rep != 2) {
        return Fail("an operand index representation not known");
      }
      if (rep >= 2) {
        Operand r;
        if (!Operand_(end, pos, r)) return false;
        o.rel.push_back(std::move(r));
        o.index[d].rel = int(o.rel.size() - 1);
      }
    }
    return true;
  }

  bool Next(uint32_t& pos, Instr& in) {
    in = Instr{};
    in.at = pos;
    const uint32_t t = code_[pos];
    in.token = t;
    in.op = t & 0x7FF;
    if (in.op == kCustomData) {
      if (pos + 2 > n_ || code_[pos + 1] < 2 || code_[pos + 1] > n_ - pos)
        return Fail("customdata past the program");
      in.len = code_[pos + 1];
      in.lits.assign(code_ + pos + 2, code_ + pos + in.len);
      pos += in.len;
      return true;
    }
    in.len = (t >> 24) & 0x7F;
    if (in.len == 0 || in.len > n_ - pos) return Fail("an instruction's length is out of range");
    const uint32_t end = pos + in.len;
    in.sat = in.op != kSync && ((t >> 13) & 1);  // sync's bit 13 is one of its flags
    in.nz = (t >> 18) & 1;
    uint32_t p = pos + 1;
    uint32_t ext = t;
    while (ext >> 31) {
      if (p >= end) return Fail("an extended opcode token past its instruction");
      ext = code_[p++];
      switch (ext & 0x3F) {
        case 1:
          for (int i = 0; i < 3; i++) {
            const int v = int((ext >> (9 + 4 * i)) & 0xF);
            in.off[i] = v >= 8 ? v - 16 : v;
          }
          in.has_off = in.off[0] || in.off[1] || in.off[2];
          break;
        case 2:
          in.res_dim = (ext >> 6) & 0x1F;
          in.stride = (ext >> 11) & 0xFFF;
          break;
        case 3:
          for (int i = 0; i < 4; i++) in.ret[i] = (ext >> (6 + 4 * i)) & 0xF;
          break;
      }
    }
    const char* layout = in.op < kOpCount ? DeclLayout(in.op) : nullptr;
    if (layout && *layout == '*') {
      in.lits.assign(code_ + p, code_ + end);
      p = end;
    } else if (layout) {
      for (const char* l = layout; *l; l++) {
        if (*l == 'o') {
          Operand o;
          if (!Operand_(end, p, o)) return false;
          in.ops.push_back(std::move(o));
        } else {
          if (p >= end) return Fail("a declaration's literal past its instruction");
          in.lits.push_back(code_[p++]);
        }
      }
    } else {
      while (p < end) {
        Operand o;
        if (!Operand_(end, p, o)) return false;
        in.ops.push_back(std::move(o));
      }
    }
    if (p != end) return Fail("an instruction's tokens do not fill its length");
    if (in.ops.size() < MinOperands(in.op)) return Fail("an instruction with too few operands");
    pos = end;
    return true;
  }

  bool Fail(const char* what) {
    if (error.empty()) error = what;
    return false;
  }

  std::string error;

 private:
  const uint32_t* code_;
  uint32_t n_;
};

struct Program {
  ShaderType type = ShaderType::Pixel;
  uint32_t major = 0, minor = 0;
  std::vector<uint32_t> code;
  std::vector<Instr> instrs;
};

bool LoadProgram(const std::vector<Chunk>& chunks, Program* out, std::string* error) {
  const Chunk* shex = nullptr;
  for (const Chunk& c : chunks)
    if (c.fourcc == FourCC("SHEX") || c.fourcc == FourCC("SHDR")) shex = &c;
  if (!shex) {
    *error = "no SHEX or SHDR chunk";
    return false;
  }
  if (shex->size < 8) {
    *error = "a program chunk shorter than its header";
    return false;
  }
  const uint32_t ver = Rd32(shex->data), len = Rd32(shex->data + 4);
  if (len < 2 || len > shex->size / 4) {
    *error = "a program's length is out of range";
    return false;
  }
  out->type = ShaderType((ver >> 16) & 0xFFFF);
  out->major = (ver >> 4) & 0xF;
  out->minor = ver & 0xF;
  out->code.resize(len);
  for (uint32_t i = 0; i < len; i++) out->code[i] = Rd32(shex->data + 4 * i);
  Decoder d(out->code.data(), len);
  for (uint32_t pos = 2; pos < len;) {
    Instr in;
    if (!d.Next(pos, in)) {
      char b[96];
      std::snprintf(b, sizeof b, "at %u: ", pos);
      *error = b + d.error;
      return false;
    }
    out->instrs.push_back(std::move(in));
  }
  return true;
}

// ---- disassembly

std::string Fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* f, ...) {
  char b[512];
  va_list a;
  va_start(a, f);
  std::vsnprintf(b, sizeof b, f, a);
  va_end(a);
  return b;
}

const char* kComp = "xyzw";

std::string ImmText(uint32_t v) {
  // Floats where the bits look like one, integers otherwise (as fxc
  // prints by the instruction's type, which a listing need not know).
  if (v < 0x10000 || v >= 0xFFFF0000u) return std::to_string(int32_t(v));
  float f;
  std::memcpy(&f, &v, 4);
  return Fmt("%f", double(f));
}

std::string DisOperand(const Operand& o);

std::string DisIndex(const Operand& o, int d) {
  const Operand::Index& ix = o.index[d];
  if (ix.rel < 0) return std::to_string(ix.imm);
  std::string r = DisOperand(o.rel[ix.rel]);
  if (ix.imm) r += " + " + std::to_string(ix.imm);
  return r;
}

std::string DisOperand(const Operand& o) {
  std::string s;
  auto reg1 = [&](const char* p) {
    if (o.ndim == 0) return std::string(p);
    if (o.index[0].rel < 0 && o.ndim == 1) return p + std::to_string(o.index[0].imm);
    std::string r = p + std::string("[") + DisIndex(o, 0) + "]";
    for (uint32_t d = 1; d < o.ndim; d++) r += "[" + DisIndex(o, d) + "]";
    return r;
  };
  auto reg2 = [&](const char* p) {  // the slot then the index (cb0[3], x0[r1.x])
    std::string r = p + (o.index[0].rel < 0 ? std::to_string(o.index[0].imm) : "[" + DisIndex(o, 0) + "]");
    for (uint32_t d = 1; d < o.ndim; d++) r += "[" + DisIndex(o, d) + "]";
    return r;
  };
  switch (o.type) {
    case kTemp: s = reg1("r"); break;
    case kInput: s = reg1("v"); break;
    case kOutput: s = reg1("o"); break;
    case kIndexableTemp: s = reg2("x"); break;
    case kImm32: {
      s = "l(";
      for (uint32_t i = 0; i < (o.ncomp == 4 ? 4u : 1u); i++) s += (i ? ", " : "") + ImmText(o.imm[i]);
      s += ")";
      break;
    }
    case kImm64: s = "d(...)"; break;
    case kSamplerReg: s = reg2("s"); break;
    case kResource: s = reg2("t"); break;
    case kConstantBuffer: s = reg2("cb"); break;
    case kImmConstantBuffer: s = "icb[" + DisIndex(o, 0) + "]"; break;
    case kLabelReg: s = reg2("l"); break;
    case kInputPrimitiveId: s = "vPrim"; break;
    case kOutputDepth: s = "oDepth"; break;
    case kNull: s = "null"; break;
    case kOutputCoverageMask: s = "oMask"; break;
    case kUav: s = reg2("u"); break;
    case kTgsm: s = reg2("g"); break;
    case kInputThreadId: s = "vThreadID"; break;
    case kInputThreadGroupId: s = "vThreadGroupID"; break;
    case kInputThreadIdInGroup: s = "vThreadIDInGroup"; break;
    case kInputCoverageMask: s = "vCoverage"; break;
    case kInputThreadIdInGroupFlattened: s = "vThreadIDInGroupFlattened"; break;
    case kOutputDepthGe: s = "oDepthGE"; break;
    case kOutputDepthLe: s = "oDepthLE"; break;
    default: s = "operand" + std::to_string(o.type) + reg1(""); break;
  }
  if (o.ncomp == 4 && o.type != kImm32) {
    s += ".";
    if (o.mode == 0) {
      for (int i = 0; i < 4; i++)
        if (o.mask & (1 << i)) s += kComp[i];
    } else if (o.mode == 1) {
      for (int i = 0; i < 4; i++) s += kComp[o.swz[i]];
    } else {
      s += kComp[o.swz[0]];
    }
  }
  if (o.mod & 2) s = "|" + s + "|";
  if (o.mod & 1) s = "-" + s;
  return s;
}

const char* kDimNames[] = {"unknown", "buffer", "texture1d", "texture2d", "texture2dms", "texture3d",
                           "texturecube", "texture1darray", "texture2darray", "texture2dmsarray",
                           "texturecubearray", "raw_buffer", "structured_buffer"};
const char* kRetNames[] = {"none", "unorm", "snorm", "sint", "uint", "float", "mixed", "double",
                           "continued", "unused"};
const char* kSvNames[] = {"undefined", "position", "clip_distance", "cull_distance", "rendertarget_array_index",
                          "viewport_array_index", "vertex_id", "primitive_id", "instance_id", "is_front_face",
                          "sampleIndex"};
const char* kInterpNames[] = {"", "constant", "linear", "linear centroid", "linear noperspective",
                              "linear noperspective centroid", "linear sample", "linear noperspective sample"};

std::string DisInstr(const Instr& in) {
  if (in.op == kCustomData) {
    const uint32_t cls = in.token >> 11;
    if (cls == 3) return Fmt("dcl_immediateConstantBuffer { %zu dwords }", in.lits.size());
    return Fmt("customdata class %u { %zu dwords }", cls, in.lits.size());
  }
  std::string s = in.op < kOpCount ? kOpNames[in.op] : "op" + std::to_string(in.op);
  const uint32_t ctl = (in.token >> 11) & 0x1FFF;
  switch (in.op) {
    case kIf: case kBreakc: case kContinuec: case kRetc: case kDiscard: case kCallc:
      s += in.nz ? "_nz" : "_z";
      break;
    case kResinfo:
      if ((ctl & 3) == 1) s += "_rcpFloat";
      if ((ctl & 3) == 2) s += "_uint";
      break;
    case kSampleInfo:
      if (ctl & 1) s += "_uint";
      break;
    case kSync:
      if (ctl & 8) s += "_uglobal";
      if (ctl & 4) s += "_ugroup";
      if (ctl & 2) s += "_g";
      if (ctl & 1) s += "_t";
      break;
    case kDclResource: case kDclUavTyped:
      s += Fmt("_%s", kDimNames[std::min(ctl & 0x1F, 12u)]);
      break;
    case kDclSampler:
      if ((ctl & 0xF) == 1) s += "_comparison";
      break;
    case kDclConstantBuffer:
      if (ctl & 1) s += " dynamicIndexed,";
      break;
    case kDclInputPs: case kDclInputPsSiv: case kDclInputPsSgv:
      if ((ctl & 0xF) < 8 && (ctl & 0xF)) s += Fmt(" %s", kInterpNames[ctl & 0xF]);
      break;
    case kDclGlobalFlags:
      s += Fmt(" 0x%x", ctl);
      break;
    default: break;
  }
  if (in.sat && !IsDeclOp(in.op)) s += "_sat";
  if (in.has_off) s += Fmt("_aoffimmi(%d,%d,%d)", in.off[0], in.off[1], in.off[2]);
  if (in.res_dim) s += Fmt("_indexable(%s)", kDimNames[std::min(in.res_dim, 12u)]);
  if (in.stride) s += Fmt("(%u)", in.stride);
  if (in.ret[0])
    s += Fmt("(%s,%s,%s,%s)", kRetNames[std::min(in.ret[0], 9u)], kRetNames[std::min(in.ret[1], 9u)],
             kRetNames[std::min(in.ret[2], 9u)], kRetNames[std::min(in.ret[3], 9u)]);
  for (size_t i = 0; i < in.ops.size(); i++) s += (i ? ", " : " ") + DisOperand(in.ops[i]);
  for (size_t i = 0; i < in.lits.size(); i++) {
    const uint32_t v = in.lits[i];
    s += in.ops.empty() && i == 0 ? " " : ", ";
    const bool sv = (in.op >= kDclInputSgv && in.op <= kDclInputSiv) || in.op == kDclInputPsSgv ||
                    in.op == kDclInputPsSiv || in.op == kDclOutputSgv || in.op == kDclOutputSiv;
    if (sv && v < 11) s += kSvNames[v];
    else if (in.op == kDclResource || in.op == kDclUavTyped)
      s += Fmt("(%s,%s,%s,%s)", kRetNames[std::min(v & 0xF, 9u)], kRetNames[std::min((v >> 4) & 0xF, 9u)],
               kRetNames[std::min((v >> 8) & 0xF, 9u)], kRetNames[std::min((v >> 12) & 0xF, 9u)]);
    else s += std::to_string(v);
  }
  return s;
}

}  // namespace

std::string Disassemble(const void* dxbc, size_t size, std::string* error) {
  std::string err;
  std::vector<Chunk> chunks;
  Program prog;
  if (!Chunks(static_cast<const uint8_t*>(dxbc), size, &chunks, &err) || !LoadProgram(chunks, &prog, &err)) {
    if (error) *error = err;
    return {};
  }
  static const char* kTypes[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
  std::string s = Fmt("%s_%u_%u\n", uint32_t(prog.type) < 6 ? kTypes[uint32_t(prog.type)] : "?", prog.major,
                      prog.minor);
  int depth = 0;
  for (const Instr& in : prog.instrs) {
    if (in.op == kElse || in.op == kEndif || in.op == kEndloop) depth = std::max(depth - 1, 0);
    if (in.op == kEndswitch) depth = std::max(depth - 2, 0);
    const int at = in.op == kCase || in.op == kDefault ? std::max(depth - 1, 0) : depth;
    s += Fmt("%5u: %*s", in.at, 2 * at, "") + DisInstr(in) + "\n";
    if (in.op == kIf || in.op == kElse || in.op == kLoop) depth++;
    if (in.op == kSwitch) depth += 2;
  }
  return s;
}

namespace {

// ---- translation

std::string TypeN(char t, int n) {
  static const char* const kNames[4][4] = {{"float", "vec2", "vec3", "vec4"},
                                           {"int", "ivec2", "ivec3", "ivec4"},
                                           {"uint", "uvec2", "uvec3", "uvec4"},
                                           {"bool", "bvec2", "bvec3", "bvec4"}};
  return kNames[t == 'f' ? 0 : t == 'i' ? 1 : t == 'u' ? 2 : 3][n - 1];
}
std::string Zero(char t) { return t == 'f' ? "0.0" : t == 'i' ? "0" : "0u"; }

std::string UintLit(uint32_t v) { return v < 0x10000 ? std::to_string(v) + "u" : Fmt("0x%Xu", v); }
std::string IntLit(uint32_t v) {
  const int32_t i = int32_t(v);
  if (v == 0x80000000u) return "int(0x80000000u)";
  return i < 0 ? "(" + std::to_string(i) + ")" : std::to_string(i);
}
// A float literal with the same bits; denormals, infinities, NaNs and
// -0 as bits.
std::string FloatLit(uint32_t v) {
  const uint32_t e = (v >> 23) & 0xFF;
  if (e == 0xFF || (e == 0 && v != 0)) return Fmt("uintBitsToFloat(0x%08Xu)", v);
  float f;
  std::memcpy(&f, &v, 4);
  std::string s = Fmt("%.9g", double(f));
  if (s.find_first_of(".e") == std::string::npos) s += ".0";
  return f < 0 ? "(" + s + ")" : s;
}
std::string Lit(uint32_t v, char t) { return t == 'f' ? FloatLit(v) : t == 'i' ? IntLit(v) : UintLit(v); }

// An expression of type `from` (f, i, u; b: bool) as `to`, keeping bits.
std::string Conv(const std::string& e, char from, char to, int n) {
  if (from == to) return e;
  if (from == 'b') {
    const std::string b = n == 1 ? "(" + e + " ? 0xFFFFFFFFu : 0u)"
                                 : "mix(" + TypeN('u', n) + "(0u), " + TypeN('u', n) + "(0xFFFFFFFFu), " + e + ")";
    return Conv(b, 'u', to, n);
  }
  if (from == 'u' && to == 'f') return "uintBitsToFloat(" + e + ")";
  if (from == 'f' && to == 'u') return "floatBitsToUint(" + e + ")";
  if (from == 'i' && to == 'f') return "intBitsToFloat(" + e + ")";
  if (from == 'f' && to == 'i') return "floatBitsToInt(" + e + ")";
  return TypeN(to, n) + "(" + e + ")";  // between int and uint
}

// A 1-3 component expression padded to four with zeros, and four
// components cut back to n.
std::string Widen(const std::string& e, char t, int n) {
  if (n == 4) return e;
  std::string r = TypeN(t, 4) + "(" + e;
  for (int i = n; i < 4; i++) r += ", " + Zero(t);
  return r + ")";
}
std::string Narrow(const std::string& e, int n) {
  return n == 4 ? e : e + "." + std::string(kComp, size_t(n));
}
std::string Chars(const int* c, int n) {
  std::string s;
  for (int i = 0; i < n; i++) s += kComp[c[i]];
  return s;
}

const int kXyzw[4] = {0, 1, 2, 3};


// GLSL the translation calls, put before it when used.
const char* const kHelpers[][2] = {
    {"r_f2i",
     "ivec4 r_f2i(vec4 a) {\n"
     "  ivec4 r = mix(ivec4(clamp(a, -2147483648.0, 2147483520.0)), ivec4(0x7FFFFFFF),\n"
     "                greaterThanEqual(a, vec4(2147483648.0)));\n"
     "  return mix(r, ivec4(0), isnan(a));\n}\n"},
    {"r_f2u",
     "uvec4 r_f2u(vec4 a) {\n"
     "  uvec4 r = mix(uvec4(clamp(a, 0.0, 4294967040.0)), uvec4(0xFFFFFFFFu),\n"
     "                greaterThanEqual(a, vec4(4294967296.0)));\n"
     "  return mix(r, uvec4(0u), isnan(a));\n}\n"},
    {"r_f2h",
     "uvec4 r_f2h(vec4 a) {\n"
     "  return uvec4(packHalf2x16(vec2(a.x, 0.0)), packHalf2x16(vec2(a.y, 0.0)),\n"
     "               packHalf2x16(vec2(a.z, 0.0)), packHalf2x16(vec2(a.w, 0.0)));\n}\n"},
    {"r_h2f",
     "vec4 r_h2f(uvec4 a) {\n"
     "  return vec4(unpackHalf2x16(a.x & 0xFFFFu).x, unpackHalf2x16(a.y & 0xFFFFu).x,\n"
     "              unpackHalf2x16(a.z & 0xFFFFu).x, unpackHalf2x16(a.w & 0xFFFFu).x);\n}\n"},
    // ubfe/ibfe: a field of width w at offset o (5 bits each); one past
    // bit 31 is v shifted right by o (GLSL leaves that undefined).
    {"r_ubfe",
     "uvec4 r_ubfe(uvec4 w, uvec4 o, uvec4 v) {\n  uvec4 r;\n  for (int i = 0; i < 4; i++) {\n"
     "    uint wi = w[i] & 31u, oi = o[i] & 31u;\n"
     "    r[i] = wi == 0u ? 0u : wi + oi < 32u ? bitfieldExtract(v[i], int(oi), int(wi)) : v[i] >> oi;\n"
     "  }\n  return r;\n}\n"},
    {"r_ibfe",
     "uvec4 r_ibfe(uvec4 w, uvec4 o, uvec4 v) {\n  uvec4 r;\n  for (int i = 0; i < 4; i++) {\n"
     "    uint wi = w[i] & 31u, oi = o[i] & 31u;\n"
     "    r[i] = uint(wi == 0u ? 0 : wi + oi < 32u ? bitfieldExtract(int(v[i]), int(oi), int(wi))\n"
     "                                              : int(v[i]) >> int(oi));\n"
     "  }\n  return r;\n}\n"},
    {"r_bfi",
     "uvec4 r_bfi(uvec4 w, uvec4 o, uvec4 i, uvec4 b) {\n  uvec4 wi = w & 31u, oi = o & 31u;\n"
     "  uvec4 m = ((uvec4(1u) << wi) - 1u) << oi;\n  return ((i << oi) & m) | (b & ~m);\n}\n"},
    // firstbit_hi/shi count from the most significant bit.
    {"r_fbh",
     "uvec4 r_fbh(uvec4 a) {\n  ivec4 m = findMSB(a);\n"
     "  return mix(uvec4(31) - uvec4(m), uvec4(0xFFFFFFFFu), equal(m, ivec4(-1)));\n}\n"},
    {"r_fbsh",
     "uvec4 r_fbsh(uvec4 a) {\n  ivec4 m = findMSB(ivec4(a));\n"
     "  return mix(uvec4(31) - uvec4(m), uvec4(0xFFFFFFFFu), equal(m, ivec4(-1)));\n}\n"},
    // A constant buffer read at a run-time index: 0 past the buffer.
    {"r_cbx",
     "uint r_cbx(uint b, uint i, uint c) {\n  return i < (r_buf[b].y >> 4) ? r_ld(b, i * 4u + c) : 0u;\n}\n"},
    // Buffer SRVs' and UAVs' words by byte address: 0 or dropped past
    // their size.
    {"r_sld", "uint r_sld(uint b, uint a) {\n  return a < r_buf[b].y ? r_ld(b, a >> 2) : 0u;\n}\n"},
    {"r_uld",
     "uint r_uld(uint b, uint a) {\n  return a < r_rwbuf[b].y ? r_rw[r_rwbuf[b].x + (a >> 2)] : 0u;\n}\n"},
    {"r_ust",
     "void r_ust(uint b, uint a, uint v) {\n  if (a < r_rwbuf[b].y) r_rw[r_rwbuf[b].x + (a >> 2)] = v;\n}\n"},
    // A cube array's direction and cube index as a coordinate in the 2D
    // array of its faces (six layers a cube, in Direct3D's order): the
    // major axis picks the face (z, then y, then x on ties).
    {"r_cubearr",
     "vec3 r_cubearr(vec4 c) {\n  vec3 a = abs(c.xyz);\n  float f, m;\n  vec2 st;\n"
     "  if (a.z >= a.x && a.z >= a.y) {\n    f = c.z < 0.0 ? 5.0 : 4.0; m = a.z; st = vec2(c.z < 0.0 ? -c.x : c.x, -c.y);\n"
     "  } else if (a.y >= a.x) {\n    f = c.y < 0.0 ? 3.0 : 2.0; m = a.y; st = vec2(c.x, c.y < 0.0 ? -c.z : c.z);\n"
     "  } else {\n    f = c.x < 0.0 ? 1.0 : 0.0; m = a.x; st = vec2(c.x < 0.0 ? c.z : -c.z, -c.y);\n  }\n"
     "  return vec3(st / (2.0 * m) + 0.5, max(floor(c.w + 0.5), 0.0) * 6.0 + f);\n}\n"},
    // Typed buffer formats (DXGI_FORMAT): bits per component (0: 10, 10,
    // 10, 2), components, kind (0 float, 1 unorm, 2 snorm, 3 uint, 4 sint)
    // and whether red and blue are swapped.
    {"r_fmt",
     "uvec4 r_fmt(uint f) {\n  switch (f) {\n"
     "    case 2u: return uvec4(32u, 4u, 0u, 0u); case 3u: return uvec4(32u, 4u, 3u, 0u);\n"
     "    case 4u: return uvec4(32u, 4u, 4u, 0u); case 6u: return uvec4(32u, 3u, 0u, 0u);\n"
     "    case 7u: return uvec4(32u, 3u, 3u, 0u); case 8u: return uvec4(32u, 3u, 4u, 0u);\n"
     "    case 10u: return uvec4(16u, 4u, 0u, 0u); case 11u: return uvec4(16u, 4u, 1u, 0u);\n"
     "    case 12u: return uvec4(16u, 4u, 3u, 0u); case 13u: return uvec4(16u, 4u, 2u, 0u);\n"
     "    case 14u: return uvec4(16u, 4u, 4u, 0u); case 16u: return uvec4(32u, 2u, 0u, 0u);\n"
     "    case 17u: return uvec4(32u, 2u, 3u, 0u); case 18u: return uvec4(32u, 2u, 4u, 0u);\n"
     "    case 24u: return uvec4(0u, 4u, 1u, 0u); case 25u: return uvec4(0u, 4u, 3u, 0u);\n"
     "    case 28u: return uvec4(8u, 4u, 1u, 0u); case 30u: return uvec4(8u, 4u, 3u, 0u);\n"
     "    case 31u: return uvec4(8u, 4u, 2u, 0u); case 32u: return uvec4(8u, 4u, 4u, 0u);\n"
     "    case 34u: return uvec4(16u, 2u, 0u, 0u); case 35u: return uvec4(16u, 2u, 1u, 0u);\n"
     "    case 36u: return uvec4(16u, 2u, 3u, 0u); case 37u: return uvec4(16u, 2u, 2u, 0u);\n"
     "    case 38u: return uvec4(16u, 2u, 4u, 0u); case 41u: return uvec4(32u, 1u, 0u, 0u);\n"
     "    case 42u: return uvec4(32u, 1u, 3u, 0u); case 43u: return uvec4(32u, 1u, 4u, 0u);\n"
     "    case 49u: return uvec4(8u, 2u, 1u, 0u); case 50u: return uvec4(8u, 2u, 3u, 0u);\n"
     "    case 51u: return uvec4(8u, 2u, 2u, 0u); case 52u: return uvec4(8u, 2u, 4u, 0u);\n"
     "    case 54u: return uvec4(16u, 1u, 0u, 0u); case 56u: return uvec4(16u, 1u, 1u, 0u);\n"
     "    case 57u: return uvec4(16u, 1u, 3u, 0u); case 58u: return uvec4(16u, 1u, 2u, 0u);\n"
     "    case 59u: return uvec4(16u, 1u, 4u, 0u); case 61u: return uvec4(8u, 1u, 1u, 0u);\n"
     "    case 62u: return uvec4(8u, 1u, 3u, 0u); case 63u: return uvec4(8u, 1u, 2u, 0u);\n"
     "    case 64u: return uvec4(8u, 1u, 4u, 0u); case 87u: return uvec4(8u, 4u, 1u, 1u);\n"
     "  }\n  return uvec4(0u);\n}\n"
     "uint r_fmtsize(uint f) {\n  uvec4 i = r_fmt(f);\n  return i.x == 0u ? 4u : i.x / 8u * i.y;\n}\n"
     // Component c's bits and position in an element (from bit sh).
     "uint r_fmtbits(uvec4 i, uint c) { return i.x != 0u ? i.x : c == 3u ? 2u : 10u; }\n"
     "uint r_fmtpos(uvec4 i, uint c, uint sh) { return sh + (i.x != 0u ? i.x : 10u) * c; }\n"
     // An element's raw components (64 bits from word w0, its first
     // byte at bit sh) as the format's values' bits; missing ones 0, 0,
     // 0, 1.
     "uvec4 r_fmtdec(uvec4 i, uint w0, uint w1, uint w2, uint w3, uint sh) {\n"
     "  uvec4 r = uvec4(0u, 0u, 0u, i.z == 0u || i.z == 1u || i.z == 2u ? 0x3F800000u : 1u);\n"
     "  for (uint c = 0u; c < i.y; c++) {\n"
     "    uint v;\n"
     "    if (i.x == 32u) {\n      v = c == 0u ? w0 : c == 1u ? w1 : c == 2u ? w2 : w3;\n"
     "    } else {\n"
     "      uint p = r_fmtpos(i, c, sh), n = r_fmtbits(i, c);\n"
     "      v = ((p < 32u ? w0 : w1) >> (p & 31u)) & ((1u << n) - 1u);\n"
     "      if (i.z == 0u) v = floatBitsToUint(unpackHalf2x16(v).x);\n"
     "      else if (i.z == 1u) v = floatBitsToUint(float(v) / float((1u << n) - 1u));\n"
     "      else if (i.z == 2u) v = floatBitsToUint(max(float(bitfieldExtract(int(v), 0, int(n))) /\n"
     "                                                   float((1u << (n - 1u)) - 1u), -1.0));\n"
     "      else if (i.z == 4u) v = uint(bitfieldExtract(int(v), 0, int(n)));\n"
     "    }\n    r[c] = v;\n  }\n"
     "  return i.w != 0u ? r.zyxw : r;\n}\n"
     // A value's bits as component c of the format (n bits).
     "uint r_fmtenc(uvec4 i, uint c, uint v) {\n"
     "  if (i.x == 32u) return v;\n  uint n = r_fmtbits(i, c), m = (1u << n) - 1u;\n"
     "  float f = uintBitsToFloat(v);\n"
     "  if (i.z == 0u) return packHalf2x16(vec2(f, 0.0));\n"
     "  if (i.z == 1u) return uint(roundEven(clamp(f, 0.0, 1.0) * float(m)));\n"
     "  if (i.z == 2u) return uint(int(roundEven(clamp(f, -1.0, 1.0) * float(m >> 1)))) & m;\n"
     // Integers narrower than their value are clamped.
     "  if (i.z == 3u) return min(v, m);\n"
     "  return uint(clamp(int(v), -int(m >> 1) - 1, int(m >> 1))) & m;\n}\n"},
    {"r_tld",
     "uvec4 r_tld(uint b, uint f, uint e) {\n  uvec4 i = r_fmt(f);\n  uint s = r_fmtsize(f), a = e * s;\n"
     "  if (i.y == 0u || a + s > r_buf[b].y) return uvec4(0u);\n  uint w = a >> 2;\n"
     "  return r_fmtdec(i, r_ld(b, w), s > 4u ? r_ld(b, w + 1u) : 0u, s > 8u ? r_ld(b, w + 2u) : 0u,\n"
     "                  s > 12u ? r_ld(b, w + 3u) : 0u, (a & 3u) * 8u);\n}\n"},
    {"r_utld",
     "uvec4 r_utld(uint b, uint f, uint e) {\n  uvec4 i = r_fmt(f);\n  uint s = r_fmtsize(f), a = e * s;\n"
     "  if (i.y == 0u || a + s > r_rwbuf[b].y) return uvec4(0u);\n  uint w = r_rwbuf[b].x + (a >> 2);\n"
     "  return r_fmtdec(i, r_rw[w], s > 4u ? r_rw[w + 1u] : 0u, s > 8u ? r_rw[w + 2u] : 0u,\n"
     "                  s > 12u ? r_rw[w + 3u] : 0u, (a & 3u) * 8u);\n}\n"},
    // Stores of components narrower than a word keep the word's other
    // bits (atomically: neighbours may be stored at the same time).
    {"r_utst",
     "void r_utst(uint b, uint f, uint e, uvec4 v) {\n  uvec4 i = r_fmt(f);\n  uint s = r_fmtsize(f), a = e * s;\n"
     "  if (i.y == 0u || a + s > r_rwbuf[b].y) return;\n  if (i.w != 0u) v = v.zyxw;\n"
     "  uint w = r_rwbuf[b].x + (a >> 2), sh = (a & 3u) * 8u;\n"
     "  for (uint c = 0u; c < i.y; c++) {\n    uint x = r_fmtenc(i, c, v[c]);\n"
     "    if (i.x == 32u) {\n      r_rw[w + c] = x;\n      continue;\n    }\n"
     "    uint p = r_fmtpos(i, c, sh), n = r_fmtbits(i, c), m = ((1u << n) - 1u) << (p & 31u);\n"
     "    atomicAnd(r_rw[w + (p >> 5)], ~m);\n    atomicOr(r_rw[w + (p >> 5)], (x << (p & 31u)) & m);\n"
     "  }\n}\n"},
};

class Translator {
 public:
  Translator(const Program& p, Shader* sh, bool annotate) : p_(p), sh_(sh), annotate_(annotate) {}

  bool Run() {
    sh_->type = p_.type;
    sh_->major = p_.major;
    sh_->minor = p_.minor;
    stage_ = p_.type;
    if (stage_ != ShaderType::Vertex && stage_ != ShaderType::Pixel && stage_ != ShaderType::Compute &&
        stage_ != ShaderType::Domain)
      return Fail("a shader type not translated (vertex, pixel, compute and domain are)");
    for (const Instr& in : p_.instrs) {
      cur_ = &in;
      if (IsDeclOp(in.op) && !Declare(in)) return false;
    }
    cur_ = nullptr;
    if (!Bind()) return false;
    for (const Instr& in : p_.instrs) {
      cur_ = &in;
      if (!IsDeclOp(in.op)) {
        if (annotate_) {
          const bool label = last_label_;
          Line(Fmt("// %u: ", in.at) + DisInstr(in));
          last_label_ = label;
        }
        Emit(in);
      }
      if (!error_.empty()) return false;
    }
    cur_ = nullptr;
    if (!flow_.empty()) return Fail("a block without its end");
    return Finish();
  }

  std::string error_;

 private:
  bool Fail(const std::string& what) {
    if (!error_.empty()) return false;
    error_ = what;
    if (cur_) error_ += Fmt(" (at %u: ", cur_->at) + DisInstr(*cur_) + ")";
    return false;
  }

  void Line(const std::string& s) {
    body_.append(size_t(2 * indent_), ' ');
    body_ += s;
    body_ += '\n';
    last_label_ = false;
  }
  void Use(const char* helper) { helpers_.insert(helper); }
  void Open(char block) {
    flow_.push_back(block);
    indent_++;
    if (flow_.size() > 64) Fail("blocks nested past 64");
  }

  // ---- declarations

  struct IoDecl {
    uint8_t mask = 0;
    uint32_t sv = 0;      // dcl_*_sgv/siv's name
    uint32_t interp = 0;  // pixel inputs
  };

  bool Declare(const Instr& in) {
    const uint32_t ctl = (in.token >> 11) & 0x1FFF;
    auto slot = [&]() { return in.ops[0].index[0].imm; };
    switch (in.op) {
      case kCustomData:
        if ((in.token >> 11) == 3) {
          if (in.lits.size() % 4 || in.lits.size() > 4 * 4096)
            return Fail("an immediate constant buffer of a partial vector or past 4096 vectors");
          icb_ = in.lits;
        }
        return true;
      case kDclGlobalFlags:
        early_depth_ = (ctl >> 2) & 1;
        if ((ctl >> 1) & 1) return Fail("double-precision instructions not translated yet");
        return true;
      case kDclConstantBuffer: {
        ConstantBuffer cb;
        cb.slot = slot();
        cb.size = in.ops[0].index[1].imm;
        cb.dynamic = ctl & 1;
        if (cb.size > 4096 || cb.slot >= 15) return Fail("a constant buffer past Direct3D 11's limits");
        sh_->cbuffers.push_back(cb);
        return true;
      }
      case kDclSampler:
        sh_->samplers.push_back({slot(), (ctl & 0xF) == 1});
        return true;
      case kDclResource: case kDclResourceRaw: case kDclResourceStructured: {
        ShaderResource r;
        r.slot = slot();
        r.dim = in.op == kDclResourceRaw ? ResourceDim::RawBuffer
                : in.op == kDclResourceStructured ? ResourceDim::StructuredBuffer
                                                  : ResourceDim(std::min(ctl & 0x1F, 13u));
        if (in.op == kDclResource) r.ret = ReturnType(std::min(in.lits[0] & 0xF, 9u));
        if (in.op == kDclResourceStructured) r.stride = in.lits[0];
        if (r.dim == ResourceDim::Unknown || uint32_t(r.dim) > uint32_t(ResourceDim::StructuredBuffer))
          return Fail("a resource of an unknown dimension");
        sh_->resources.push_back(r);
        return true;
      }
      case kDclUavTyped: case kDclUavRaw: case kDclUavStructured: {
        UnorderedAccess u;
        u.slot = slot();
        u.dim = in.op == kDclUavRaw ? ResourceDim::RawBuffer
                : in.op == kDclUavStructured ? ResourceDim::StructuredBuffer
                                             : ResourceDim(std::min(ctl & 0x1F, 13u));
        if (in.op == kDclUavTyped) u.ret = ReturnType(std::min(in.lits[0] & 0xF, 9u));
        if (in.op == kDclUavStructured) {
          u.stride = in.lits[0];
          u.counter = (in.token >> 23) & 1;
        }
        if (stage_ != ShaderType::Compute) return Fail("UAVs outside compute shaders not translated yet");
        if (u.dim == ResourceDim::Unknown || u.dim == ResourceDim::Texture2DMS ||
            u.dim == ResourceDim::Texture2DMSArray || u.dim == ResourceDim::TextureCube ||
            u.dim == ResourceDim::TextureCubeArray || uint32_t(u.dim) > uint32_t(ResourceDim::StructuredBuffer))
          return Fail("a UAV of this dimension");
        sh_->uavs.push_back(u);
        return true;
      }
      case kDclTgsmRaw: case kDclTgsmStructured: {
        const uint64_t bytes = in.op == kDclTgsmRaw ? in.lits[0] : uint64_t(in.lits[0]) * in.lits[1];
        if (bytes == 0 || bytes + sh_->tgsm_bytes > 32768) return Fail("group-shared memory past 32 KiB");
        tgsm_[slot()] = {uint32_t(bytes + 3) / 4, in.op == kDclTgsmRaw ? 0 : in.lits[0]};
        sh_->tgsm_bytes += uint32_t(bytes);
        return true;
      }
      // Direct3D 11's limits: 4096 temps and indexable temps' vectors
      // together, groups of 1024 threads.
      case kDclTemps:
        temps_ = in.lits[0];
        if (temps_ > 4096) return Fail("more than 4096 temps");
        return true;
      case kDclIndexableTemp: {
        xtemps_[in.lits[0]] = in.lits[1];
        uint64_t n = 0;
        for (const auto& [reg, size] : xtemps_) n += size;
        if (in.lits[0] >= 4096 || in.lits[1] == 0 || n > 4096) return Fail("indexable temps past 4096 vectors");
        return true;
      }
      case kDclThreadGroup:
        for (int i = 0; i < 3; i++) sh_->local_size[i] = in.lits[i];
        if (!in.lits[0] || !in.lits[1] || !in.lits[2] || in.lits[0] > 1024 || in.lits[1] > 1024 ||
            in.lits[2] > 64 || uint64_t(in.lits[0]) * in.lits[1] * in.lits[2] > 1024)
          return Fail("a thread group past 1024 threads");
        return true;
      case kDclIndexRange:
        return true;  // v# and o# are arrays
      case kDclInputControlPointCount:
        if (stage_ == ShaderType::Domain && ((in.token >> 11) & 0x3F) != 1)
          return Fail("a domain shader of patches of more than one control point");
        return true;
      case kDclTessDomain:
        if (stage_ == ShaderType::Domain && (ctl & 3) != 3) return Fail("a domain other than quads");
        return true;
      case kDclInput: case kDclInputSgv: case kDclInputSiv: case kDclInputPs: case kDclInputPsSgv:
      case kDclInputPsSiv: case kDclOutput: case kDclOutputSgv: case kDclOutputSiv: {
        const Operand& o = in.ops[0];
        const bool out = in.op >= kDclOutput;
        // A domain shader's control point (its only one) is its inputs.
        const bool point = !out && o.type == kInputControlPoint && stage_ == ShaderType::Domain;
        if (o.type == (out ? kOutput : kInput) || point) {
          if (point && (o.ndim != 2 || o.index[1].rel >= 0)) return Fail("a control point input of this form");
          if (!point && (o.ndim != 1 || o.index[0].rel >= 0))
            return Fail("an input or output of more than one index");
          const uint32_t reg = o.index[point ? 1 : 0].imm;
          if (reg >= 32) return Fail("an input or output register past 31");
          // A register's components can be declared apart (an input and
          // a system value in one register).
          IoDecl d;
          d.mask = o.ncomp == 4 ? o.mask : 1;
          if (!d.mask) return Fail("an input or output of no components");
          if (!in.lits.empty()) d.sv = in.lits[0];
          if (in.op >= kDclInputPs && in.op <= kDclInputPsSiv) d.interp = ctl & 0xF;
          std::vector<IoDecl>& v = (out ? outs_ : ins_)[reg];
          for (IoDecl& e : v)
            if (e.sv == d.sv && e.interp == d.interp) {
              e.mask |= d.mask;
              return true;
            }
          v.push_back(d);
          return true;
        }
        switch (o.type) {
          case kOutputDepth: case kOutputDepthGe: case kOutputDepthLe:
            out_depth_ = o.type;
            sh_->writes_depth = true;
            return true;
          case kOutputCoverageMask: out_mask_ = true; return true;
          case kInputCoverageMask:
            // Read in main(): SPIR-V to MSL cannot pass it to a function.
            coverage_ = true;
            return true;
          case kInputThreadId: case kInputThreadGroupId: case kInputThreadIdInGroup:
          case kInputThreadIdInGroupFlattened: case kInputPrimitiveId:
            return true;
          case kInputDomainPoint:
            domain_point_ = true;
            return true;
          default: return Fail("an input or output of this type not translated yet");
        }
      }
      default:
        return Fail("a declaration not translated yet");
    }
  }

  // ---- bindings

  ShaderResource* Srv(uint32_t slot) {
    for (ShaderResource& r : sh_->resources)
      if (r.slot == slot) return &r;
    return nullptr;
  }
  UnorderedAccess* Uav(uint32_t slot) {
    for (UnorderedAccess& u : sh_->uavs)
      if (u.slot == slot) return &u;
    return nullptr;
  }
  static bool IsBuffer(ResourceDim d) {
    return d == ResourceDim::Buffer || d == ResourceDim::RawBuffer || d == ResourceDim::StructuredBuffer;
  }

  // Gfx buffers, textures (the pairs the code samples with) and images.
  bool Bind() {
    auto by_slot = [](const auto& a, const auto& b) { return a.slot < b.slot; };
    std::sort(sh_->cbuffers.begin(), sh_->cbuffers.end(), by_slot);
    std::sort(sh_->resources.begin(), sh_->resources.end(), by_slot);
    std::sort(sh_->uavs.begin(), sh_->uavs.end(), by_slot);
    std::sort(sh_->samplers.begin(), sh_->samplers.end(), by_slot);
    int nbuf = 0;
    for (const ConstantBuffer& cb : sh_->cbuffers) cb_[cb.slot] = nbuf++;
    for (ShaderResource& r : sh_->resources)
      if (IsBuffer(r.dim)) r.buffer = nbuf++;
    int nrw = 0, nimg = 0;
    for (UnorderedAccess& u : sh_->uavs) {
      if (IsBuffer(u.dim)) u.buffer = nrw++;
      else u.image = nimg++;
    }
    // A counter: declared (RWStructuredBuffer's), or used (an append or
    // consume buffer's, declared without the flag).
    for (const Instr& in : p_.instrs)
      if ((in.op == kImmAtomicAlloc || in.op == kImmAtomicConsume) && in.ops[1].type == kUav)
        if (UnorderedAccess* u = Uav(in.ops[1].index[0].imm)) u->counter = true;
    for (UnorderedAccess& u : sh_->uavs)
      if (u.counter) u.counter_buffer = nrw++;
    sh_->stage.buffers = uint32_t(nbuf);
    sh_->stage.rw_buffers = uint32_t(nrw);
    sh_->stage.rw_images = uint32_t(nimg);

    // Each (texture, sampler, comparison) the code samples with; loads
    // and queries take one of the texture's pairs without comparison, or
    // a pair of their own.
    std::set<uint32_t> loads;
    for (const Instr& in : p_.instrs) {
      cur_ = &in;
      int res = -1, smp = -1;
      bool cmp = false;
      switch (in.op) {
        case kSample: case kSampleB: case kSampleL: case kSampleD: case kGather4: case kLod:
          res = 2, smp = 3;
          break;
        case kSampleC: case kSampleCLz: case kGather4C:
          res = 2, smp = 3, cmp = true;
          break;
        case kGather4Po: res = 3, smp = 4; break;
        case kGather4PoC: res = 3, smp = 4, cmp = true; break;
        case kLd: case kLdMs: res = 2; break;
        case kResinfo: res = 2; break;
        case kSampleInfo: res = 1; break;
        case kLdUavTyped:
          if (UnorderedAccess* u = Uav(in.ops[2].index[0].imm)) u->read = true;
          continue;
        case kAtomicAnd: case kAtomicOr: case kAtomicXor: case kAtomicCmpStore: case kAtomicIadd:
        case kAtomicImax: case kAtomicImin: case kAtomicUmax: case kAtomicUmin:
          if (in.ops[0].type == kUav)
            if (UnorderedAccess* u = Uav(in.ops[0].index[0].imm)) u->read = true;
          continue;
        case kImmAtomicIadd: case kImmAtomicAnd: case kImmAtomicOr: case kImmAtomicXor:
        case kImmAtomicExch: case kImmAtomicCmpExch: case kImmAtomicImax: case kImmAtomicImin:
        case kImmAtomicUmax: case kImmAtomicUmin:
          if (in.ops[1].type == kUav)
            if (UnorderedAccess* u = Uav(in.ops[1].index[0].imm)) u->read = true;
          continue;
        default: continue;
      }
      if (size_t(res) >= in.ops.size() || (smp >= 0 && size_t(smp) >= in.ops.size()))
        return Fail("an instruction with too few operands");
      const Operand& r = in.ops[size_t(res)];
      if (r.type != kResource) continue;  // sampleinfo on the rasterizer, resinfo of a UAV
      if (r.index[0].rel >= 0) return Fail("a resource at a run-time index");
      const ShaderResource* sr = Srv(r.index[0].imm);
      if (!sr) return Fail("a resource not declared");
      if (IsBuffer(sr->dim)) continue;
      if (smp < 0) {
        loads.insert(sr->slot);
        continue;
      }
      if (in.ops[size_t(smp)].type != kSamplerReg) return Fail("a sampler operand of the wrong type");
      pairs_[{sr->slot, int(in.ops[size_t(smp)].index[0].imm), cmp}] = 0;
    }
    cur_ = nullptr;
    for (uint32_t t : loads) {
      bool found = false;
      for (const auto& [k, n] : pairs_) found |= std::get<0>(k) == t && !std::get<2>(k);
      if (!found) pairs_[{t, -1, false}] = 0;
    }
    int ntex = 0;
    for (auto& [k, n] : pairs_) {
      n = ntex++;
      sh_->textures.push_back({std::get<0>(k), std::get<1>(k), std::get<2>(k)});
    }
    if (ntex > 16) {
      // More than the device binds with their samplers: the samplers
      // apart, each (slot, comparison) once, and texel loads with any.
      if (stage_ == ShaderType::Compute || ntex > int(gfx::kMaxStageTextures))
        return Fail(Fmt("%d textures (the device binds 16, or %u with samplers apart)", ntex,
                        gfx::kMaxStageTextures));
      auto apart = [&](int slot, bool cmp) {
        for (size_t k = 0; k < sh_->samplers_apart.size(); k++)
          if (sh_->samplers_apart[k].slot == slot && sh_->samplers_apart[k].comparison == cmp) return int(k);
        sh_->samplers_apart.push_back({slot, cmp});
        return int(sh_->samplers_apart.size() - 1);
      };
      for (TextureBinding& t : sh_->textures)
        if (t.sampler >= 0) t.apart = apart(t.sampler, t.comparison);
      for (TextureBinding& t : sh_->textures)
        if (t.sampler < 0) {
          for (size_t k = 0; k < sh_->samplers_apart.size() && t.apart < 0; k++)
            if (!sh_->samplers_apart[k].comparison) t.apart = int(k);
          if (t.apart < 0) t.apart = apart(-1, false);
        }
      if (sh_->samplers_apart.size() > 16) return Fail("more than 16 samplers");
      sh_->stage.samplers = uint32_t(sh_->samplers_apart.size());
    }
    sh_->stage.textures = uint32_t(ntex);
    for (int i = 0; i < ntex; i++) {
      const ShaderResource* r = Srv(sh_->textures[size_t(i)].resource);
      gfx::TextureKind k = gfx::TextureKind::Tex2D;
      switch (r->dim) {
        case ResourceDim::Texture1DArray: case ResourceDim::Texture2DArray:
        case ResourceDim::Texture2DMSArray: case ResourceDim::TextureCubeArray:
          k = gfx::TextureKind::Tex2DArray;
          break;
        case ResourceDim::Texture3D: k = gfx::TextureKind::Tex3D; break;
        case ResourceDim::TextureCube: k = gfx::TextureKind::Cube; break;
        default: break;
      }
      sh_->stage.texture_kinds[i] = k;
    }
    for (const UnorderedAccess& u : sh_->uavs)
      if (u.read && !IsBuffer(u.dim) && u.ret != ReturnType::Float && u.ret != ReturnType::Uint &&
          u.ret != ReturnType::Sint)
        return Fail(Fmt("a typed UAV read of return type %u", unsigned(u.ret)));
    return true;
  }

  // The gfx texture the code samples `res` with through `smp`; a load's
  // (smp null).
  std::string Tex(const Operand& res, const Operand* smp, bool cmp) {
    const uint32_t t = res.index[0].imm;
    if (smp) {
      auto it = pairs_.find({t, int(smp->index[0].imm), cmp});
      if (it != pairs_.end()) return "r_tex" + std::to_string(it->second);
    } else {
      for (const auto& [k, n] : pairs_)
        if (std::get<0>(k) == t && !std::get<2>(k)) return "r_tex" + std::to_string(n);
    }
    Fail("a texture binding missing");
    return "r_tex0";
  }

  // ---- operands

  // The components of `o` that feed destination components `comps`.
  static void Sel(const Operand& o, const int* comps, int n, int* sel) {
    for (int i = 0; i < n; i++)
      sel[i] = o.ncomp == 4 ? (o.mode == 0 ? comps[i] : o.swz[comps[i]]) : 0;
  }

  // An index as a uint expression.
  std::string Index(const Operand& o, int d) {
    const Operand::Index& ix = o.index[d];
    if (ix.rel < 0) return UintLit(ix.imm);
    const int c0 = 0;
    std::string r = Src(o.rel[size_t(ix.rel)], 'u', &c0, 1);
    return ix.imm ? "(" + r + " + " + UintLit(ix.imm) + ")" : r;
  }

  // A register's uvec4 expression; empty for operands that are not one.
  std::string Reg(const Operand& o) {
    switch (o.type) {
      case kTemp:
        if (o.index[0].rel >= 0) return Fail("a temp at a run-time index"), "r0";
        if (o.index[0].imm >= temps_) return Fail("a temp past dcl_temps"), "r0";
        return "r" + std::to_string(o.index[0].imm);
      case kInput: {
        if (o.ndim != 1) return Fail("an input of more than one index"), "r_v[0]";
        const std::string i = Index(o, 0);
        if (o.index[0].rel >= 0) return "r_v[min(" + i + ", " + UintLit(InputCount() - 1) + ")]";
        if (o.index[0].imm >= InputCount()) return Fail("an input not declared"), "r_v[0]";
        return "r_v[" + std::to_string(o.index[0].imm) + "]";
      }
      case kOutput: {
        const std::string i = Index(o, 0);
        if (o.index[0].rel >= 0) return "r_o[min(" + i + ", " + UintLit(OutputCount() - 1) + ")]";
        if (o.index[0].imm >= OutputCount()) return Fail("an output not declared"), "r_o[0]";
        return "r_o[" + std::to_string(o.index[0].imm) + "]";
      }
      case kIndexableTemp: {
        auto it = xtemps_.find(o.index[0].imm);
        if (it == xtemps_.end() || o.ndim != 2) return Fail("an indexable temp not declared"), "r0";
        const std::string i = Index(o, 1);
        const std::string x = "x" + std::to_string(it->first);
        if (o.index[1].rel >= 0) return x + "[min(" + i + ", " + UintLit(it->second - 1) + ")]";
        if (o.index[1].imm >= it->second) return Fail("an indexable temp past its size"), "r0";
        return x + "[" + std::to_string(o.index[1].imm) + "]";
      }
      case kImmConstantBuffer: {
        const uint32_t n = uint32_t(icb_.size() / 4);
        if (!n) return Fail("an immediate constant buffer not declared"), "r0";
        const std::string i = Index(o, 0);
        if (o.index[0].rel >= 0) return "r_icb[min(" + i + ", " + UintLit(n - 1) + ")]";
        return "r_icb[" + std::to_string(std::min(o.index[0].imm, n - 1)) + "]";
      }
      case kInputThreadId: return "uvec4(gl_GlobalInvocationID, 0u)";
      case kInputThreadGroupId: return "uvec4(gl_WorkGroupID, 0u)";
      case kInputThreadIdInGroup: return "uvec4(gl_LocalInvocationID, 0u)";
      case kInputThreadIdInGroupFlattened: return "uvec4(gl_LocalInvocationIndex)";
      case kInputPrimitiveId: return "uvec4(uint(gl_PrimitiveID))";
      case kInputCoverageMask: return "uvec4(r_cov)";
      case kInputControlPoint: {
        if (o.ndim != 2 || o.index[0].rel >= 0 || o.index[0].imm != 0 || o.index[1].rel >= 0)
          return Fail("a control point other than the patch's one"), "r_v[0]";
        if (o.index[1].imm >= InputCount()) return Fail("an input not declared"), "r_v[0]";
        return "r_v[" + std::to_string(o.index[1].imm) + "]";
      }
      case kInputDomainPoint: return "r_dom";
      default: return {};
    }
  }

  // A source operand's components for destination components `comps`
  // as type t (f, i, u), modifiers applied.
  std::string Src(const Operand& o, char t, const int* comps, int n) {
    int sel[4];
    Sel(o, comps, n, sel);
    std::string e;
    if (o.type == kImm32) {
      if (n == 1) {
        e = Lit(o.imm[sel[0]], t);
      } else {
        e = TypeN(t, n) + "(";
        for (int i = 0; i < n; i++) e += (i ? ", " : "") + Lit(o.imm[sel[i]], t);
        e += ")";
      }
    } else if (o.type == kConstantBuffer) {
      auto it = cb_.find(o.index[0].imm);
      if (it == cb_.end() || o.ndim != 2 || o.index[0].rel >= 0) {
        Fail("a constant buffer not declared");
        return Zero(t);
      }
      const std::string b = UintLit(uint32_t(it->second));
      std::string u = n == 1 ? "" : TypeN('u', n) + "(";
      for (int i = 0; i < n; i++) {
        if (i) u += ", ";
        if (o.index[1].rel < 0) {
          u += "r_ld(" + b + ", " + UintLit(o.index[1].imm * 4 + uint32_t(sel[i])) + ")";
        } else {
          Use("r_cbx");
          u += "r_cbx(" + b + ", " + Index(o, 1) + ", " + UintLit(uint32_t(sel[i])) + ")";
        }
      }
      if (n > 1) u += ")";
      e = Conv(u, 'u', t, n);
    } else {
      const std::string r = Reg(o);
      if (r.empty()) {
        Fail(Fmt("a source operand of type %u not translated yet", o.type));
        return Zero(t);
      }
      e = Conv(r + "." + Chars(sel, n), 'u', t, n);
    }
    if (o.mod & 2) e = "abs(" + e + ")";
    if (o.mod & 1) e = "(-" + e + ")";
    return e;
  }
  std::string Src4(const Operand& o, char t) { return Src(o, t, kXyzw, 4); }
  std::string Scalar(const Operand& o, char t) {
    const int c0 = 0;
    return Src(o, t, &c0, 1);
  }

  struct Dst {
    std::string lv;  // empty: null
    int comps[4] = {};
    int n = 0;
  };
  Dst DstOf(const Operand& o) {
    Dst d;
    if (o.type == kNull) return d;
    const uint8_t mask = o.ncomp == 4 ? (o.mode == 0 ? o.mask : 0xF) : 1;
    for (int i = 0; i < 4; i++)
      if (mask & (1 << i)) d.comps[d.n++] = i;
    switch (o.type) {
      case kOutputDepth: case kOutputDepthGe: case kOutputDepthLe:
        d.lv = "r_odepth";
        d.n = 1;
        return d;
      case kOutputCoverageMask:
        d.lv = "r_omask";
        d.n = 1;
        return d;
      case kTemp: case kOutput: case kIndexableTemp: {
        if (o.mode != 0 && o.ncomp == 4) Fail("a destination with a swizzle");
        d.lv = Reg(o) + "." + Chars(d.comps, d.n);
        return d;
      }
      default:
        Fail(Fmt("a destination operand of type %u not translated yet", o.type));
        d.n = 0;
        return d;
    }
  }
  // dst = v (type t, dst.n components), saturated if sat.
  void Assign(const Dst& d, std::string v, char t, bool sat) {
    if (d.lv.empty()) return;
    if (sat) {
      if (t != 'f') {
        Fail("_sat on an integer result");
        return;
      }
      v = "clamp(" + v + ", 0.0, 1.0)";
    }
    Line(d.lv + " = " + Conv(v, t, 'u', d.n) + ";");
  }
  // A scalar result for every destination component.
  static std::string Splat(const std::string& v, char t, int n) {
    return n == 1 ? v : TypeN(t, n) + "(" + v + ")";
  }

  // Component-wise: dst = f(sources), sources typed by `st` (one letter
  // per source, the last repeated), the result typed rt.
  template <typename F>
  void Cw(const Instr& in, const char* st, char rt, F f) {
    const Dst d = DstOf(in.ops[0]);
    if (!d.n || !error_.empty()) return;
    std::vector<std::string> s;
    const size_t nst = std::strlen(st);
    for (size_t i = 1; i < in.ops.size(); i++) s.push_back(Src(in.ops[i], st[std::min(i - 1, nst - 1)], d.comps, d.n));
    if (!error_.empty()) return;
    Assign(d, f(s, d.n), rt, in.sat);
  }
  // A binary operator, or a comparison (operator for scalars, function
  // for vectors).
  void Bin(const Instr& in, const char* st, char rt, const char* op) {
    Cw(in, st, rt, [&](const std::vector<std::string>& s, int) { return "(" + s[0] + " " + op + " " + s[1] + ")"; });
  }
  void Cmp(const Instr& in, char st, const char* op, const char* fn) {
    const char sts[2] = {st, 0};
    Cw(in, sts, 'b', [&](const std::vector<std::string>& s, int n) {
      return n == 1 ? "(" + s[0] + " " + op + " " + s[1] + ")" : std::string(fn) + "(" + s[0] + ", " + s[1] + ")";
    });
  }
  void Fn(const Instr& in, const char* st, char rt, const char* fn) {
    Cw(in, st, rt, [&](const std::vector<std::string>& s, int) {
      std::string r = std::string(fn) + "(";
      for (size_t i = 0; i < s.size(); i++) r += (i ? ", " : "") + s[i];
      return r + ")";
    });
  }
  // A helper on four components (kHelpers), its arguments widened.
  void Helper4(const Instr& in, const char* st, char rt, const char* fn) {
    Use(fn);
    const size_t nst = std::strlen(st);
    Cw(in, st, rt, [&](const std::vector<std::string>& s, int n) {
      std::string r = std::string(fn) + "(";
      for (size_t i = 0; i < s.size(); i++) r += (i ? ", " : "") + Widen(s[i], st[std::min(i, nst - 1)], n);
      return Narrow(r + ")", n);
    });
  }

  std::string Test(const Instr& in) { return "(" + Scalar(in.ops[0], 'u') + (in.nz ? " != 0u)" : " == 0u)"); }

  // ---- instructions

  void Emit(const Instr& in) {
    switch (in.op) {
      // Floating point.
      case kAdd: return Bin(in, "f", 'f', "+");
      case kMul: return Bin(in, "f", 'f', "*");
      case kDiv: return Bin(in, "f", 'f', "/");
      case kMad:
        return Cw(in, "f", 'f', [](const std::vector<std::string>& s, int) { return "(" + s[0] + " * " + s[1] + " + " + s[2] + ")"; });
      case kMin: return Fn(in, "f", 'f', "min");
      case kMax: return Fn(in, "f", 'f', "max");
      case kFrc: return Fn(in, "f", 'f', "fract");
      case kExp: return Fn(in, "f", 'f', "exp2");
      case kLog: return Fn(in, "f", 'f', "log2");
      case kRsq: return Fn(in, "f", 'f', "inversesqrt");
      case kSqrt: return Fn(in, "f", 'f', "sqrt");
      case kRcp:
        return Cw(in, "f", 'f', [](const std::vector<std::string>& s, int) { return "(1.0 / " + s[0] + ")"; });
      case kRoundNe: return Fn(in, "f", 'f', "roundEven");
      case kRoundNi: return Fn(in, "f", 'f', "floor");
      case kRoundPi: return Fn(in, "f", 'f', "ceil");
      case kRoundZ: return Fn(in, "f", 'f', "trunc");
      case kDerivRtx: case kDerivRty: case kDerivRtxCoarse: case kDerivRtxFine: case kDerivRtyCoarse:
      case kDerivRtyFine: {
        if (stage_ != ShaderType::Pixel) return void(Fail("derivatives outside a pixel shader"));
        static const std::map<uint32_t, const char*> kFns = {
            {kDerivRtx, "dFdx"}, {kDerivRty, "dFdy"}, {kDerivRtxCoarse, "dFdxCoarse"},
            {kDerivRtxFine, "dFdxFine"}, {kDerivRtyCoarse, "dFdyCoarse"}, {kDerivRtyFine, "dFdyFine"}};
        return Fn(in, "f", 'f', kFns.at(in.op));
      }
      case kDp2: case kDp3: case kDp4: {
        const int k = in.op == kDp2 ? 2 : in.op == kDp3 ? 3 : 4;
        const Dst d = DstOf(in.ops[0]);
        if (!d.n) return;
        const std::string v = "dot(" + Src(in.ops[1], 'f', kXyzw, k) + ", " + Src(in.ops[2], 'f', kXyzw, k) + ")";
        return Assign(d, Splat(v, 'f', d.n), 'f', in.sat);
      }
      case kSincos: {
        const Dst s = DstOf(in.ops[0]), c = DstOf(in.ops[1]);
        Line("{");
        indent_++;
        Line("vec4 r_a = " + Src4(in.ops[2], 'f') + ";");
        if (s.n) Assign(s, "sin(r_a." + Chars(s.comps, s.n) + ")", 'f', in.sat);
        if (c.n) Assign(c, "cos(r_a." + Chars(c.comps, c.n) + ")", 'f', in.sat);
        indent_--;
        return Line("}");
      }
      case kEq: return Cmp(in, 'f', "==", "equal");
      case kNe: return Cmp(in, 'f', "!=", "notEqual");
      case kLt: return Cmp(in, 'f', "<", "lessThan");
      case kGe: return Cmp(in, 'f', ">=", "greaterThanEqual");
      case kMov: {
        const bool f = in.sat || in.ops[1].mod;
        return Cw(in, f ? "f" : "u", f ? 'f' : 'u', [](const std::vector<std::string>& s, int) { return s[0]; });
      }
      case kMovc: {
        const bool f = in.sat || in.ops[2].mod || in.ops[3].mod;
        return Cw(in, f ? "uf" : "u", f ? 'f' : 'u', [](const std::vector<std::string>& s, int n) {
          return n == 1 ? "(" + s[0] + " != 0u ? " + s[1] + " : " + s[2] + ")"
                        : "mix(" + s[2] + ", " + s[1] + ", notEqual(" + s[0] + ", " + TypeN('u', n) + "(0u)))";
        });
      }
      case kSwapc: {
        const Dst a = DstOf(in.ops[0]), b = DstOf(in.ops[1]);
        Line("{");
        indent_++;
        Line("uvec4 r_c = " + Src4(in.ops[2], 'u') + ", r_x = " + Src4(in.ops[3], 'u') + ", r_y = " +
             Src4(in.ops[4], 'u') + ";");
        auto sel = [&](const Dst& d, const char* t, const char* f) {
          if (!d.n) return;
          const std::string m = Chars(d.comps, d.n);
          Assign(d, d.n == 1 ? "(r_c." + m + " != 0u ? r_" + t + "." + m + " : r_" + f + "." + m + ")"
                             : "mix(r_" + std::string(f) + "." + m + ", r_" + t + "." + m + ", notEqual(r_c." + m +
                                   ", " + TypeN('u', d.n) + "(0u)))",
                 'u', false);
        };
        sel(a, "y", "x");  // dst0: cond ? src2 : src1
        sel(b, "x", "y");
        indent_--;
        return Line("}");
      }

      // Integers.
      case kIadd: return Bin(in, "i", 'i', "+");
      case kImad:
        return Cw(in, "i", 'i', [](const std::vector<std::string>& s, int) { return "(" + s[0] + " * " + s[1] + " + " + s[2] + ")"; });
      case kUmad:
        return Cw(in, "u", 'u', [](const std::vector<std::string>& s, int) { return "(" + s[0] + " * " + s[1] + " + " + s[2] + ")"; });
      case kImax: return Fn(in, "i", 'i', "max");
      case kImin: return Fn(in, "i", 'i', "min");
      case kUmax: return Fn(in, "u", 'u', "max");
      case kUmin: return Fn(in, "u", 'u', "min");
      case kIneg:
        return Cw(in, "i", 'i', [](const std::vector<std::string>& s, int) { return "(-" + s[0] + ")"; });
      case kIeq: return Cmp(in, 'i', "==", "equal");
      case kIne: return Cmp(in, 'i', "!=", "notEqual");
      case kIlt: return Cmp(in, 'i', "<", "lessThan");
      case kIge: return Cmp(in, 'i', ">=", "greaterThanEqual");
      case kUlt: return Cmp(in, 'u', "<", "lessThan");
      case kUge: return Cmp(in, 'u', ">=", "greaterThanEqual");
      case kAnd: return Bin(in, "u", 'u', "&");
      case kOr: return Bin(in, "u", 'u', "|");
      case kXor: return Bin(in, "u", 'u', "^");
      case kNot:
        return Cw(in, "u", 'u', [](const std::vector<std::string>& s, int) { return "(~" + s[0] + ")"; });
      case kIshl:
        return Cw(in, "u", 'u', [](const std::vector<std::string>& s, int) { return "(" + s[0] + " << (" + s[1] + " & 31u))"; });
      case kUshr:
        return Cw(in, "u", 'u', [](const std::vector<std::string>& s, int) { return "(" + s[0] + " >> (" + s[1] + " & 31u))"; });
      case kIshr:
        return Cw(in, "iu", 'i', [](const std::vector<std::string>& s, int) { return "(" + s[0] + " >> (" + s[1] + " & 31u))"; });
      case kImul: case kUmul: return MulWide(in);
      case kUdiv: {
        const Dst q = DstOf(in.ops[0]), r = DstOf(in.ops[1]);
        Line("{");
        indent_++;
        Line("uvec4 r_x = " + Src4(in.ops[2], 'u') + ", r_y = " + Src4(in.ops[3], 'u') + ";");
        auto div = [&](const Dst& d, const char* op) {
          if (!d.n) return;
          const std::string m = Chars(d.comps, d.n), u = TypeN('u', d.n);
          Assign(d, d.n == 1 ? "(r_y." + m + " == 0u ? 0xFFFFFFFFu : r_x." + m + " " + op + " r_y." + m + ")"
                             : "mix(r_x." + m + " " + op + " max(r_y." + m + ", " + u + "(1u)), " + u +
                                   "(0xFFFFFFFFu), equal(r_y." + m + ", " + u + "(0u)))",
                 'u', false);
        };
        div(q, "/");
        div(r, "%");
        indent_--;
        return Line("}");
      }
      case kUaddc: case kUsubb: {
        const Dst s = DstOf(in.ops[0]), c = DstOf(in.ops[1]);
        Line("{");
        indent_++;
        Line("uvec4 r_c, r_s = " + std::string(in.op == kUaddc ? "uaddCarry(" : "usubBorrow(") + Src4(in.ops[2], 'u') +
             ", " + Src4(in.ops[3], 'u') + ", r_c);");
        if (s.n) Assign(s, "r_s." + Chars(s.comps, s.n), 'u', false);
        if (c.n) Assign(c, "r_c." + Chars(c.comps, c.n), 'u', false);
        indent_--;
        return Line("}");
      }
      case kItof: case kUtof:
        return Cw(in, in.op == kItof ? "i" : "u", 'f',
                  [](const std::vector<std::string>& s, int n) { return TypeN('f', n) + "(" + s[0] + ")"; });
      case kFtoi: return Helper4(in, "f", 'i', "r_f2i");
      case kFtou: return Helper4(in, "f", 'u', "r_f2u");
      case kF32ToF16: return Helper4(in, "f", 'u', "r_f2h");
      case kF16ToF32: return Helper4(in, "u", 'f', "r_h2f");
      case kCountbits:
        return Cw(in, "u", 'u', [](const std::vector<std::string>& s, int n) { return TypeN('u', n) + "(bitCount(" + s[0] + "))"; });
      case kFirstbitLo:
        return Cw(in, "u", 'u', [](const std::vector<std::string>& s, int n) { return TypeN('u', n) + "(findLSB(" + s[0] + "))"; });
      case kFirstbitHi: return Helper4(in, "u", 'u', "r_fbh");
      case kFirstbitShi: return Helper4(in, "u", 'u', "r_fbsh");
      case kUbfe: return Helper4(in, "u", 'u', "r_ubfe");
      case kIbfe: return Helper4(in, "u", 'u', "r_ibfe");
      case kBfi: return Helper4(in, "u", 'u', "r_bfi");
      case kBfrev: return Fn(in, "u", 'u', "bitfieldReverse");

      // Control flow, its nesting checked: blocks (i: if, e: its else, l:
      // loop, s: switch) on flow_.
      case kIf:
        Line("if " + Test(in) + " {");
        return Open('i');
      case kElse:
        if (flow_.empty() || flow_.back() != 'i') return void(Fail("else outside an if"));
        flow_.back() = 'e';
        indent_--;
        Line("} else {");
        indent_++;
        return;
      case kEndif: case kEndloop:
        if (flow_.empty() || (in.op == kEndloop ? flow_.back() != 'l' : flow_.back() == 'l' || flow_.back() == 's'))
          return void(Fail("a block's end without its start"));
        flow_.pop_back();
        indent_--;
        return Line("}");
      case kLoop:
        Line("for (;;) {");
        return Open('l');
      case kBreak: case kBreakc:
        if (std::find_if(flow_.begin(), flow_.end(), [](char c) { return c == 'l' || c == 's'; }) == flow_.end())
          return void(Fail("break outside a loop or switch"));
        return Line(in.op == kBreak ? "break;" : "if " + Test(in) + " break;");
      case kContinue: case kContinuec:
        if (std::find(flow_.begin(), flow_.end(), 'l') == flow_.end()) return void(Fail("continue outside a loop"));
        return Line(in.op == kContinue ? "continue;" : "if " + Test(in) + " continue;");
      case kSwitch:
        Line("switch (" + Scalar(in.ops[0], 'u') + ") {");
        indent_++;
        return Open('s');
      case kCase: case kDefault:
        if (flow_.empty() || flow_.back() != 's') return void(Fail("a case outside a switch"));
        indent_--;
        Line(in.op == kCase ? "case " + UintLit(in.ops[0].imm[0]) + ":" : std::string("default:"));
        indent_++;
        last_label_ = true;
        return;
      case kEndswitch:
        if (flow_.empty() || flow_.back() != 's') return void(Fail("endswitch without a switch"));
        flow_.pop_back();
        if (last_label_) Line("break;");
        indent_ -= 2;
        return Line("}");
      case kRet: return Line("return;");
      case kRetc: return Line("if " + Test(in) + " return;");
      case kDiscard:
        if (stage_ != ShaderType::Pixel) return void(Fail("discard outside a pixel shader"));
        sh_->discards = true;
        return Line("if " + Test(in) + " discard;");
      case kNop: return;
      case kSync: return Sync(in);

      // Resources.
      case kSample: case kSampleB: case kSampleL: case kSampleD: case kSampleC: case kSampleCLz:
      case kGather4: case kGather4C: case kGather4Po: case kGather4PoC: case kLod:
        return SampleOp(in);
      case kLd: case kLdMs: return Load(in);
      case kResinfo: return Resinfo(in);
      case kSampleInfo: return SampleInfo(in);
      case kBufinfo: return Bufinfo(in);
      case kLdRaw: case kLdStructured: return LoadWords(in);
      case kStoreRaw: case kStoreStructured: return StoreWords(in);
      case kLdUavTyped: return LoadTyped(in);
      case kStoreUavTyped: return StoreTyped(in);
      case kAtomicAnd: case kAtomicOr: case kAtomicXor: case kAtomicCmpStore: case kAtomicIadd:
      case kAtomicImax: case kAtomicImin: case kAtomicUmax: case kAtomicUmin: case kImmAtomicIadd:
      case kImmAtomicAnd: case kImmAtomicOr: case kImmAtomicXor: case kImmAtomicExch:
      case kImmAtomicCmpExch: case kImmAtomicImax: case kImmAtomicImin: case kImmAtomicUmax:
      case kImmAtomicUmin:
        return Atomic(in);
      case kImmAtomicAlloc: case kImmAtomicConsume: return Counter(in);
      default:
        Fail("an instruction not translated yet");
    }
  }

  void MulWide(const Instr& in) {
    const bool s = in.op == kImul;
    const Dst hi = DstOf(in.ops[0]), lo = DstOf(in.ops[1]);
    if (!hi.n) {
      if (!lo.n) return;
      const std::string a = Src(in.ops[2], s ? 'i' : 'u', lo.comps, lo.n), b = Src(in.ops[3], s ? 'i' : 'u', lo.comps, lo.n);
      return Assign(lo, "(" + a + " * " + b + ")", s ? 'i' : 'u', false);
    }
    const char t = s ? 'i' : 'u';
    Line("{");
    indent_++;
    Line(TypeN(t, 4) + " r_h, r_l;");
    Line(std::string(s ? "imulExtended(" : "umulExtended(") + Src4(in.ops[2], t) + ", " + Src4(in.ops[3], t) +
         ", r_h, r_l);");
    Assign(hi, "r_h." + Chars(hi.comps, hi.n), t, false);
    if (lo.n) Assign(lo, "r_l." + Chars(lo.comps, lo.n), t, false);
    indent_--;
    Line("}");
  }

  void Sync(const Instr& in) {
    const uint32_t ctl = (in.token >> 11) & 0xF;
    if (stage_ != ShaderType::Compute) return void(Fail("sync outside a compute shader"));
    if (ctl & 2) Line("memoryBarrierShared();");
    if (ctl & 0xC) {
      Line("memoryBarrierBuffer();");
      Line("memoryBarrierImage();");
    }
    if (ctl & 1) Line("barrier();");
  }

  // ---- textures

  static int CoordCount(ResourceDim d) {
    switch (d) {
      case ResourceDim::Buffer: case ResourceDim::Texture1D: return 1;
      case ResourceDim::Texture2D: case ResourceDim::Texture2DMS: case ResourceDim::Texture1DArray: return 2;
      case ResourceDim::TextureCubeArray: return 4;
      default: return 3;
    }
  }
  static char RetType(ReturnType r) { return r == ReturnType::Uint ? 'u' : r == ReturnType::Sint ? 'i' : 'f'; }

  // The sample-family instructions.
  void SampleOp(const Instr& in) {
    const bool po = in.op == kGather4Po || in.op == kGather4PoC;
    const size_t ires = po ? 3 : 2, ismp = ires + 1, iref = ismp + 1;
    const bool cmp = in.op == kSampleC || in.op == kSampleCLz || in.op == kGather4C || in.op == kGather4PoC;
    if (in.ops.size() < ismp + 1) return void(Fail("an instruction with too few operands"));
    const Operand& addr = in.ops[1];
    const Operand& res = in.ops[ires];
    const Operand& smp = in.ops[ismp];
    const Dst d = DstOf(in.ops[0]);
    if (!d.n || res.type != kResource) return void(Fail("a sample of something not a texture"));
    const ShaderResource* r = Srv(res.index[0].imm);
    if (!r || IsBuffer(r->dim) || r->dim == ResourceDim::Texture2DMS || r->dim == ResourceDim::Texture2DMSArray)
      return void(Fail("a sample of a resource that cannot be sampled"));
    const std::string s = Tex(res, &smp, cmp);
    const ResourceDim dim = r->dim;
    const bool cube_array = dim == ResourceDim::TextureCubeArray;
    const bool gather = in.op == kGather4 || in.op == kGather4C || in.op == kGather4Po || in.op == kGather4PoC;
    if (cube_array && (gather || in.op == kLod)) return void(Fail("a gather or lod of a cube array not translated yet"));

    // The coordinate (with the reference value for comparisons).
    const int c0 = 0, c1 = 1;
    std::string p;
    switch (dim) {
      case ResourceDim::Texture1D: p = "vec2(" + Src(addr, 'f', &c0, 1) + ", 0.5)"; break;
      case ResourceDim::Texture1DArray:
        p = "vec3(" + Src(addr, 'f', &c0, 1) + ", 0.5, " + Src(addr, 'f', &c1, 1) + ")";
        break;
      case ResourceDim::TextureCubeArray:
        Use("r_cubearr");
        p = "r_cubearr(" + Src(addr, 'f', kXyzw, 4) + ")";
        break;
      default: p = Src(addr, 'f', kXyzw, CoordCount(dim)); break;
    }
    if (cmp && !gather) {
      const std::string ref = Scalar(in.ops[iref], 'f');
      const int np = dim == ResourceDim::Texture1D || dim == ResourceDim::Texture2D ? 3 : 4;
      p = (np == 3 ? "vec3(" : "vec4(") + p + ", " + ref + ")";
    }
    // Immediate offsets (cubes have none).
    std::string off;
    const bool cube = dim == ResourceDim::TextureCube || cube_array;
    if (in.has_off && !cube) {
      if (dim == ResourceDim::Texture3D) off = Fmt("ivec3(%d, %d, %d)", in.off[0], in.off[1], in.off[2]);
      else if (dim == ResourceDim::Texture1D || dim == ResourceDim::Texture1DArray) off = Fmt("ivec2(%d, 0)", in.off[0]);
      else off = Fmt("ivec2(%d, %d)", in.off[0], in.off[1]);
    }
    // Gradients: the coordinate's components without the array index.
    auto grad = [&](const Operand& g) {
      if (dim == ResourceDim::Texture1D || dim == ResourceDim::Texture1DArray) return "vec2(" + Src(g, 'f', &c0, 1) + ", 0.0)";
      const int k = dim == ResourceDim::Texture2D || dim == ResourceDim::Texture2DArray ? 2 : 3;
      return Src(g, 'f', kXyzw, k);
    };
    std::string call;
    switch (in.op) {
      case kSample: case kSampleC:
        call = off.empty() ? "texture(" + s + ", " + p + ")" : "textureOffset(" + s + ", " + p + ", " + off + ")";
        break;
      case kSampleB: {
        const std::string b = Scalar(in.ops[4], 'f');
        call = off.empty() ? "texture(" + s + ", " + p + ", " + b + ")"
                           : "textureOffset(" + s + ", " + p + ", " + off + ", " + b + ")";
        break;
      }
      case kSampleL: {
        const std::string l = Scalar(in.ops[4], 'f');
        call = off.empty() ? "textureLod(" + s + ", " + p + ", " + l + ")"
                           : "textureLodOffset(" + s + ", " + p + ", " + l + ", " + off + ")";
        break;
      }
      case kSampleD: {
        if (in.ops.size() < 6) return void(Fail("an instruction with too few operands"));
        const std::string gx = grad(in.ops[4]), gy = grad(in.ops[5]);
        if (cube_array) {
          // The face's own gradients are not derived: an implicit one.
          call = "texture(" + s + ", " + p + ")";
          break;
        }
        call = off.empty() ? "textureGrad(" + s + ", " + p + ", " + gx + ", " + gy + ")"
                           : "textureGradOffset(" + s + ", " + p + ", " + gx + ", " + gy + ", " + off + ")";
        break;
      }
      case kSampleCLz:
        // Level 0: textureLod where GLSL has it for the shadow sampler,
        // zero gradients elsewhere.
        if (dim == ResourceDim::Texture1D || dim == ResourceDim::Texture2D) {
          call = off.empty() ? "textureLod(" + s + ", " + p + ", 0.0)"
                             : "textureLodOffset(" + s + ", " + p + ", 0.0, " + off + ")";
        } else {
          const std::string z = dim == ResourceDim::TextureCube ? "vec3(0.0)" : "vec2(0.0)";
          call = off.empty() ? "textureGrad(" + s + ", " + p + ", " + z + ", " + z + ")"
                             : "textureGradOffset(" + s + ", " + p + ", " + z + ", " + z + ", " + off + ")";
        }
        break;
      case kGather4: case kGather4Po: {
        const std::string comp = std::to_string(smp.ncomp == 4 ? smp.swz[0] : 0);
        std::string o = off;
        if (po) o = Src(in.ops[2], 'i', kXyzw, 2);
        call = o.empty() ? "textureGather(" + s + ", " + p + ", " + comp + ")"
                         : "textureGatherOffset(" + s + ", " + p + ", " + o + ", " + comp + ")";
        break;
      }
      case kGather4C: case kGather4PoC: {
        const std::string ref = Scalar(in.ops[iref], 'f');
        std::string o = off;
        if (po) o = Src(in.ops[2], 'i', kXyzw, 2);
        call = o.empty() ? "textureGather(" + s + ", " + p + ", " + ref + ")"
                         : "textureGatherOffset(" + s + ", " + p + ", " + ref + ", " + o + ")";
        break;
      }
      case kLod: {
        // From the coordinate's derivatives in texels (textureQueryLod is
        // not in MSL 2.1): the larger of the two axes' lengths, clamped to
        // the texture's levels (not to the sampler's LOD range).
        std::string pt, sz;
        switch (dim) {
          case ResourceDim::Texture1D: case ResourceDim::Texture1DArray:
            pt = Src(addr, 'f', &c0, 1), sz = "float(textureSize(" + s + ", 0).x)";
            break;
          case ResourceDim::Texture2D: case ResourceDim::Texture2DArray:
            pt = Src(addr, 'f', kXyzw, 2), sz = "vec2(textureSize(" + s + ", 0).xy)";
            break;
          case ResourceDim::Texture3D:
            pt = Src(addr, 'f', kXyzw, 3), sz = "vec3(textureSize(" + s + ", 0))";
            break;
          default:  // a cube: the direction on its face
            pt = Src(addr, 'f', kXyzw, 3);
            pt = "(" + pt + " / max(abs(" + pt + ").x, max(abs(" + pt + ").y, abs(" + pt + ").z)))";
            sz = "vec3(0.5 * float(textureSize(" + s + ", 0).x))";
            break;
        }
        Line("{");
        indent_++;
        Line("float r_u = 0.5 * log2(max(dot(dFdx(" + pt + ") * " + sz + ", dFdx(" + pt + ") * " + sz +
             "), dot(dFdy(" + pt + ") * " + sz + ", dFdy(" + pt + ") * " + sz + ")));");
        call = "vec4(clamp(r_u, 0.0, float(textureQueryLevels(" + s + ") - 1)), r_u, 0.0, 0.0)";
        int sel[4];
        Sel(res, d.comps, d.n, sel);
        Assign(d, call + "." + Chars(sel, d.n), 'f', in.sat);
        indent_--;
        return Line("}");
      }
    }
    if (cmp && !gather) return Assign(d, Splat(call, 'f', d.n), 'f', in.sat);
    int sel[4];
    Sel(res, d.comps, d.n, sel);
    Assign(d, call + "." + Chars(sel, d.n), in.op == kLod ? 'f' : RetType(r->ret), in.sat);
  }

  // ld, ld_ms: texel loads (a typed buffer's element).
  void Load(const Instr& in) {
    const Operand& addr = in.ops[1];
    const Operand& res = in.ops[2];
    const Dst d = DstOf(in.ops[0]);
    if (!d.n) return;
    if (res.type != kResource) return void(Fail("a load from something not an SRV"));
    const ShaderResource* r = Srv(res.index[0].imm);
    if (!r) return void(Fail("a resource not declared"));
    int sel[4];
    Sel(res, d.comps, d.n, sel);
    const int c0 = 0, c1 = 1, c3 = 3;
    if (r->dim == ResourceDim::Buffer) {
      Use("r_fmt"), Use("r_tld");
      const std::string v = "r_tld(" + UintLit(uint32_t(r->buffer)) + ", R_FMT_T" + std::to_string(r->slot) + ", " +
                            Src(addr, 'u', &c0, 1) + ")";
      return Assign(d, v + "." + Chars(sel, d.n), 'u', false);
    }
    if (IsBuffer(r->dim) || r->dim == ResourceDim::TextureCube || r->dim == ResourceDim::TextureCubeArray)
      return void(Fail("a texel load from a resource that has none"));
    const std::string s = Tex(res, nullptr, false);
    std::string p;
    switch (r->dim) {
      case ResourceDim::Texture1D: p = "ivec2(" + Src(addr, 'i', &c0, 1) + ", 0)"; break;
      case ResourceDim::Texture1DArray:
        p = "ivec3(" + Src(addr, 'i', &c0, 1) + ", 0, " + Src(addr, 'i', &c1, 1) + ")";
        break;
      default: p = Src(addr, 'i', kXyzw, CoordCount(r->dim)); break;
    }
    if (in.has_off) {
      if (r->dim == ResourceDim::Texture3D) p += Fmt(" + ivec3(%d, %d, %d)", in.off[0], in.off[1], in.off[2]);
      else if (r->dim == ResourceDim::Texture1D) p += Fmt(" + ivec2(%d, 0)", in.off[0]);
      else if (r->dim == ResourceDim::Texture1DArray) p += Fmt(" + ivec3(%d, 0, 0)", in.off[0]);
      else if (r->dim == ResourceDim::Texture2DArray || r->dim == ResourceDim::Texture2DMSArray)
        p += Fmt(" + ivec3(%d, %d, 0)", in.off[0], in.off[1]);
      else p += Fmt(" + ivec2(%d, %d)", in.off[0], in.off[1]);
    }
    // Multisampled textures are single-sampled ones here (the device has
    // no multisampled images): every sample is level 0's texel.
    const bool ms = r->dim == ResourceDim::Texture2DMS || r->dim == ResourceDim::Texture2DMSArray;
    const std::string lod = ms ? "0" : Src(addr, 'i', &c3, 1);
    Assign(d, "texelFetch(" + s + ", " + p + ", " + lod + ")." + Chars(sel, d.n), RetType(r->ret), false);
  }

  void Resinfo(const Instr& in) {
    const Dst d = DstOf(in.ops[0]);
    if (!d.n) return;
    const Operand& res = in.ops[2];
    const uint32_t rt = (in.token >> 11) & 3;
    int sel[4];
    Sel(res, d.comps, d.n, sel);
    Line("{");
    indent_++;
    if (res.type == kUav) {
      const UnorderedAccess* u = Uav(res.index[0].imm);
      if (!u || u->image < 0) return void(Fail("resinfo of a UAV that is not a texture"));
      const std::string img = "r_img" + std::to_string(u->image);
      std::string q;
      switch (u->dim) {
        case ResourceDim::Texture1D: q = "uvec4(uint(imageSize(" + img + ").x), 0u, 0u, 1u)"; break;
        case ResourceDim::Texture1DArray: q = "uvec4(uvec3(imageSize(" + img + ")).xz, 0u, 1u)"; break;
        case ResourceDim::Texture2D: q = "uvec4(uvec2(imageSize(" + img + ")), 0u, 1u)"; break;
        default: q = "uvec4(uvec3(imageSize(" + img + ")), 1u)"; break;
      }
      Line("uvec4 r_q = " + q + ";");
    } else if (res.type == kResource) {
      const ShaderResource* r = Srv(res.index[0].imm);
      if (!r || IsBuffer(r->dim)) return void(Fail("resinfo of a resource that is not a texture"));
      const std::string s = Tex(res, nullptr, false);
      Line("uint r_l = " + Scalar(in.ops[1], 'u') + ", r_n = uint(textureQueryLevels(" + s + "));");
      Line("uvec4 r_q = uvec4(0u, 0u, 0u, r_n);");
      const std::string sz = "textureSize(" + s + ", int(r_l))";
      std::string set;
      switch (r->dim) {
        case ResourceDim::Texture1D: set = "r_q.x = uint(" + sz + ".x);"; break;
        case ResourceDim::Texture1DArray: set = "r_q.xy = uvec2(" + sz + ".xz);"; break;
        case ResourceDim::Texture2D: case ResourceDim::Texture2DMS: case ResourceDim::TextureCube:
          set = "r_q.xy = uvec2(" + sz + ");";
          break;
        case ResourceDim::TextureCubeArray: set = "r_q.xyz = uvec3(" + sz + ") / uvec3(1u, 1u, 6u);"; break;
        default: set = "r_q.xyz = uvec3(" + sz + ");"; break;
      }
      Line("if (r_l < r_n) " + set);
    } else {
      return void(Fail("resinfo of an operand that is not a resource"));
    }
    const std::string m = Chars(sel, d.n);
    if (rt == 2) Assign(d, "r_q." + m, 'u', false);
    else if (rt == 1) Assign(d, "vec4(1.0 / vec3(r_q.xyz), float(r_q.w))." + m, 'f', false);
    else Assign(d, "vec4(r_q)." + m, 'f', in.sat);
    indent_--;
    Line("}");
  }

  // Sample counts: one (multisampled resources are single-sampled here).
  void SampleInfo(const Instr& in) {
    const Dst d = DstOf(in.ops[0]);
    if (!d.n) return;
    int sel[4];
    Sel(in.ops[1], d.comps, d.n, sel);
    const bool u = (in.token >> 11) & 1;
    const std::string v = u ? "uvec4(1u, 0u, 0u, 0u)" : "vec4(1.0, 0.0, 0.0, 0.0)";
    Assign(d, v + "." + Chars(sel, d.n), u ? 'u' : 'f', false);
  }

  void Bufinfo(const Instr& in) {
    const Dst d = DstOf(in.ops[0]);
    if (!d.n) return;
    const Operand& res = in.ops[1];
    std::string bytes, fmt;
    ResourceDim dim;
    uint32_t stride;
    if (res.type == kResource) {
      const ShaderResource* r = Srv(res.index[0].imm);
      if (!r || r->buffer < 0) return void(Fail("bufinfo of a resource that is not a buffer"));
      bytes = "r_buf[" + std::to_string(r->buffer) + "].y";
      fmt = "R_FMT_T" + std::to_string(r->slot);
      dim = r->dim, stride = r->stride;
    } else if (res.type == kUav) {
      const UnorderedAccess* u = Uav(res.index[0].imm);
      if (!u || u->buffer < 0) return void(Fail("bufinfo of a UAV that is not a buffer"));
      bytes = "r_rwbuf[" + std::to_string(u->buffer) + "].y";
      fmt = "R_FMT_U" + std::to_string(u->slot);
      dim = u->dim, stride = u->stride;
    } else {
      return void(Fail("bufinfo of an operand that is not a resource"));
    }
    std::string v = bytes;
    if (dim == ResourceDim::StructuredBuffer) v = "(" + bytes + " / " + UintLit(std::max(stride, 1u)) + ")";
    if (dim == ResourceDim::Buffer) {
      Use("r_fmt");
      v = "(" + bytes + " / max(r_fmtsize(" + fmt + "), 1u))";
    }
    Assign(d, Splat(v, 'u', d.n), 'u', false);
  }

  // ---- buffers and group-shared memory

  // A word reader or writer for raw or structured memory `o` (an SRV, a
  // UAV or g#), by byte address; its stride (0 for raw).
  struct Mem {
    std::string ld, st;  // "r_sld(2u, " ... + address + ")"
    uint32_t stride = 0;
    bool ok = false;
  };
  Mem MemOf(const Operand& o) {
    Mem m;
    const uint32_t slot = o.index[0].imm;
    if (o.type == kResource) {
      const ShaderResource* r = Srv(slot);
      if (!r || (r->dim != ResourceDim::RawBuffer && r->dim != ResourceDim::StructuredBuffer))
        return Fail("a raw or structured access to another resource"), m;
      Use("r_sld");
      m.ld = "r_sld(" + UintLit(uint32_t(r->buffer)) + ", ";
      m.stride = r->stride;
    } else if (o.type == kUav) {
      const UnorderedAccess* u = Uav(slot);
      if (!u || (u->dim != ResourceDim::RawBuffer && u->dim != ResourceDim::StructuredBuffer))
        return Fail("a raw or structured access to another UAV"), m;
      Use("r_uld"), Use("r_ust");
      m.ld = "r_uld(" + UintLit(uint32_t(u->buffer)) + ", ";
      m.st = "r_ust(" + UintLit(uint32_t(u->buffer)) + ", ";
      m.stride = u->stride;
    } else if (o.type == kTgsm) {
      auto it = tgsm_.find(slot);
      if (it == tgsm_.end()) return Fail("group-shared memory not declared"), m;
      m.ld = "r_g" + std::to_string(slot) + "_ld(";
      m.st = "r_g" + std::to_string(slot) + "_st(";
      m.stride = it->second.stride;
    } else {
      return Fail("a raw or structured access to an operand of this type"), m;
    }
    m.ok = true;
    return m;
  }
  // The byte address of a structured (index, offset) or raw access.
  std::string Address(const Instr& in, size_t first, bool structured, uint32_t stride) {
    if (!structured) return Scalar(in.ops[first], 'u');
    return "(" + Scalar(in.ops[first], 'u') + " * " + UintLit(stride) + " + " + Scalar(in.ops[first + 1], 'u') + ")";
  }

  void LoadWords(const Instr& in) {
    const bool structured = in.op == kLdStructured;
    const Operand& res = in.ops.back();
    const Dst d = DstOf(in.ops[0]);
    if (!d.n) return;
    const Mem m = MemOf(res);
    if (!m.ok) return;
    if (structured && !m.stride) return void(Fail("a structured load from raw memory"));
    int sel[4];
    Sel(res, d.comps, d.n, sel);
    Line("{");
    indent_++;
    Line("uint r_a = " + Address(in, 1, structured, m.stride) + ";");
    std::string v = d.n == 1 ? "" : TypeN('u', d.n) + "(";
    for (int i = 0; i < d.n; i++) v += (i ? ", " : "") + m.ld + (sel[i] ? "r_a + " + UintLit(4u * sel[i]) : "r_a") + ")";
    if (d.n > 1) v += ")";
    Assign(d, v, 'u', false);
    indent_--;
    Line("}");
  }

  void StoreWords(const Instr& in) {
    const bool structured = in.op == kStoreStructured;
    const Operand& dst = in.ops[0];
    const Mem m = MemOf(dst);
    if (!m.ok) return;
    if (m.st.empty()) return void(Fail("a store to read-only memory"));
    if (structured && !m.stride) return void(Fail("a structured store to raw memory"));
    const uint8_t mask = dst.ncomp == 4 && dst.mode == 0 ? dst.mask : 1;
    Line("{");
    indent_++;
    Line("uint r_a = " + Address(in, 1, structured, m.stride) + ";");
    Line("uvec4 r_s = " + Src4(in.ops.back(), 'u') + ";");
    for (int c = 0; c < 4; c++)
      if (mask & (1 << c))
        Line(m.st + (c ? "r_a + " + UintLit(4u * c) : "r_a") + ", r_s." + kComp[c] + ");");
    indent_--;
    Line("}");
  }

  // A typed UAV's image coordinate (ivec2/ivec3).
  std::string ImageCoord(const UnorderedAccess& u, const Operand& addr) {
    const int c0 = 0, c1 = 1;
    switch (u.dim) {
      case ResourceDim::Texture1D: return "ivec2(" + Src(addr, 'i', &c0, 1) + ", 0)";
      case ResourceDim::Texture1DArray:
        return "ivec3(" + Src(addr, 'i', &c0, 1) + ", 0, " + Src(addr, 'i', &c1, 1) + ")";
      case ResourceDim::Texture2D: return Src(addr, 'i', kXyzw, 2);
      default: return Src(addr, 'i', kXyzw, 3);
    }
  }

  void LoadTyped(const Instr& in) {
    const Dst d = DstOf(in.ops[0]);
    if (!d.n) return;
    const Operand& res = in.ops[2];
    const UnorderedAccess* u = res.type == kUav ? Uav(res.index[0].imm) : nullptr;
    if (!u) return void(Fail("a typed load from something not a UAV"));
    int sel[4];
    Sel(res, d.comps, d.n, sel);
    const int c0 = 0;
    if (u->dim == ResourceDim::Buffer) {
      Use("r_fmt"), Use("r_utld");
      return Assign(d, "r_utld(" + UintLit(uint32_t(u->buffer)) + ", R_FMT_U" + std::to_string(u->slot) + ", " +
                           Src(in.ops[1], 'u', &c0, 1) + ")." + Chars(sel, d.n),
                    'u', false);
    }
    if (u->image < 0) return void(Fail("a typed load from a UAV that is not typed"));
    Assign(d, "imageLoad(r_img" + std::to_string(u->image) + ", " + ImageCoord(*u, in.ops[1]) + ")." + Chars(sel, d.n),
           RetType(u->ret), false);
  }

  void StoreTyped(const Instr& in) {
    const Operand& dst = in.ops[0];
    const UnorderedAccess* u = dst.type == kUav ? Uav(dst.index[0].imm) : nullptr;
    if (!u) return void(Fail("a typed store to something not a UAV"));
    const int c0 = 0;
    if (u->dim == ResourceDim::Buffer) {
      Use("r_fmt"), Use("r_utst");
      return Line("r_utst(" + UintLit(uint32_t(u->buffer)) + ", R_FMT_U" + std::to_string(u->slot) + ", " +
                  Src(in.ops[1], 'u', &c0, 1) + ", " + Src4(in.ops[2], 'u') + ");");
    }
    if (u->image < 0) return void(Fail("a typed store to a UAV that is not typed"));
    const char t = RetType(u->ret);
    Line("imageStore(r_img" + std::to_string(u->image) + ", " + ImageCoord(*u, in.ops[1]) + ", " + Src4(in.ops[2], t) +
         ");");
  }

  void Atomic(const Instr& in) {
    const bool imm = in.op >= kImmAtomicIadd;
    const size_t im = imm ? 1 : 0;  // the memory operand
    const Operand& mem = in.ops[im];
    const bool cas = in.op == kAtomicCmpStore || in.op == kImmAtomicCmpExch;
    const bool signed_minmax = in.op == kAtomicImax || in.op == kAtomicImin || in.op == kImmAtomicImax ||
                               in.op == kImmAtomicImin;
    static const std::map<uint32_t, const char*> kFns = {
        {kAtomicAnd, "And"}, {kAtomicOr, "Or"}, {kAtomicXor, "Xor"}, {kAtomicCmpStore, "CompSwap"},
        {kAtomicIadd, "Add"}, {kAtomicUmax, "Max"}, {kAtomicUmin, "Min"}, {kImmAtomicIadd, "Add"},
        {kImmAtomicAnd, "And"}, {kImmAtomicOr, "Or"}, {kImmAtomicXor, "Xor"}, {kImmAtomicExch, "Exchange"},
        {kImmAtomicCmpExch, "CompSwap"}, {kImmAtomicUmax, "Max"}, {kImmAtomicUmin, "Min"},
        {kAtomicImax, "Max"}, {kAtomicImin, "Min"}, {kImmAtomicImax, "Max"}, {kImmAtomicImin, "Min"}};
    const std::string fn = kFns.at(in.op);
    const std::string v = Scalar(in.ops[im + 2], 'u');
    const std::string v2 = cas ? Scalar(in.ops[im + 3], 'u') : "";
    Dst d;
    if (imm) d = DstOf(in.ops[0]);
    const bool result = d.n > 0;  // an imm_atomic's result (null: none)
    // The word: an lvalue and the condition that it is inside its memory.
    std::string word, inside;
    if (mem.type == kUav) {
      const UnorderedAccess* u = Uav(mem.index[0].imm);
      if (!u) return void(Fail("a UAV not declared"));
      if (u->image >= 0) {
        if (u->ret != ReturnType::Uint && u->ret != ReturnType::Sint)
          return void(Fail("an atomic on a UAV that is not an integer one"));
        const std::string img = "r_img" + std::to_string(u->image), c = ImageCoord(*u, in.ops[im + 1]);
        const char t = RetType(u->ret);
        std::string call = "imageAtomic" + fn + "(" + img + ", " + c + ", " + Conv(v, 'u', t, 1) +
                           (cas ? ", " + Conv(v2, 'u', t, 1) : "") + ")";
        if (signed_minmax && t != 'i') return void(Fail("a signed atomic on an unsigned image"));
        if (!result) return Line(call + ";");
        return Assign(d, Splat(Conv(call, t, 'u', 1), 'u', d.n), 'u', false);
      }
      const std::string b = std::to_string(u->buffer);
      // Raw: a byte address; structured: (element, byte offset); typed
      // buffers (R32 formats): an element.
      std::string addr = Scalar(in.ops[im + 1], 'u');
      if (u->dim == ResourceDim::Buffer) addr = "(" + addr + " * 4u)";
      if (u->dim == ResourceDim::StructuredBuffer) {
        const int c0 = 0, c1 = 1;
        addr = "(" + Src(in.ops[im + 1], 'u', &c0, 1) + " * " + UintLit(u->stride) + " + " +
               Src(in.ops[im + 1], 'u', &c1, 1) + ")";
      }
      Line("{");
      indent_++;
      Line("uint r_a = " + addr + ";");
      word = "r_rw[r_rwbuf[" + b + "].x + (r_a >> 2)]";
      inside = "r_a < r_rwbuf[" + b + "].y";
    } else if (mem.type == kTgsm) {
      auto it = tgsm_.find(mem.index[0].imm);
      if (it == tgsm_.end()) return void(Fail("group-shared memory not declared"));
      std::string addr = Scalar(in.ops[im + 1], 'u');
      if (it->second.stride) {
        const int c0 = 0, c1 = 1;
        addr = "(" + Src(in.ops[im + 1], 'u', &c0, 1) + " * " + UintLit(it->second.stride) + " + " +
               Src(in.ops[im + 1], 'u', &c1, 1) + ")";
      }
      Line("{");
      indent_++;
      Line("uint r_a = " + addr + " >> 2;");
      word = "r_g" + std::to_string(mem.index[0].imm) + "[r_a]";
      inside = "r_a < " + UintLit(it->second.words);
    } else {
      return void(Fail("an atomic on an operand of this type"));
    }
    if (signed_minmax) {
      // On int values of the uint words: a compare-and-swap loop.
      const char* pick = in.op == kAtomicImax || in.op == kImmAtomicImax ? "max" : "min";
      Line("uint r_old = 0u;");
      Line("if (" + inside + ") {");
      indent_++;
      Line("r_old = " + word + ";");
      Line("for (;;) {");
      Line("  uint r_p = atomicCompSwap(" + word + ", r_old, uint(" + pick + "(int(r_old), int(" + v + "))));");
      Line("  if (r_p == r_old) break;");
      Line("  r_old = r_p;");
      Line("}");
      indent_--;
      Line("}");
      if (result) Assign(d, Splat("r_old", 'u', d.n), 'u', false);
    } else {
      const std::string call = "atomic" + fn + "(" + word + ", " + v + (cas ? ", " + v2 : "") + ")";
      if (result) Assign(d, Splat("(" + inside + " ? " + call + " : 0u)", 'u', d.n), 'u', false);
      else Line("if (" + inside + ") " + call + ";");
    }
    indent_--;
    Line("}");
  }

  // imm_atomic_alloc (the counter before its increment) and
  // imm_atomic_consume (after its decrement) on a structured UAV's hidden
  // counter: a word of its own read-write buffer.
  void Counter(const Instr& in) {
    if (in.ops[1].type != kUav) return void(Fail("a counter of an operand of this type"));
    const UnorderedAccess* u = Uav(in.ops[1].index[0].imm);
    if (!u) return void(Fail("a UAV not declared"));
    const Dst d = DstOf(in.ops[0]);
    const std::string b = std::to_string(u->counter_buffer);
    const std::string word = "r_rw[r_rwbuf[" + b + "].x]", inside = "r_rwbuf[" + b + "].y >= 4u";
    const std::string v = in.op == kImmAtomicAlloc ? "atomicAdd(" + word + ", 1u)"
                                                   : "(atomicAdd(" + word + ", 0xFFFFFFFFu) - 1u)";
    if (d.n) Assign(d, Splat("(" + inside + " ? " + v + " : 0u)", 'u', d.n), 'u', false);
    else Line("if (" + inside + ") " + v + ";");
  }

  // ---- the stage's interface

  uint32_t InputCount() const { return ins_.empty() ? 0 : ins_.rbegin()->first + 1; }
  // A vertex input register that r_fetch fills (not only system values).
  bool Fetched(uint32_t reg) const {
    auto it = ins_.find(reg);
    if (it == ins_.end()) return false;
    for (const IoDecl& d : it->second)
      if (d.sv == 0) return true;
    return false;
  }
  uint32_t OutputCount() const {
    uint32_t n = outs_.empty() ? 0 : outs_.rbegin()->first + 1;
    if (VertexLike())
      for (const SignatureElement& el : sh_->outputs)
        if (el.system_value == 0 && el.reg < 32) n = std::max(n, el.reg + 1);
    return n;
  }

  // The stages whose outputs are the vertex's (a domain shader is drawn
  // as a vertex stage, dxbc.h).
  bool VertexLike() const { return stage_ == ShaderType::Vertex || stage_ == ShaderType::Domain; }

  static char CompType(uint32_t t) { return t == 1 ? 'u' : t == 2 ? 'i' : 'f'; }
  static std::string Vec4Of(char t) { return t == 'u' ? "uvec4" : t == 'i' ? "ivec4" : "vec4"; }
  static int Comps(uint8_t mask, int* c) {
    int n = 0;
    for (int i = 0; i < 4; i++)
      if (mask & (1 << i)) c[n++] = i;
    return n;
  }

  bool Finish() {
    std::string g, pre, post;
    char b[256];
    if (early_depth_ && stage_ == ShaderType::Pixel) g += "layout(early_fragment_tests) in;\n";
    // Textures and images.
    for (size_t i = 0; i < sh_->textures.size(); i++) {
      const TextureBinding& t = sh_->textures[i];
      const ShaderResource* r = Srv(t.resource);
      const char pfx = RetType(r->ret);
      const char* kind = "2D";
      switch (r->dim) {
        case ResourceDim::Texture1DArray: case ResourceDim::Texture2DArray: case ResourceDim::Texture2DMSArray:
        case ResourceDim::TextureCubeArray:
          kind = "2DArray";
          break;
        case ResourceDim::Texture3D: kind = "3D"; break;
        case ResourceDim::TextureCube: kind = "Cube"; break;
        default: break;
      }
      if (t.comparison && (pfx != 'f' || r->dim == ResourceDim::Texture3D))
        return Fail("a comparison sample of an integer or 3D texture");
      const char* type = pfx == 'f' ? "" : pfx == 'u' ? "u" : "i";
      if (t.apart >= 0) {
        // The texture apart, and r_tex<n> the pair made where it is used.
        std::snprintf(b, sizeof b, "layout(R_TEXTURE(%zu)) uniform %stexture%s r_timg%zu;  // t%u, s%d\n", i,
                      type, kind, i, t.resource, t.sampler);
        g += b;
        std::snprintf(b, sizeof b, "#define r_tex%zu %ssampler%s%s(r_timg%zu, r_smp%d)\n", i, type, kind,
                      t.comparison ? "Shadow" : "", i, t.apart);
        g += b;
        continue;
      }
      std::snprintf(b, sizeof b, "layout(R_TEXTURE(%zu)) uniform %ssampler%s%s r_tex%zu;  // t%u, s%d\n", i,
                    type, kind, t.comparison ? "Shadow" : "", i, t.resource, t.sampler);
      g += b;
    }
    for (size_t k = 0; k < sh_->samplers_apart.size(); k++) {
      const SamplerApart& s = sh_->samplers_apart[k];
      std::snprintf(b, sizeof b, "layout(R_SAMPLER(%zu)) uniform sampler%s r_smp%zu;  // s%d\n", k,
                    s.comparison ? "Shadow" : "", k, s.slot);
      g += b;
    }
    for (const UnorderedAccess& u : sh_->uavs) {
      if (u.image < 0) continue;
      const char t = RetType(u.ret);
      const char* kind = u.dim == ResourceDim::Texture3D ? "3D"
                         : u.dim == ResourceDim::Texture2DArray || u.dim == ResourceDim::Texture1DArray ? "2DArray"
                                                                                                         : "2D";
      const std::string fmt = u.read ? (t == 'u' ? ", r32ui" : t == 'i' ? ", r32i" : ", r32f") : "";
      std::snprintf(b, sizeof b, "layout(R_IMAGE(%d)%s) uniform %s%simage%s r_img%d;  // u%u\n", u.image, fmt.c_str(),
                    u.read ? "" : "writeonly ", t == 'f' ? "" : t == 'u' ? "u" : "i", kind, u.image, u.slot);
      g += b;
    }
    for (const auto& [slot, m] : tgsm_) {
      const std::string n = std::to_string(slot), w = UintLit(m.words);
      g += "shared uint r_g" + n + "[" + std::to_string(m.words) + "];\n";
      g += "uint r_g" + n + "_ld(uint a) { return (a >> 2) < " + w + " ? r_g" + n + "[a >> 2] : 0u; }\n";
      g += "void r_g" + n + "_st(uint a, uint v) { if ((a >> 2) < " + w + ") r_g" + n + "[a >> 2] = v; }\n";
    }
    if (!icb_.empty()) {
      const size_t n = icb_.size() / 4;
      g += "const uvec4 r_icb[" + std::to_string(n) + "] = uvec4[](";
      for (size_t i = 0; i < n; i++)
        g += Fmt("%suvec4(0x%Xu, 0x%Xu, 0x%Xu, 0x%Xu)", i ? ",\n    " : "\n    ", icb_[4 * i], icb_[4 * i + 1],
                 icb_[4 * i + 2], icb_[4 * i + 3]);
      g += ");\n";
    }

    // Inputs and outputs.
    const uint32_t nin = InputCount(), nout = OutputCount();
    if (domain_point_) {
      g += "#ifndef R_DOMAIN_POINT\n#define R_DOMAIN_POINT vec3(0.0)\n#endif\nuvec4 r_dom;\n";
      pre += "  r_dom = floatBitsToUint(vec4(R_DOMAIN_POINT, 0.0));\n";
    }
    if (VertexLike()) {
      for (const auto& [reg, decls] : ins_) {
        const std::string r = "r_v[" + std::to_string(reg) + "]";
        if (Fetched(reg)) pre += "  " + r + " = r_fetch(" + UintLit(reg) + ");\n";
        for (const IoDecl& d : decls) {
          int c[4];
          const int n = Comps(d.mask, c);
          switch (d.sv) {
            case 0: break;
            case 6: pre += "  " + r + "." + Chars(c, n) + " = " + Splat("R_VERTEX_ID", 'u', n) + ";\n"; break;
            case 8: pre += "  " + r + "." + Chars(c, n) + " = " + Splat("R_INSTANCE_ID", 'u', n) + ";\n"; break;
            default: return Fail(Fmt("a vertex input of system value %u not translated yet", d.sv));
          }
        }
      }
      g += "#ifndef R_VERTEX_ID\n#define R_VERTEX_ID uint(gl_VertexIndex)\n#endif\n"
           "#ifndef R_INSTANCE_ID\n#define R_INSTANCE_ID uint(gl_InstanceIndex)\n#endif\n";
      // Every varying of the signature is written, those the shader does
      // not declare as 0: a pixel shader may read them all.
      int clip = 0;
      for (size_t e = 0; e < sh_->outputs.size(); e++) {
        const SignatureElement& el = sh_->outputs[e];
        if (!el.mask || (!outs_.count(el.reg) && (el.system_value != 0 || el.reg >= 32))) continue;
        const std::string r = "r_o[" + std::to_string(el.reg) + "]";
        int c[4];
        const int n = Comps(el.mask, c);
        if (el.system_value == 1) {
          post += "  gl_Position = uintBitsToFloat(" + r + ");\n";
        } else if (el.system_value == 2) {
          for (int i = 0; i < n; i++)
            post += "  gl_ClipDistance[" + std::to_string(clip++) + "] = uintBitsToFloat(" + r + "." + kComp[c[i]] + ");\n";
        } else if (el.system_value == 0) {
          const char t = CompType(el.component_type);
          std::snprintf(b, sizeof b, "layout(location = %s) out %s r_out%zu;\n", LocationMacro(el).c_str(),
                        Vec4Of(t).c_str(), e);
          g += b;
          std::string v = r + "." + Chars(c, n);
          if (n < 4) v = Widen(v, 'u', n);
          post += "  r_out" + std::to_string(e) + " = " + Conv(v, 'u', t, 4) + ";\n";
        } else {
          return Fail(Fmt("a vertex output of system value %u not translated yet", el.system_value));
        }
      }
      if (clip)
        g += "out gl_PerVertex {\n  vec4 gl_Position;\n  float gl_PointSize;\n  float gl_ClipDistance[" +
             std::to_string(clip) + "];\n};\n";
      post += "  gl_PointSize = 1.0;\n";
    } else if (stage_ == ShaderType::Pixel) {
      for (const auto& [reg, decls] : ins_)
      for (const IoDecl& d : decls) {
        const std::string r = "r_v[" + std::to_string(reg) + "]";
        int c[4];
        const int n = Comps(d.mask, c);
        const std::string m = Chars(c, n);
        switch (d.sv) {
          case 0: break;  // varyings: by element, below
          case 1:
            pre += "  " + r + "." + m + " = floatBitsToUint(vec4(gl_FragCoord.xyz, 1.0 / gl_FragCoord.w)." + m + ");\n";
            break;
          case 7: pre += "  " + r + "." + m + " = " + Splat("uint(gl_PrimitiveID)", 'u', n) + ";\n"; break;
          case 9: pre += "  " + r + "." + m + " = " + Splat("(gl_FrontFacing ? 0xFFFFFFFFu : 0u)", 'u', n) + ";\n"; break;
          case 10: pre += "  " + r + "." + m + " = " + Splat("uint(gl_SampleID)", 'u', n) + ";\n"; break;
          case 4: pre += "  " + r + "." + m + " = " + Splat("uint(gl_Layer)", 'u', n) + ";\n"; break;
          case 5: pre += "  " + r + "." + m + " = " + Splat("uint(gl_ViewportIndex)", 'u', n) + ";\n"; break;
          default: return Fail(Fmt("a pixel input of system value %u not translated yet", d.sv));
        }
      }
      static const char* const kInterp[8] = {"", "flat ", "", "centroid ", "noperspective ",
                                              "noperspective centroid ", "sample ", "noperspective sample "};
      for (size_t e = 0; e < sh_->inputs.size(); e++) {
        const SignatureElement& el = sh_->inputs[e];
        auto it = ins_.find(el.reg);
        if (it == ins_.end() || el.system_value != 0 || !el.mask) continue;
        const IoDecl* d = nullptr;
        for (const IoDecl& x : it->second)
          if (x.sv == 0 && (x.mask & el.mask)) d = &x;
        if (!d) continue;
        const char t = CompType(el.component_type);
        const char* q = t != 'f' ? "flat " : kInterp[std::min(d->interp, 7u)];
        std::snprintf(b, sizeof b, "layout(location = %s) %sin %s r_in%zu;\n", LocationMacro(el).c_str(), q,
                      Vec4Of(t).c_str(), e);
        g += b;
        int c[4];
        const int n = Comps(el.mask, c);
        const std::string v = "r_in" + std::to_string(e) + "." + std::string(kComp, size_t(n));
        pre += "  r_v[" + std::to_string(el.reg) + "]." + Chars(c, n) + " = " + Conv(v, t, 'u', n) + ";\n";
      }
      for (const SignatureElement& el : sh_->outputs) {
        if (el.system_value != 64 || !outs_.count(el.reg)) continue;
        if (el.index >= 8) return Fail("a render target past 7");
        const char t = CompType(el.component_type);
        std::snprintf(b, sizeof b, "layout(location = %u) out %s r_target%u;\n", el.index, Vec4Of(t).c_str(), el.index);
        g += b;
        post += "  r_target" + std::to_string(el.index) + " = " +
                Conv("r_o[" + std::to_string(el.reg) + "]", 'u', t, 4) + ";\n";
        sh_->targets |= 1u << el.index;
      }
      for (const auto& [reg, decls] : outs_) {
        bool target = false;
        for (const SignatureElement& el : sh_->outputs) target |= el.system_value == 64 && el.reg == reg;
        if (!target) return Fail(Fmt("pixel output o%u is not a render target", reg));
      }
      if (out_depth_) {
        if (out_depth_ == kOutputDepthGe) g += "layout(depth_greater) out float gl_FragDepth;\n";
        if (out_depth_ == kOutputDepthLe) g += "layout(depth_less) out float gl_FragDepth;\n";
        // A path that writes no depth keeps the rasterized one.
        g += "uint r_odepth;\n";
        pre += "  r_odepth = floatBitsToUint(gl_FragCoord.z);\n";
        post += "  gl_FragDepth = uintBitsToFloat(r_odepth);\n";
      }
      if (out_mask_) {
        g += "uint r_omask = 0xFFFFFFFFu;\n";
        post += "  gl_SampleMask[0] = int(r_omask);\n";
      }
      if (coverage_) {
        g += "uint r_cov;\n";
        pre += "  r_cov = uint(gl_SampleMaskIn[0]);\n";
      }
    }
    if (nin) g += "uvec4 r_v[" + std::to_string(nin) + "];\n";
    if (nout) g += "uvec4 r_o[" + std::to_string(nout) + "];\n";
    for (const auto& h : kHelpers)
      if (helpers_.count(h[0])) g += h[1];

    g += "void r_main() {\n";
    for (uint32_t i = 0; i < temps_; i++)
      g += std::string(i % 8 ? ", " : i ? ";\n  uvec4 " : "  uvec4 ") + "r" + std::to_string(i) + " = uvec4(0u)";
    if (temps_) g += ";\n";
    for (const auto& [reg, size] : xtemps_) {
      const std::string x = "x" + std::to_string(reg);
      g += "  uvec4 " + x + "[" + std::to_string(size) + "];\n";
      g += "  for (int i = 0; i < " + std::to_string(size) + "; i++) " + x + "[i] = uvec4(0u);\n";
    }
    g += body_;
    g += "}\nvoid main() {\n";
    for (uint32_t i = 0; i < nin; i++)
      if (!VertexLike() || !Fetched(i)) g += "  r_v[" + std::to_string(i) + "] = uvec4(0u);\n";
    if (nout) g += "  for (int i = 0; i < " + std::to_string(nout) + "; i++) r_o[i] = uvec4(0u);\n";
    g += pre + "  r_main();\n" + post + "}\n";
    sh_->stage.glsl = std::move(g);
    return true;
  }

  const Program& p_;
  Shader* sh_;
  const bool annotate_;
  ShaderType stage_ = ShaderType::Pixel;
  const Instr* cur_ = nullptr;
  std::string body_;
  int indent_ = 1;
  std::vector<char> flow_;
  bool last_label_ = false, early_depth_ = false, out_mask_ = false, coverage_ = false;
  bool domain_point_ = false;
  uint32_t out_depth_ = 0;
  std::set<std::string> helpers_;
  uint32_t temps_ = 0;
  std::map<uint32_t, uint32_t> xtemps_;  // x# and its size
  std::vector<uint32_t> icb_;
  std::map<uint32_t, int> cb_;  // slot: gfx buffer
  std::map<std::tuple<uint32_t, int, bool>, int> pairs_;  // (t#, s#, comparison): gfx texture
  struct Tgsm {
    uint32_t words = 0, stride = 0;
  };
  std::map<uint32_t, Tgsm> tgsm_;
  std::map<uint32_t, std::vector<IoDecl>> ins_, outs_;
};

}  // namespace

namespace {

bool Load(const void* dxbc, size_t size, Program* p) {
  std::vector<Chunk> chunks;
  std::string err;
  return Chunks(static_cast<const uint8_t*>(dxbc), size, &chunks, &err) && LoadProgram(chunks, p, &err);
}

// mov o#, src# into the output register of the source's number, each
// component from itself: a stage passing a register through. The source
// is v# or, with `point`, the patch's first control point's register
// (v[0][#] in a hull shader's control point phase).
bool CopiesRegister(const Instr& in, bool point) {
  if (in.op != kMov || in.sat || in.ops.size() != 2) return false;
  const Operand& d = in.ops[0];
  const Operand& s = in.ops[1];
  if (d.type != kOutput || d.ndim != 1 || d.index[0].rel >= 0 || d.ncomp != 4 || d.mode != 0) return false;
  if ((s.type != kInput && !(point && s.type == kInputControlPoint)) || s.mod || s.ncomp != 4) return false;
  const int dim = point ? 1 : 0;
  if (s.ndim != uint32_t(dim + 1) || s.index[dim].rel >= 0 || s.index[dim].imm != d.index[0].imm) return false;
  if (dim && (s.index[0].rel >= 0 || s.index[0].imm != 0)) return false;
  for (int c = 0; c < 4; c++)
    if ((d.mask >> c) & 1) {
      const int from = s.mode == 0 ? c : s.swz[c];
      if (from != c) return false;
    }
  return true;
}

}  // namespace

bool PassThroughVertexShader(const void* dxbc, size_t size, std::vector<ConstantOutput>* constants) {
  Program p;
  constants->clear();
  if (!Load(dxbc, size, &p) || p.type != ShaderType::Vertex) return false;
  for (const Instr& in : p.instrs) {
    if (IsDeclOp(in.op) || in.op == kRet || CopiesRegister(in, false)) continue;
    // mov o#.mask, l(...): constants into an output register.
    if (in.op != kMov || in.sat || in.ops.size() != 2) return false;
    const Operand& d = in.ops[0];
    const Operand& s = in.ops[1];
    if (d.type != kOutput || d.ndim != 1 || d.index[0].rel >= 0 || d.ncomp != 4 || d.mode != 0 ||
        s.type != kImm32 || s.mod)
      return false;
    for (uint32_t c = 0; c < 4; c++)
      if ((d.mask >> c) & 1) constants->push_back({d.index[0].imm, c, s.imm[s.ncomp == 4 ? c : 0]});
  }
  return true;
}

bool PointToQuadHull(const void* dxbc, size_t size, bool* ccw, uint32_t* segments) {
  Program p;
  if (!Load(dxbc, size, &p) || p.type != ShaderType::Hull) return false;
  enum { kDecls, kControlPoints, kPatch } phase = kDecls;
  uint32_t factors = 0;  // the tessellation factors declared (bit: system value - 11)
  bool quad = false, prim = false;
  uint32_t partitioning = 0;
  float factor = 0;  // the largest factor
  std::vector<uint32_t> icb;  // the immediate constant buffer
  for (const Instr& in : p.instrs) {
    const uint32_t ctl = (in.token >> 11) & 0x1FFF;
    switch (in.op) {
      case kHsDecls: continue;
      case kCustomData:
        if ((in.token >> 11) == 3) icb = in.lits;
        continue;
      case kHsControlPointPhase: phase = kControlPoints; continue;
      case kHsForkPhase: case kHsJoinPhase: phase = kPatch; continue;
      case kDclInputControlPointCount: case kDclOutputControlPointCount:
        if ((ctl & 0x3F) != 1) return false;
        continue;
      case kDclTessDomain: quad = (ctl & 3) == 3; continue;
      case kDclTessPartitioning: partitioning = ctl & 7; continue;
      case kDclTessOutputPrimitive:
        if ((ctl & 7) != 3 && (ctl & 7) != 4) return false;  // triangle_cw, triangle_ccw
        prim = true;
        *ccw = (ctl & 7) == 4;
        continue;
      case kDclOutputSiv:
        if (!in.lits.empty() && in.lits[0] >= 11 && in.lits[0] <= 16) factors |= 1u << (in.lits[0] - 11);
        continue;
      case kRet: continue;
      default: break;
    }
    if (IsDeclOp(in.op)) continue;
    if (phase == kControlPoints) {
      if (!CopiesRegister(in, true)) return false;
      continue;
    }
    if (phase != kPatch || in.op != kMov || in.sat || in.ops.size() != 2) return false;
    // The patch phases: the fork/join instance number into a temp (to
    // index the factors by), and 1.0 into each factor.
    const Operand& d = in.ops[0];
    const Operand& s = in.ops[1];
    if (d.type == kTemp && (s.type == kInputForkInstanceId || s.type == kInputJoinInstanceId)) continue;
    if (d.type != kOutput || s.mod) return false;
    // Each a whole number of segments, as the partitioning makes of it:
    // integer any, pow2 a power of two, fractional_odd an odd number,
    // fractional_even an even one (D3D11_TESSELLATOR_PARTITIONING 1-4).
    auto whole = [&](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, 4);
      const uint32_t n = uint32_t(f);
      if (float(n) != f || n < 1 || n > 16) return false;
      factor = std::max(factor, f);
      return partitioning == 1 || (partitioning == 2 && !(n & (n - 1))) || (partitioning == 3 && (n & 1)) ||
             (partitioning == 4 && !(n & 1));
    };
    if (s.type == kImm32) {
      for (uint32_t c = 0; c < (s.ncomp == 4 ? 4u : 1u); c++)
        if (!whole(s.imm[c])) return false;
    } else if (s.type == kImmConstantBuffer && !icb.empty()) {
      // A factor from the immediate constant buffer (by the fork phase's
      // instance): every vector's component it reads.
      const uint32_t c = s.ncomp == 4 && s.mode != 0 ? s.swz[0] : 0;
      for (size_t v = 0; v + 3 < icb.size(); v += 4)
        if (!whole(icb[v + c])) return false;
    } else {
      return false;
    }
  }
  if (!quad || !prim || factors != 0x3F) return false;
  // Factors that differ (edges finer than the inside) are drawn as the
  // grid of the largest: the same points on the edges, and more inside,
  // where a sprite's domain shader is linear.
  *segments = uint32_t(factor);
  return true;
}

bool Translate(const void* dxbc, size_t size, Shader* out, bool annotate) {
  *out = Shader{};
  std::vector<Chunk> chunks;
  if (!Chunks(static_cast<const uint8_t*>(dxbc), size, &chunks, &out->error)) return false;
  for (const Chunk& c : chunks) {
    if (c.fourcc == FourCC("ISGN") || c.fourcc == FourCC("ISG1")) {
      if (!Signature(c, &out->inputs, &out->error)) return false;
    } else if (c.fourcc == FourCC("OSGN") || c.fourcc == FourCC("OSG1") || c.fourcc == FourCC("OSG5")) {
      if (!Signature(c, &out->outputs, &out->error)) return false;
    }
  }
  Program prog;
  if (!LoadProgram(chunks, &prog, &out->error)) return false;
  Translator t(prog, out, annotate);
  if (!t.Run()) {
    out->error = t.error_;
    out->stage.glsl.clear();
    return false;
  }
  return true;
}

std::string LocationMacro(const SignatureElement& e) {
  std::string s = "R_LOC_";
  for (char c : e.semantic) s += char(std::toupper(uint8_t(c)));
  return s + std::to_string(e.index);
}

std::string VaryingDefines(const Shader* vs, const Shader* ps) {
  std::vector<std::string> order;
  std::set<std::string> seen;
  auto add = [&](const std::vector<SignatureElement>& els) {
    for (const SignatureElement& e : els) {
      if (e.system_value != 0) continue;
      const std::string m = LocationMacro(e);
      if (seen.insert(m).second) order.push_back(m);
    }
  };
  if (vs) add(vs->outputs);
  if (ps) add(ps->inputs);
  std::string s;
  for (size_t i = 0; i < order.size(); i++) s += "#define " + order[i] + " " + std::to_string(i) + "\n";
  return s;
}

bool FormatSupported(uint32_t f) {
  static const std::set<uint32_t> kFormats = {2,  3,  4,  6,  7,  8,  10, 11, 12, 13, 14, 16, 17, 18, 24, 25, 28, 30,
                                              31, 32, 34, 35, 36, 37, 38, 41, 42, 43, 49, 50, 51, 52, 54, 56, 57, 58,
                                              59, 61, 62, 63, 64, 87};
  return kFormats.count(f) != 0;
}

}  // namespace recomp::hle::d3d11::dxbc
