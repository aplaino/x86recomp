// GCN to GLSL (orbis/gpu/shader.h). Field layouts of the descriptors (V#,
// T#, S#) and the instructions' semantics follow AMD's Sea Islands
// instruction set reference.

#include "orbis/gpu/shader.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "orbis/gpu/gcn.h"
#include "runtime/runtime.h"

namespace recomp::orbis::gpu {
namespace {

using gcn::Fmt;
using gcn::Instr;

// ---- the code: instructions by address, from the stage's code and the
// fetch shader it calls

struct Code {
  std::map<gaddr, Instr> at;  // every reachable instruction
  std::set<gaddr> leaders;    // first instructions of basic blocks
  std::set<gaddr> fetch;      // the fetch shader's instructions
  gaddr base[2] = {};         // the stage's code, the fetch shader
  bool branches = false;      // any branch, call or return
};

constexpr uint32_t kMaxInstructions = 1 << 16;

// Whether n bytes at a are guest memory (draws can point anywhere).
bool Readable(gaddr a, uint64_t n) {
  return g_proc->vm.mem().IsCommitted(a) && g_proc->vm.mem().IsCommitted(a + n - 1);
}

int64_t Simm16(const Instr& i) { return int16_t(i.w0 & 0xFFFF); }
bool IsSopp(const Instr& i, uint32_t op) { return i.fmt == Fmt::Sopp && i.op == op; }
bool IsBranch(const Instr& i) {
  return i.fmt == Fmt::Sopp && (i.op == 2 || (i.op >= 4 && i.op <= 9));
}

// The instructions reachable from entry until s_endpgm (or, in a fetch
// shader, s_setpc_b64), with the blocks they start; false with the
// reason if an instruction does not decode.
bool Walk(gaddr entry, Code& c, std::string* err) {
  std::vector<gaddr> work{entry};
  c.leaders.insert(entry);
  while (!work.empty()) {
    gaddr pc = work.back();
    work.pop_back();
    for (;;) {
      if (c.at.count(pc)) break;
      if (c.at.size() > kMaxInstructions) {
        *err = "a shader with too many instructions";
        return false;
      }
      Instr in;
      if (!Readable(pc, 12)) {
        char b[64];
        std::snprintf(b, sizeof b, "code at %llX outside memory", (unsigned long long)pc);
        *err = b;
        return false;
      }
      if (!gcn::Decode(reinterpret_cast<const uint32_t*>(GPtr(pc)), 3, &in)) {
        char b[96];
        std::snprintf(b, sizeof b, "an undecodable word %08X at %llX", Rd32(pc),
                      (unsigned long long)pc);
        *err = b;
        return false;
      }
      c.at[pc] = in;
      const gaddr next = pc + 4 * uint64_t(in.dwords);
      if (IsSopp(in, 1)) break;  // s_endpgm
      if (in.fmt == Fmt::Sop1 && in.op == 32) {  // s_setpc_b64: a fetch shader's return
        c.branches = true;
        break;
      }
      if (in.fmt == Fmt::Sop1 && in.op == 33) {  // s_swappc_b64: returns here
        c.branches = true;
        c.leaders.insert(next);
      }
      if (IsBranch(in)) {
        c.branches = true;
        const gaddr target = next + 4 * Simm16(in);
        c.leaders.insert(target);
        c.leaders.insert(next);
        work.push_back(target);
        if (in.op == 2) break;  // s_branch
      }
      pc = next;
    }
  }
  return true;
}

// ---- the scalar evaluation (Resolve)

// SGPRs 0-103, VCC 106-107, M0 124 and EXEC 126-127, and whether each is
// known.
struct Scalars {
  uint32_t v[128] = {};
  bool known[128] = {};
  bool Get(uint32_t r, uint32_t* out) const {
    if (r >= 128 || !known[r]) return false;
    *out = v[r];
    return true;
  }
  void Set(uint32_t r, uint32_t x) {
    if (r < 128) v[r] = x, known[r] = true;
  }
  void Forget(uint32_t r) {
    if (r < 128) known[r] = false;
  }
};

// A scalar source operand's value: registers, inline constants, the
// literal.
bool Source(const Scalars& s, const Instr& in, uint32_t code, uint32_t* out) {
  if (code < 128) return s.Get(code, out);
  if (code == gcn::kZero) return *out = 0, true;
  if (code >= gcn::kIntPos && code < gcn::kIntPos + 64) return *out = code - 128, true;
  if (code >= gcn::kIntNeg && code < gcn::kIntNeg + 16) return *out = uint32_t(192 - int(code)), true;
  if (code >= gcn::kFloatHalf && code < gcn::kFloatHalf + 8) {
    static const float kF[8] = {0.5f, -0.5f, 1, -1, 2, -2, 4, -4};
    std::memcpy(out, &kF[code - gcn::kFloatHalf], 4);
    return true;
  }
  if (code == gcn::kLiteral) return *out = in.literal, true;
  return false;
}

// The resources a stage's memory instructions use, in binding order, by
// the instruction's place (its region and offset).
struct Site {
  int buffer = -1, image = -1;
  uint32_t vsharp[4] = {};  // buffer instructions: their V#
};

struct Resolver {
  const StageState& st;
  Code code;
  Resolved r;
  Scalars s;
  std::map<gaddr, Site> sites;

  explicit Resolver(const StageState& st) : st(st) {}

  int Buffer(gaddr base, uint32_t size, bool written) {
    for (size_t i = 0; i < r.buffers.size(); i++)
      if (r.buffers[i].base == base && r.buffers[i].size == size) {
        r.buffers[i].written |= written;
        return int(i);
      }
    r.buffers.push_back({base, size, written});
    return int(r.buffers.size() - 1);
  }

  bool Fail(gaddr pc, const char* what) {
    char b[160];
    std::snprintf(b, sizeof b, "%s at %llX (%s)", what, (unsigned long long)pc,
                  gcn::Disassemble(code.at[pc]).c_str());
    r.error = b;
    return false;
  }

  bool Vsharp(uint32_t first, uint32_t v[4]) {
    for (int i = 0; i < 4; i++)
      if (!s.Get(first + uint32_t(i), &v[i])) return false;
    return true;
  }

  // A V#'s range: stride x records, or records bytes without a stride.
  static void Range(const uint32_t v[4], gaddr* base, uint32_t* size) {
    *base = v[0] | uint64_t(v[1] & 0xFFFF) << 32;
    const uint32_t stride = (v[1] >> 16) & 0x3FFF;
    const uint64_t bytes = stride ? uint64_t(stride) * v[2] : v[2];
    *size = uint32_t(std::min<uint64_t>(bytes, 256u << 20));
  }

  // Runs the scalar instruction at pc.
  bool Step(gaddr pc, const Instr& in) {
    const uint32_t w = in.w0;
    switch (in.fmt) {
      case Fmt::Sop1: {
        const uint32_t d = (w >> 16) & 0x7F;
        uint32_t a, b;
        switch (in.op) {
          case 3:  // s_mov_b32
            if (Source(s, in, w & 0xFF, &a)) s.Set(d, a); else s.Forget(d);
            return true;
          case 4: {  // s_mov_b64: constants sign-extended, the literal zero-extended
            const uint32_t c = w & 0xFF;
            bool ok;
            if (c < 128) {
              ok = s.Get(c, &a) && s.Get(c + 1, &b);
            } else {
              ok = Source(s, in, c, &a);
              b = c == gcn::kLiteral || int32_t(a) >= 0 ? 0 : ~0u;
            }
            if (ok) {
              s.Set(d, a);
              s.Set(d + 1, b);
            } else {
              s.Forget(d);
              s.Forget(d + 1);
            }
            return true;
          }
          case 31:  // s_getpc_b64
            s.Set(d, uint32_t(pc + 4));
            s.Set(d + 1, uint32_t((pc + 4) >> 32));
            return true;
          case 33: {  // s_swappc_b64: the fetch shader
            uint32_t lo, hi;
            if (!s.Get(w & 0xFF, &lo) || !s.Get((w & 0xFF) + 1, &hi))
              return Fail(pc, "a call to an unknown address");
            const gaddr target = lo | uint64_t(hi & 0xFFFF) << 32;
            if (code.base[1] && code.base[1] != target)
              return Fail(pc, "calls to more than one fetch shader");
            code.base[1] = target;
            r.fetch = target;
            s.Set(d, uint32_t(pc + 4));
            s.Set(d + 1, uint32_t((pc + 4) >> 32));
            Code fc;
            if (!Walk(target, fc, &r.error)) return false;
            if (fc.branches && !std::any_of(fc.at.begin(), fc.at.end(), [](auto& e) {
                  return e.second.fmt == Fmt::Sop1 && e.second.op == 32;
                }))
              return Fail(pc, "a fetch shader without a return");
            for (auto& [a, i] : fc.at) {
              if (IsBranch(i)) return Fail(a, "a branch in a fetch shader");
              code.at[a] = i;
              code.fetch.insert(a);
            }
            code.leaders.insert(target);
            for (auto& [a, i] : fc.at)
              if (!Step(a, i)) return false;
            return true;
          }
          case 32: return true;  // s_setpc_b64 (the fetch shader's return)
          default:
            s.Forget(d);
            if (in.op == 4 || (in.op >= 36 && in.op <= 43)) s.Forget(d + 1);
            return true;
        }
      }
      case Fmt::Sopk: {
        const uint32_t d = (w >> 16) & 0x7F;
        uint32_t a;
        if (in.op == 0) s.Set(d, uint32_t(int32_t(int16_t(w & 0xFFFF))));
        else if (in.op == 15 && s.Get(d, &a)) s.Set(d, a + uint32_t(int32_t(int16_t(w & 0xFFFF))));
        else if (in.op == 16 && s.Get(d, &a)) s.Set(d, a * uint32_t(int32_t(int16_t(w & 0xFFFF))));
        else if (in.op >= 3 && in.op <= 14) {}  // compares: SCC only
        else s.Forget(d);
        return true;
      }
      case Fmt::Sop2: {
        const uint32_t d = (w >> 16) & 0x7F;
        uint32_t a, b;
        const bool k = Source(s, in, w & 0xFF, &a) && Source(s, in, (w >> 8) & 0xFF, &b);
        const bool wide = in.op == 11 || in.op == 15 || in.op == 17 || in.op == 19 || in.op == 21 ||
                          in.op == 23 || in.op == 25 || in.op == 27 || in.op == 29 || in.op == 31 ||
                          in.op == 33 || in.op == 35 || in.op == 37 || in.op == 41 || in.op == 42;
        if (!k || wide) {
          s.Forget(d);
          if (wide) s.Forget(d + 1);
          return true;
        }
        switch (in.op) {
          case 0: case 2: s.Set(d, a + b); break;
          case 1: case 3: s.Set(d, a - b); break;
          case 14: s.Set(d, a & b); break;
          case 16: s.Set(d, a | b); break;
          case 18: s.Set(d, a ^ b); break;
          case 30: s.Set(d, a << (b & 31)); break;
          case 32: s.Set(d, a >> (b & 31)); break;
          case 34: s.Set(d, uint32_t(int32_t(a) >> (b & 31))); break;
          case 38: s.Set(d, a * b); break;
          case 39: {  // s_bfe_u32
            const uint32_t off = b & 31, width = (b >> 16) & 0x7F;
            s.Set(d, width ? (a >> off) & (width >= 32 ? ~0u : (1u << width) - 1) : 0);
            break;
          }
          default: s.Forget(d); break;
        }
        return true;
      }
      case Fmt::Smrd: {
        const uint32_t d = (w >> 15) & 0x7F, base = ((w >> 9) & 0x3F) * 2;
        if (in.op >= 29) {  // s_memtime, cache control
          if (in.op == 30) s.Forget(d), s.Forget(d + 1);
          return true;
        }
        const uint32_t n = 1u << (in.op & 7);
        uint32_t off;
        if (w & 0x100) off = (w & 0xFF) * 4;
        else if (in.literal_used) off = in.literal * 4;
        else if (!s.Get(w & 0xFF, &off)) return Fail(pc, "a scalar load at an unknown offset");
        gaddr a;
        Site& site = sites[pc];
        if (in.op >= 8) {  // s_buffer_load: from a V#
          uint32_t v[4];
          if (!Vsharp(base, v)) return Fail(pc, "a scalar buffer load from an unknown V#");
          uint32_t size;
          gaddr vb;
          Range(v, &vb, &size);
          site.buffer = Buffer(vb, size, false);
          std::memcpy(site.vsharp, v, sizeof v);
          a = vb + off;
          if (!Readable(vb, std::max(size, 1u))) return Fail(pc, "a scalar buffer load outside memory");
          for (uint32_t i = 0; i < n; i++)
            if (off + 4 * i + 4 <= size) s.Set(d + i, Rd32(a + 4 * i)); else s.Set(d + i, 0);
        } else {
          uint32_t lo, hi;
          if (!s.Get(base, &lo) || !s.Get(base + 1, &hi))
            return Fail(pc, "a scalar load from an unknown address");
          a = (lo | uint64_t(hi & 0xFFFF) << 32) + off;
          if (!Readable(a, 4 * n)) return Fail(pc, "a scalar load outside memory");
          site.buffer = Buffer(a & ~uint64_t(3), 4 * n, false);
          for (uint32_t i = 0; i < n; i++) s.Set(d + i, Rd32(a + 4 * i));
        }
        return true;
      }
      case Fmt::Mubuf: case Fmt::Mtbuf: {
        uint32_t v[4];
        if (!Vsharp(((in.w1 >> 16) & 0x1F) * 4, v)) return Fail(pc, "a buffer access with an unknown V#");
        const bool store = in.fmt == Fmt::Mubuf ? (in.op >= 4 && in.op <= 7) || (in.op >= 24 && in.op <= 31) ||
                                                      (in.op >= 48 && in.op <= 64)
                                                : in.op >= 4;
        if (in.fmt == Fmt::Mubuf && in.op >= 112) return true;  // cache control
        gaddr base;
        uint32_t size;
        Range(v, &base, &size);
        Site& site = sites[pc];
        site.buffer = Buffer(base, size, store);
        std::memcpy(site.vsharp, v, sizeof v);
        return true;
      }
      case Fmt::Mimg: {
        ImageRef ref;
        const uint32_t t = ((in.w1 >> 16) & 0x1F) * 4, smp = ((in.w1 >> 21) & 0x1F) * 4;
        const int words = (in.w0 & 0x8000) ? 4 : 8;
        for (int i = 0; i < words; i++)
          if (!s.Get(t + uint32_t(i), &ref.t[i])) return Fail(pc, "an image access with an unknown T#");
        if (in.op >= 32)  // samples, gathers, levels: with a sampler
          for (int i = 0; i < 4; i++)
            if (!s.Get(smp + uint32_t(i), &ref.s[i])) return Fail(pc, "a sample with an unknown S#");
        ref.storage = in.op >= 8 && in.op <= 11;
        // Same texture and sampler: one binding.
        int at = -1;
        for (size_t i = 0; i < r.images.size(); i++)
          if (!std::memcmp(r.images[i].t, ref.t, sizeof ref.t) &&
              !std::memcmp(r.images[i].s, ref.s, sizeof ref.s) && r.images[i].storage == ref.storage)
            at = int(i);
        if (at < 0) {
          r.images.push_back(ref);
          at = int(r.images.size() - 1);
        }
        sites[pc].image = at;
        return true;
      }
      case Fmt::Vop1:
        if (in.op == 2) s.Forget((w >> 17) & 0xFF);  // v_readfirstlane_b32
        return true;
      case Fmt::Vopc:
        s.Forget(gcn::kVccLo), s.Forget(gcn::kVccHi);
        return true;
      case Fmt::Vop2:
        if (in.op == 1 || (in.op >= 37 && in.op <= 42)) s.Forget(gcn::kVccLo), s.Forget(gcn::kVccHi);
        return true;
      case Fmt::Vop3: {
        const uint32_t d = w & 0xFF;
        if (in.op < 256 || in.op == 257 || in.op == 365 || in.op == 366)
          s.Forget(d), s.Forget(d + 1);
        if (in.op >= 293 && in.op <= 298) {
          const uint32_t sd = (w >> 8) & 0x7F;
          s.Forget(sd), s.Forget(sd + 1);
        }
        return true;
      }
      default:
        return true;
    }
  }

  bool Run() {
    code.base[0] = st.code;
    if (!Walk(st.code, code, &r.error)) return false;
    const uint32_t user = (st.rsrc2 >> 1) & 0x1F;
    for (uint32_t i = 0; i < user && i < 16; i++) s.Set(i, st.user[i]);
    s.Set(gcn::kExecLo, 1);
    s.Set(gcn::kExecHi, 0);
    // Program order: the stage's code by address, a call running the
    // fetch shader where it is.
    std::vector<std::pair<gaddr, Instr>> order(code.at.begin(), code.at.end());
    for (auto& [pc, in] : order)
      if (!Step(pc, in)) return false;
    return true;
  }
};

// ---- formats

// Buffer data formats (BUF_DATA_FORMAT) and number formats.
struct BufFmt {
  uint32_t dfmt, nfmt;
};
uint32_t BufComponents(uint32_t dfmt) {
  static const uint8_t k[16] = {0, 1, 1, 2, 1, 2, 3, 3, 4, 4, 4, 2, 4, 3, 4, 0};
  return k[dfmt & 15];
}
uint32_t BufBytes(uint32_t dfmt) {
  static const uint8_t k[16] = {0, 1, 2, 2, 4, 4, 4, 4, 4, 4, 4, 8, 8, 12, 16, 0};
  return k[dfmt & 15];
}

// Image formats: the texture kind of a T#'s type, and whether its values
// are integers.
gfx::TextureKind ImageKind(const uint32_t t[8]) {
  switch (t[3] >> 28) {
    case 10: return gfx::TextureKind::Tex3D;
    case 11: return gfx::TextureKind::Cube;
    case 12: case 13: case 15: return gfx::TextureKind::Tex2DArray;
    default: return gfx::TextureKind::Tex2D;
  }
}
// 'f', 'u' or 'i': what the shader reads.
char ImageType(const uint32_t t[8]) {
  const uint32_t nfmt = (t[1] >> 26) & 0xF;
  return nfmt == 4 ? 'u' : nfmt == 5 ? 'i' : 'f';
}

// ---- the translation

struct Translator {
  const StageState& st;
  const Resolver& res;
  std::string body, decl, error;
  int max_vgpr = -1;
  std::set<uint32_t> params;           // VS: exported parameters
  std::map<uint32_t, bool> inputs;     // PS: input locations, flat
  std::set<uint32_t> mrts;             // PS: exported targets
  bool depth = false, uses_lds = false;
  std::map<gaddr, int> block_of;
  std::vector<int> ro_of, rw_of;       // binding -> RO / RW index (-1: not that kind)
  int nro = 0, nrw = 0;

  Translator(const StageState& st, const Resolver& res) : st(st), res(res) {}

  void Fail(gaddr pc, const char* what) {
    if (!error.empty()) return;
    char b[200];
    auto it = res.code.at.find(pc);
    std::snprintf(b, sizeof b, "%s at %llX (%s)", what, (unsigned long long)pc,
                  it == res.code.at.end() ? "?" : gcn::Disassemble(it->second).c_str());
    error = b;
  }

  // ---- operands (every value is a uint)

  std::string V(uint32_t n) {
    max_vgpr = std::max(max_vgpr, int(n));
    return "v" + std::to_string(n);
  }
  static std::string S(uint32_t n) {
    switch (n) {
      case gcn::kVccLo: return "vcc_lo";
      case gcn::kVccHi: return "vcc_hi";
      case gcn::kM0: return "m0";
      case gcn::kExecLo: return "exec_lo";
      case gcn::kExecHi: return "exec_hi";
    }
    return "s" + std::to_string(n);
  }
  static std::string U(uint32_t x) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%Xu", x);
    return b;
  }
  // A 9-bit source (8-bit scalar sources use the same codes below 256).
  std::string Src(uint32_t code, const Instr& in, gaddr pc) {
    if (code >= gcn::kVgpr) return V(code - gcn::kVgpr);
    if (code < 104 || code == gcn::kVccLo || code == gcn::kVccHi || code == gcn::kM0 ||
        code == gcn::kExecLo || code == gcn::kExecHi)
      return S(code);
    uint32_t c;
    Scalars none;
    if (Source(none, in, code, &c)) return U(c);
    switch (code) {
      case gcn::kVccz: return "((vcc_lo & 1u) == 0u ? 1u : 0u)";
      case gcn::kExecz: return "((exec_lo & 1u) == 0u ? 1u : 0u)";
      case gcn::kScc: return "scc";
    }
    Fail(pc, "an operand not translated yet");
    return "0u";
  }
  std::string Dst(uint32_t code) { return code < 128 ? S(code) : "r_unused"; }

  // ---- emitting

  void Line(const std::string& s) { body += "  " + s + "\n"; }

  static std::string F(const std::string& u) { return "uintBitsToFloat(" + u + ")"; }
  static std::string Bits(const std::string& f) { return "floatBitsToUint(" + f + ")"; }

  // A float source with VOP3's abs and neg.
  std::string FSrc(const std::string& u, bool abs, bool neg) {
    std::string f = F(u);
    if (abs) f = "abs(" + f + ")";
    if (neg) f = "(-" + f + ")";
    return f;
  }
  // A float result with VOP3's output modifier and clamp.
  std::string FOut(std::string f, uint32_t omod, bool clamp) {
    if (omod == 1) f = "(" + f + " * 2.0)";
    else if (omod == 2) f = "(" + f + " * 4.0)";
    else if (omod == 3) f = "(" + f + " * 0.5)";
    if (clamp) f = "clamp(" + f + ", 0.0, 1.0)";
    return Bits(f);
  }

  // A VALU operation: its name (VOP1/VOP2/VOP3 numbering: VOP2's op,
  // VOP1's op + 384, VOP3-only ops), sources and destination.
  bool Valu(gaddr pc, const Instr& in, uint32_t op, const std::string d, const std::string s[3],
            bool abs[3], bool neg[3], uint32_t omod, bool clamp, uint32_t sdst) {
    auto f = [&](int i) { return FSrc(s[i], abs[i], neg[i]); };
    auto fo = [&](const std::string& e) { Line(d + " = " + FOut(e, omod, clamp) + ";"); };
    auto io = [&](const std::string& e) { Line(d + " = " + e + ";"); };
    auto si = [&](int i) { return "int(" + s[i] + ")"; };
    // Carry-out ops write sdst (VCC for VOP2).
    auto carry = [&](const std::string& e) {
      Line(Dst(sdst) + " = (" + e + ") ? 1u : 0u; " + Dst(sdst + 1) + " = 0u;");
    };
    switch (op) {
      case 0:  // v_cndmask_b32: s1 if the lane's bit of s2 (VCC) is set
        io("((" + s[2] + " & 1u) != 0u ? " + s[1] + " : " + s[0] + ")");
        return true;
      case 3: fo(f(0) + " + " + f(1)); return true;
      case 4: fo(f(0) + " - " + f(1)); return true;
      case 5: fo(f(1) + " - " + f(0)); return true;
      case 6:  // v_mac_legacy_f32: 0 * anything is 0
        fo("r_mul_legacy(" + f(0) + ", " + f(1) + ") + " + F(d));
        return true;
      case 7: fo("r_mul_legacy(" + f(0) + ", " + f(1) + ")"); return true;
      case 8: fo(f(0) + " * " + f(1)); return true;
      case 9: io("uint(((" + si(0) + " << 8) >> 8) * ((" + si(1) + " << 8) >> 8))"); return true;
      case 11: io("(" + s[0] + " & 0xFFFFFFu) * (" + s[1] + " & 0xFFFFFFu)"); return true;
      case 13: fo("r_min_legacy(" + f(0) + ", " + f(1) + ")"); return true;
      case 14: fo("r_max_legacy(" + f(0) + ", " + f(1) + ")"); return true;
      case 15: fo("min(" + f(0) + ", " + f(1) + ")"); return true;
      case 16: fo("max(" + f(0) + ", " + f(1) + ")"); return true;
      case 17: io("uint(min(" + si(0) + ", " + si(1) + "))"); return true;
      case 18: io("uint(max(" + si(0) + ", " + si(1) + "))"); return true;
      case 19: io("min(" + s[0] + ", " + s[1] + ")"); return true;
      case 20: io("max(" + s[0] + ", " + s[1] + ")"); return true;
      case 21: io(s[0] + " >> (" + s[1] + " & 31u)"); return true;
      case 22: io(s[1] + " >> (" + s[0] + " & 31u)"); return true;
      case 23: io("uint(" + si(0) + " >> (" + s[1] + " & 31u))"); return true;
      case 24: io("uint(" + si(1) + " >> (" + s[0] + " & 31u))"); return true;
      case 25: io(s[0] + " << (" + s[1] + " & 31u)"); return true;
      case 26: io(s[1] + " << (" + s[0] + " & 31u)"); return true;
      case 27: io(s[0] + " & " + s[1]); return true;
      case 28: io(s[0] + " | " + s[1]); return true;
      case 29: io(s[0] + " ^ " + s[1]); return true;
      case 30: io("((1u << (" + s[0] + " & 31u)) - 1u) << (" + s[1] + " & 31u)"); return true;
      case 31: fo(f(0) + " * " + f(1) + " + " + F(d)); return true;
      case 32: fo(f(0) + " * " + F(U(in.literal)) + " + " + f(1)); return true;  // madmk
      case 33: fo(f(0) + " * " + f(1) + " + " + F(U(in.literal))); return true;  // madak
      case 34: io("uint(bitCount(" + s[0] + ")) + " + s[1]); return true;
      case 37: {  // v_add_i32: carry out
        Line("r_t = " + s[0] + " + " + s[1] + ";");
        carry("r_t < " + s[0]);
        io("r_t");
        return true;
      }
      case 38:
        Line("r_t = " + s[0] + " - " + s[1] + ";");
        carry(s[1] + " > " + s[0]);
        io("r_t");
        return true;
      case 39:
        Line("r_t = " + s[1] + " - " + s[0] + ";");
        carry(s[0] + " > " + s[1]);
        io("r_t");
        return true;
      case 43: fo("ldexp(" + f(0) + ", " + si(1) + ")"); return true;
      case 46: io("packUnorm2x16(vec2(" + f(0) + ", " + f(1) + "))"); return true;
      case 45: io("packSnorm2x16(vec2(" + f(0) + ", " + f(1) + "))"); return true;
      case 47: io("packHalf2x16(vec2(" + f(0) + ", " + f(1) + "))"); return true;
      case 48: io("(" + s[0] + " & 0xFFFFu) | (" + s[1] + " << 16)"); return true;
      // VOP3-only.
      case 320: fo("r_mul_legacy(" + f(0) + ", " + f(1) + ") + " + f(2)); return true;
      case 321: fo(f(0) + " * " + f(1) + " + " + f(2)); return true;
      case 322: io("uint(((" + si(0) + " << 8) >> 8) * ((" + si(1) + " << 8) >> 8) + " + si(2) + ")"); return true;
      case 323: io("(" + s[0] + " & 0xFFFFFFu) * (" + s[1] + " & 0xFFFFFFu) + " + s[2]); return true;
      case 328: io("bitfieldExtract(" + s[0] + ", int(" + s[1] + " & 31u), int(" + s[2] + " & 31u))"); return true;
      case 329: io("uint(bitfieldExtract(" + si(0) + ", int(" + s[1] + " & 31u), int(" + s[2] + " & 31u)))"); return true;
      case 330: io("(" + s[0] + " & " + s[1] + ") | (~" + s[0] + " & " + s[2] + ")"); return true;
      case 331: fo("fma(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return true;
      case 334: io("r_alignbit(" + s[0] + ", " + s[1] + ", " + s[2] + ")"); return true;
      case 337: fo("min(min(" + f(0) + ", " + f(1) + "), " + f(2) + ")"); return true;
      case 338: io("uint(min(min(" + si(0) + ", " + si(1) + "), " + si(2) + "))"); return true;
      case 339: io("min(min(" + s[0] + ", " + s[1] + "), " + s[2] + ")"); return true;
      case 340: fo("max(max(" + f(0) + ", " + f(1) + "), " + f(2) + ")"); return true;
      case 341: io("uint(max(max(" + si(0) + ", " + si(1) + "), " + si(2) + "))"); return true;
      case 342: io("max(max(" + s[0] + ", " + s[1] + "), " + s[2] + ")"); return true;
      case 343: fo("r_med3(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return true;
      case 344: io("uint(max(min(" + si(0) + ", " + si(1) + "), min(max(" + si(0) + ", " + si(1) + "), " + si(2) + ")))"); return true;
      case 345: io("max(min(" + s[0] + ", " + s[1] + "), min(max(" + s[0] + ", " + s[1] + "), " + s[2] + "))"); return true;
      case 361: case 363: io(s[0] + " * " + s[1]); return true;
      case 362: { io("r_mulhi_u(" + s[0] + ", " + s[1] + ")"); return true; }
      case 364: { io("r_mulhi_i(" + s[0] + ", " + s[1] + ")"); return true; }
      // VOP1 (+ 384).
      case 384 + 1: io(s[0]); return true;
      case 384 + 5: fo("float(" + si(0) + ")"); return true;
      case 384 + 6: fo("float(" + s[0] + ")"); return true;
      case 384 + 7: io("r_f2u(" + f(0) + ")"); return true;
      case 384 + 8: io("uint(r_f2i(" + f(0) + "))"); return true;
      case 384 + 10: io("packHalf2x16(vec2(" + f(0) + ", 0.0)) & 0xFFFFu"); return true;
      case 384 + 11: fo("unpackHalf2x16(" + s[0] + ").x"); return true;
      case 384 + 12: io("uint(r_f2i(floor(" + f(0) + " + 0.5)))"); return true;
      case 384 + 13: io("uint(r_f2i(floor(" + f(0) + ")))"); return true;
      case 384 + 17: fo("float(" + s[0] + " & 0xFFu)"); return true;
      case 384 + 18: fo("float((" + s[0] + " >> 8) & 0xFFu)"); return true;
      case 384 + 19: fo("float((" + s[0] + " >> 16) & 0xFFu)"); return true;
      case 384 + 20: fo("float(" + s[0] + " >> 24)"); return true;
      case 384 + 32: fo("r_fract(" + f(0) + ")"); return true;
      case 384 + 33: fo("trunc(" + f(0) + ")"); return true;
      case 384 + 34: fo("ceil(" + f(0) + ")"); return true;
      case 384 + 35: fo("roundEven(" + f(0) + ")"); return true;
      case 384 + 36: fo("floor(" + f(0) + ")"); return true;
      case 384 + 37: fo("exp2(" + f(0) + ")"); return true;
      case 384 + 39: fo("log2(" + f(0) + ")"); return true;
      case 384 + 42: fo("1.0 / " + f(0)); return true;
      case 384 + 44: fo("r_rsq_clamp(" + f(0) + ")"); return true;
      case 384 + 46: fo("inversesqrt(" + f(0) + ")"); return true;
      case 384 + 51: fo("sqrt(" + f(0) + ")"); return true;
      // sin and cos take revolutions.
      case 384 + 53: fo("sin(" + f(0) + " * 6.2831853071795864)"); return true;
      case 384 + 54: fo("cos(" + f(0) + " * 6.2831853071795864)"); return true;
      case 384 + 55: io("~" + s[0]); return true;
      case 384 + 56: io("bitfieldReverse(" + s[0] + ")"); return true;
      case 384 + 57: io("(" + s[0] + " == 0u ? 0xFFFFFFFFu : uint(31 - findMSB(" + s[0] + ")))"); return true;
      case 384 + 58: io("uint(findLSB(" + s[0] + "))"); return true;
    }
    (void)in;
    char b[64];
    std::snprintf(b, sizeof b, "a vector operation (%u) not translated yet", op);
    Fail(pc, b);
    return false;
  }

  // A comparison's condition (VOPC numbering) on s[0], s[1].
  bool Compare(gaddr pc, uint32_t op, const std::string s[2], bool abs[2], bool neg[2],
               std::string* cond) {
    const uint32_t c = op & 15, family = op >> 4;
    if (op == 0x88) {
      Fail(pc, "v_cmp_class not translated yet");
      return false;
    }
    if (family <= 1) {  // f32 (and cmpx)
      const std::string a = FSrc(s[0], abs[0], neg[0]), b = FSrc(s[1], abs[1], neg[1]);
      static const char* kOp[16] = {"false", "<", "==", "<=", ">", "!=", ">=", "o", "u",
                                    "nge", "nlg", "ngt", "nle", "neq", "nlt", "true"};
      switch (c) {
        case 0: *cond = "false"; break;
        case 15: *cond = "true"; break;
        case 7: *cond = "(!isnan(" + a + ") && !isnan(" + b + "))"; break;
        case 8: *cond = "(isnan(" + a + ") || isnan(" + b + "))"; break;
        case 5: *cond = "(" + a + " < " + b + " || " + a + " > " + b + ")"; break;
        case 9: *cond = "!(" + a + " >= " + b + ")"; break;
        case 10: *cond = "!(" + a + " < " + b + " || " + a + " > " + b + ")"; break;
        case 11: *cond = "!(" + a + " > " + b + ")"; break;
        case 12: *cond = "!(" + a + " <= " + b + ")"; break;
        case 13: *cond = "!(" + a + " == " + b + ")"; break;
        case 14: *cond = "!(" + a + " < " + b + ")"; break;
        default: *cond = "(" + a + " " + kOp[c] + " " + b + ")"; break;
      }
      return true;
    }
    if (family == 8 || family == 9 || family == 12 || family == 13) {  // i32, u32 (and cmpx)
      if (c > 7) {
        Fail(pc, "a comparison not translated yet");
        return false;
      }
      const bool sign = family == 8 || family == 9;
      const std::string a = sign ? "int(" + s[0] + ")" : s[0], b = sign ? "int(" + s[1] + ")" : s[1];
      static const char* kOp[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
      *cond = c == 0 ? "false" : c == 7 ? "true" : "(" + a + " " + kOp[c] + " " + b + ")";
      return true;
    }
    Fail(pc, "a comparison not translated yet");
    return false;
  }

  // ---- memory

  // A read-only or read-write binding's word at a guest byte address.
  std::string Load(int b, const std::string& addr) {
    if (rw_of[size_t(b)] >= 0) return "r_rdw(" + std::to_string(rw_of[size_t(b)]) + ", " + addr + ")";
    return "r_rd(" + std::to_string(ro_of[size_t(b)]) + ", " + addr + ")";
  }

  // A buffer instruction's byte address (low 32 bits) for element index
  // and offset VGPRs, its V# in SGPRs.
  std::string BufAddr(const Instr& in, gaddr pc, uint32_t* stride_out) {
    const uint32_t w0 = in.w0, w1 = in.w1;
    const uint32_t vaddr = w1 & 0xFF, srsrc = ((w1 >> 16) & 0x1F) * 4, soff = w1 >> 24;
    const bool offen = w0 & 0x1000, idxen = w0 & 0x2000;
    if (w0 & 0x8000) Fail(pc, "a 64-bit buffer address not translated yet");
    std::string a = S(srsrc) + " + " + U(w0 & 0xFFF) + " + " + Src(soff, in, pc);
    uint32_t next = vaddr;
    if (idxen) {
      a += " + " + V(next) + " * ((" + S(srsrc + 1) + " >> 16) & 0x3FFFu)";
      next++;
    }
    if (offen) a += " + " + V(next);
    *stride_out = 0;
    return "(" + a + ")";
  }

  // Converts the n-byte element at addr of binding b in buffer format
  // (dfmt, nfmt) to its components (floats as bits, integers as they
  // are) in r_c.
  void Fetch(int b, const std::string& addr, uint32_t dfmt, uint32_t nfmt, gaddr pc) {
    const std::string w0 = Load(b, addr), w1 = Load(b, addr + " + 4u"), w2 = Load(b, addr + " + 8u"),
                      w3 = Load(b, addr + " + 12u");
    auto conv = [&](const std::string& raw, uint32_t bits) -> std::string {
      // A component of `bits` bits in the number format.
      const std::string mx = U(bits >= 32 ? ~0u : (1u << bits) - 1);
      switch (nfmt) {
        case 0: return Bits("float(" + raw + ") / float(" + mx + ")");  // unorm
        case 1: {  // snorm
          const std::string sx = "(int(" + raw + " << " + std::to_string(32 - bits) + ") >> " + std::to_string(32 - bits) + ")";
          return Bits("max(float(" + sx + ") / float(" + U((1u << (bits - 1)) - 1) + "), -1.0)");
        }
        case 2: return Bits("float(" + raw + ")");  // uscaled
        case 3: return Bits("float(int(" + raw + " << " + std::to_string(32 - bits) + ") >> " + std::to_string(32 - bits) + ")");
        case 4: return raw;
        case 5: return "uint(int(" + raw + " << " + std::to_string(32 - bits) + ") >> " + std::to_string(32 - bits) + ")";
        case 7:
          if (bits == 32) return raw;
          if (bits == 16) return Bits("unpackHalf2x16(" + raw + ").x");
          break;
      }
      Fail(pc, "a buffer number format not translated yet");
      return "0u";
    };
    auto field = [](const std::string& w, uint32_t lo, uint32_t bits) {
      return bits == 32 ? w : "((" + w + " >> " + std::to_string(lo) + ") & " + U((1u << bits) - 1) + ")";
    };
    std::string c[4] = {"0u", "0u", "0u", Bits("1.0")};
    if (nfmt == 4 || nfmt == 5) c[3] = "1u";
    switch (dfmt) {
      case 1: c[0] = conv(field("r_w", 0, 8), 8); break;
      case 2: c[0] = conv(field("r_w", 0, 16), 16); break;
      case 3: for (int i = 0; i < 2; i++) c[i] = conv(field("r_w", 8 * i, 8), 8); break;
      case 4: c[0] = conv("r_w", 32); break;
      case 5: for (int i = 0; i < 2; i++) c[i] = conv(field("r_w", 16 * i, 16), 16); break;
      case 9:
        for (int i = 0; i < 3; i++) c[i] = conv(field("r_w", 10 * i, 10), 10);
        c[3] = conv(field("r_w", 30, 2), 2);
        break;
      case 10: for (int i = 0; i < 4; i++) c[i] = conv(field("r_w", 8 * i, 8), 8); break;
      case 11: c[0] = conv("r_w", 32); c[1] = conv("r_w1", 32); break;
      case 12:
        for (int i = 0; i < 2; i++) c[i] = conv(field("r_w", 16 * i, 16), 16);
        for (int i = 0; i < 2; i++) c[2 + i] = conv(field("r_w1", 16 * i, 16), 16);
        break;
      case 13: c[0] = conv("r_w", 32); c[1] = conv("r_w1", 32); c[2] = conv("r_w2", 32); break;
      case 14:
        c[0] = conv("r_w", 32); c[1] = conv("r_w1", 32); c[2] = conv("r_w2", 32); c[3] = conv("r_w3", 32);
        break;
      default: Fail(pc, "a buffer data format not translated yet"); return;
    }
    const uint32_t bytes = BufBytes(dfmt);
    if (bytes < 4 && ro_of[size_t(b)] < 0) return Fail(pc, "a short read of a written range not translated yet");
    Line("r_w = " + (bytes < 4 ? "r_rdu(" + std::to_string(ro_of[size_t(b)]) + ", " + addr + ")" : w0) + ";");
    if (bytes > 4) Line("r_w1 = " + w1 + ";");
    if (bytes > 8) Line("r_w2 = " + w2 + ";");
    if (bytes > 12) Line("r_w3 = " + w3 + ";");
    for (int i = 0; i < 4; i++) Line("r_c[" + std::to_string(i) + "] = " + c[i] + ";");
  }

  // Applies a V#'s destination selects to r_c and writes `count` of them
  // from vdata on.
  void Select(uint32_t vsel, uint32_t vdata, uint32_t count) {
    Line("r_d = r_c;");
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t sel = (vsel >> (3 * i)) & 7;
      const std::string v = sel == 0 ? "0u" : sel == 1 ? Bits("1.0") : sel >= 4 ? "r_d[" + std::to_string(sel - 4) + "]" : "0u";
      Line(V(vdata + i) + " = " + v + ";");
    }
  }

  void Buffer(gaddr pc, const Instr& in, const Site& site) {
    const uint32_t vdata = (in.w1 >> 8) & 0xFF;
    uint32_t stride;
    const std::string addr = BufAddr(in, pc, &stride);
    const int b = site.buffer;
    // Records past num_records read 0 and are not written (with an index
    // or a stride: index >= records; else offset >= records).
    const uint32_t* v = site.vsharp;
    if (in.fmt == Fmt::Mtbuf) {
      if (in.op >= 4) return Fail(pc, "a typed buffer store not translated yet");
      const uint32_t dfmt = (in.w0 >> 19) & 0xF, nfmt = (in.w0 >> 23) & 7;
      Fetch(b, addr, dfmt, nfmt, pc);
      Select(v[3], vdata, in.op + 1);
      return;
    }
    switch (in.op) {
      case 0: case 1: case 2: case 3: {  // buffer_load_format_*
        const uint32_t dfmt = (v[3] >> 15) & 0xF, nfmt = (v[3] >> 12) & 7;
        Fetch(b, addr, dfmt, nfmt, pc);
        Select(v[3], vdata, in.op + 1);
        return;
      }
      case 12: case 13: case 14: case 15: {  // buffer_load_dword(x2, x4, x3)
        const uint32_t n = in.op == 12 ? 1 : in.op == 13 ? 2 : in.op == 14 ? 4 : 3;
        Line("r_a = " + addr + ";");
        for (uint32_t i = 0; i < n; i++) Line(V(vdata + i) + " = " + Load(b, "r_a + " + U(4 * i)) + ";");
        return;
      }
      case 4: case 28: case 29: case 30: case 31: {  // buffer_store_format_x, _dword(s)
        if (rw_of[size_t(b)] < 0) return Fail(pc, "a store outside a compute stage not translated yet");
        const uint32_t n = in.op == 4 || in.op == 28 ? 1 : in.op == 29 ? 2 : in.op == 30 ? 4 : 3;
        if (in.op == 4 && ((v[3] >> 15) & 0xF) != 4) return Fail(pc, "a format store not translated yet");
        Line("r_a = " + addr + ";");
        for (uint32_t i = 0; i < n; i++)
          Line("r_wr(" + std::to_string(rw_of[size_t(b)]) + ", r_a + " + U(4 * i) + ", " + V(vdata + i) + ");");
        return;
      }
    }
    Fail(pc, "a buffer instruction not translated yet");
  }

  void Image(gaddr pc, const Instr& in, const Site& site) {
    const ImageRef& ref = res.r.images[size_t(site.image)];
    const std::string t = "r_tex" + std::to_string(site.image);
    const uint32_t vaddr = in.w1 & 0xFF, vdata = (in.w1 >> 8) & 0xFF, dmask = (in.w0 >> 8) & 0xF;
    const gfx::TextureKind kind = ImageKind(ref.t);
    const char type = ImageType(ref.t);
    const int dims = kind == gfx::TextureKind::Tex2D ? 2 : 3;
    const std::string cast = type == 'f' ? "floatBitsToUint" : "uvec4";
    auto write = [&](const std::string& value) {
      Line("r_v4 = " + value + ";");
      uint32_t at = vdata;
      for (int i = 0; i < 4; i++)
        if (dmask & (1u << i)) Line(V(at++) + " = r_v4[" + std::to_string(i) + "];");
    };
    if (in.op == 14) {  // image_get_resinfo: width, height, depth or layers, levels
      const std::string lod = "int(" + V(vaddr) + ")";
      const std::string size = kind == gfx::TextureKind::Tex2D || kind == gfx::TextureKind::Cube
                                   ? "uvec3(textureSize(" + t + ", " + lod + "), 1u)"
                                   : "uvec3(textureSize(" + t + ", " + lod + "))";
      write("uvec4(" + size + ", uint(textureQueryLevels(" + t + ")))");
      return;
    }
    if (in.op == 1) {  // image_load_mip
      std::string c = dims == 2 ? "ivec2(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "))"
                                : "ivec3(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "), int(" + V(vaddr + 2) + "))";
      if (kind == gfx::TextureKind::Cube) return Fail(pc, "a cube image load not translated yet");
      write(cast + "(texelFetch(" + t + ", " + c + ", int(" + V(vaddr + uint32_t(dims)) + ")))");
      return;
    }
    if (in.op == 8) {  // image_store (compute)
      std::string c = dims == 2 ? "ivec2(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "))"
                                : "ivec3(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "), int(" + V(vaddr + 2) + "))";
      std::string v[4] = {"0u", "0u", "0u", "0u"};
      uint32_t at = vdata;
      for (int i = 0; i < 4; i++)
        if (dmask & (1u << i)) v[i] = V(at++);
      const std::string img = "r_img" + std::to_string(site.image);
      const std::string val = "uvec4(" + v[0] + ", " + v[1] + ", " + v[2] + ", " + v[3] + ")";
      Line("imageStore(" + img + ", " + c + ", " + (type == 'f' ? "uintBitsToFloat(" + val + ")" : type == 'i' ? "ivec4(" + val + ")" : val) + ");");
      return;
    }
    if (in.op < 32 || in.op > 63) return Fail(pc, "an image instruction not translated yet");
    // Samples: [offset] [bias] [compare] coordinates [lod].
    const uint32_t v = in.op - 32;
    const bool offset = v >= 16, compare = (v & 8) != 0;
    const uint32_t mode = v & 7;  // 0 plain, 1 cl, 2 d, 3 d_cl, 4 l, 5 b, 6 b_cl, 7 lz
    if (mode == 1 || mode == 2 || mode == 3 || mode == 6) return Fail(pc, "a sample mode not translated yet");
    uint32_t at = vaddr;
    std::string off, bias, ref_z;
    if (offset) off = V(at++);
    if (mode == 5) bias = F(V(at++));
    if (compare) ref_z = F(V(at++));
    std::string coord;
    const int n = kind == gfx::TextureKind::Tex2D ? 2 : 3;
    for (int i = 0; i < n; i++) coord += (i ? ", " : "") + F(V(at + uint32_t(i)));
    at += uint32_t(n);
    if (kind == gfx::TextureKind::Tex2DArray) {
      coord = F(V(at - 3)) + ", " + F(V(at - 2)) + ", roundEven(" + F(V(at - 1)) + ")";
    }
    if (kind == gfx::TextureKind::Cube) return Fail(pc, "cube sampling not translated yet");
    // Offsets (6 bits, signed, per coordinate, in the VGPR) are in texels
    // of the level sampled: added to the coordinates here, as GLSL takes
    // only constant ones.
    std::string lod = mode == 4 ? F(V(at)) : "0.0";
    if (offset) {
      const std::string o = "vec2(bitfieldExtract(int(" + off + "), 0, 6), bitfieldExtract(int(" + off + "), 8, 6))";
      const std::string size = "vec2(textureSize(" + t + ", " + (mode == 4 ? "int(" + lod + ")" : std::string("0")) + ").xy)";
      if (n == 2) coord = "vec2(" + coord + ") + " + o + " / " + size;
      else coord = "vec3(vec2(" + F(V(at - 3)) + ", " + F(V(at - 2)) + ") + " + o + " / " + size + ", " + coord.substr(coord.rfind(", ") + 2) + ")";
    }
    const std::string vec = n == 2 ? "vec2(" + coord + ")" : "vec3(" + coord + ")";
    std::string call;
    if (compare) {
      const std::string sv = n == 2 ? "vec3(" + vec + ", " + ref_z + ")" : "vec4(" + vec + ", " + ref_z + ")";
      if (mode == 4) return Fail(pc, "a compare sample with a level not translated yet");
      call = mode == 7 || st.stage != Stage::Pixel ? "textureLod(" + t + ", " + sv + ", 0.0)" : "texture(" + t + ", " + sv + ")";
      write("floatBitsToUint(vec4(" + call + "))");
      return;
    }
    if (mode == 7 || mode == 4 || st.stage != Stage::Pixel) call = "textureLod(" + t + ", " + vec + ", " + lod + ")";
    else if (mode == 5) call = "texture(" + t + ", " + vec + ", " + bias + ")";
    else call = "texture(" + t + ", " + vec + ")";
    write(cast + "(" + call + ")");
  }

  void Export(gaddr pc, const Instr& in) {
    const uint32_t en = in.w0 & 0xF, target = (in.w0 >> 4) & 0x3F;
    const bool compr = in.w0 & 0x400, done = in.w0 & 0x800;
    std::string v[4];
    for (int i = 0; i < 4; i++) v[i] = V((in.w1 >> (8 * i)) & 0xFF);
    std::string c[4];
    if (compr) {
      c[0] = "unpackHalf2x16(" + v[0] + ").x";
      c[1] = "unpackHalf2x16(" + v[0] + ").y";
      c[2] = "unpackHalf2x16(" + v[1] + ").x";
      c[3] = "unpackHalf2x16(" + v[1] + ").y";
    } else {
      for (int i = 0; i < 4; i++) c[i] = F(v[i]);
    }
    const std::string vec = "vec4(" + c[0] + ", " + c[1] + ", " + c[2] + ", " + c[3] + ")";
    if (st.stage == Stage::Pixel) {
      if (target < 8) {
        const uint32_t fmt = (st.col_format >> (4 * target)) & 0xF;
        if (compr && fmt != 4) {
          if (fmt == 5) {
            Line("r_color" + std::to_string(target) + " = vec4(unpackUnorm2x16(" + v[0] + "), unpackUnorm2x16(" + v[1] + "));");
            mrts.insert(target);
          } else {
            Fail(pc, "a packed export format not translated yet");
          }
        } else {
          mrts.insert(target);
          for (int i = 0; i < 4; i++)
            if (compr || (en & (1u << i)))
              Line("r_color" + std::to_string(target) + "[" + std::to_string(i) + "] = " + c[i] + ";");
        }
      } else if (target == 8) {
        depth = true;
        Line("gl_FragDepth = " + c[0] + ";");
      } else if (target != 9) {
        Fail(pc, "a pixel export target not translated yet");
      }
      if (done) Line("if ((exec_lo & 1u) == 0u) discard;");
      return;
    }
    if (target == 12) {
      Line("gl_Position = " + vec + ";");
    } else if (target >= 13 && target <= 15) {
      // Point size, edge flags, clip distances: not used.
    } else if (target >= 32 && target < 64) {
      params.insert(target - 32);
      Line("r_param" + std::to_string(target - 32) + " = " + vec + ";");
    } else {
      Fail(pc, "an export target not translated yet");
    }
  }

  void Interp(gaddr pc, const Instr& in) {
    const uint32_t vdst = (in.w0 >> 18) & 0xFF, attr = (in.w0 >> 10) & 0x3F, chan = (in.w0 >> 8) & 3;
    const uint32_t cntl = attr < 32 ? st.ps_input_cntl[attr] : 0;
    const uint32_t loc = cntl & 0x3F;
    if (loc & 0x20) {  // the default value
      static const char* kDefault[4] = {"vec4(0, 0, 0, 0)", "vec4(0, 0, 0, 1)", "vec4(1, 1, 1, 0)", "vec4(1, 1, 1, 1)"};
      Line(V(vdst) + " = floatBitsToUint(" + std::string(kDefault[(cntl >> 8) & 3]) + "[" + std::to_string(chan) + "]);");
      return;
    }
    const bool flat = in.op == 2 || (cntl & 0x400);
    auto it = inputs.find(loc);
    if (it == inputs.end()) inputs[loc] = flat;
    if (in.op == 0) return;  // p1: p2 gives the value
    Line(V(vdst) + " = floatBitsToUint(r_in" + std::to_string(loc) + "[" + std::to_string(chan) + "]);");
    (void)pc;
  }

  void Lds(gaddr pc, const Instr& in) {
    uses_lds = true;
    const uint32_t off0 = in.w0 & 0xFF, off1 = (in.w0 >> 8) & 0xFF;
    const uint32_t addr = in.w1 & 0xFF, d0 = (in.w1 >> 8) & 0xFF, d1 = (in.w1 >> 16) & 0xFF,
                   vdst = in.w1 >> 24;
    const std::string a = V(addr);
    switch (in.op) {
      case 13: Line("r_lds[(" + a + " + " + U(off0 | off1 << 8) + ") >> 2] = " + V(d0) + ";"); return;
      case 14:
        Line("r_lds[(" + a + " + " + U(off0 * 4) + ") >> 2] = " + V(d0) + ";");
        Line("r_lds[(" + a + " + " + U(off1 * 4) + ") >> 2] = " + V(d1) + ";");
        return;
      case 54: Line(V(vdst) + " = r_lds[(" + a + " + " + U(off0 | off1 << 8) + ") >> 2];"); return;
      case 55:
        Line(V(vdst) + " = r_lds[(" + a + " + " + U(off0 * 4) + ") >> 2];");
        Line(V(vdst + 1) + " = r_lds[(" + a + " + " + U(off1 * 4) + ") >> 2];");
        return;
    }
    Fail(pc, "an LDS instruction not translated yet");
  }

  // A scalar instruction.
  void Scalar(gaddr pc, const Instr& in, gaddr next) {
    const uint32_t w = in.w0;
    auto src = [&](uint32_t c) { return Src(c, in, pc); };
    switch (in.fmt) {
      case Fmt::Sop1: {
        const uint32_t d = (w >> 16) & 0x7F, s0 = w & 0xFF;
        auto pair = [&](uint32_t c, int half) -> std::string {
          if (c == gcn::kLiteral) return half ? "0u" : U(in.literal);
          if (c >= 128) {
            uint32_t k;
            Scalars none;
            Source(none, in, c, &k);
            return half ? (int32_t(k) < 0 ? "0xFFFFFFFFu" : "0u") : U(k);
          }
          return S(c + uint32_t(half));
        };
        switch (in.op) {
          case 3: Line(Dst(d) + " = " + src(s0) + ";"); return;
          case 4:
            Line("r_t = " + pair(s0, 0) + "; r_t2 = " + pair(s0, 1) + ";");
            Line(Dst(d) + " = r_t; " + Dst(d + 1) + " = r_t2;");
            return;
          case 10: {  // s_wqm_b64: every lane of a quad with any lane set
            Line("r_t = " + pair(s0, 0) + "; r_t2 = " + pair(s0, 1) + ";");
            Line("r_t = r_wqm(r_t); r_t2 = r_wqm(r_t2);");
            Line(Dst(d) + " = r_t; " + Dst(d + 1) + " = r_t2; scc = (r_t | r_t2) != 0u ? 1u : 0u;");
            return;
          }
          case 36: case 37: case 38: case 39: {  // s_and/or/xor/andn2_saveexec_b64
            static const char* kOp[4] = {"&", "|", "^", "& ~"};
            Line("r_t = " + pair(s0, 0) + "; r_t2 = " + pair(s0, 1) + ";");
            Line(Dst(d) + " = exec_lo; " + Dst(d + 1) + " = exec_hi;");
            if (in.op == 39) {
              Line("exec_lo = r_t & ~exec_lo; exec_hi = r_t2 & ~exec_hi;");
            } else {
              Line(std::string("exec_lo = r_t ") + kOp[in.op - 36] + " exec_lo; exec_hi = r_t2 " +
                   kOp[in.op - 36] + " exec_hi;");
            }
            Line("scc = (exec_lo | exec_hi) != 0u ? 1u : 0u;");
            return;
          }
          case 7: Line(Dst(d) + " = ~" + src(s0) + "; scc = " + Dst(d) + " != 0u ? 1u : 0u;"); return;
          case 31: {  // s_getpc_b64: the code base from the uniforms
            const int region = res.code.fetch.count(pc) ? 1 : 0;
            const gaddr rel = next - res.code.base[region];
            Line(Dst(d) + " = r_misc[" + std::to_string(2 * region) + "] + " + U(uint32_t(rel)) + ";");
            Line(Dst(d + 1) + " = r_misc[" + std::to_string(2 * region + 1) + "];");
            return;
          }
          case 33: {  // s_swappc_b64: call the fetch shader; the return block in sdst
            Line(Dst(d) + " = " + U(uint32_t(block_of.at(next))) + "; " + Dst(d + 1) + " = 0xFFFFFFFFu;");
            Line("pc = " + U(uint32_t(block_of.at(res.code.base[1]))) + "; continue;");
            return;
          }
          case 32:  // s_setpc_b64: the return
            Line("pc = " + S(s0) + "; continue;");
            return;
        }
        break;
      }
      case Fmt::Sop2: {
        const uint32_t d = (w >> 16) & 0x7F;
        const std::string a = src(w & 0xFF), b = src((w >> 8) & 0xFF);
        auto pair = [&](uint32_t c, int half) -> std::string {
          if (c == gcn::kLiteral) return half ? "0u" : U(in.literal);
          if (c >= 128) {
            uint32_t k;
            Scalars none;
            Source(none, in, c, &k);
            return half ? (int32_t(k) < 0 ? "0xFFFFFFFFu" : "0u") : U(k);
          }
          return S(c + uint32_t(half));
        };
        auto set = [&](const std::string& e) {
          Line("r_t = " + e + ";");
          Line(Dst(d) + " = r_t; scc = r_t != 0u ? 1u : 0u;");
        };
        auto wide = [&](const char* op) {
          Line("r_t = " + pair(w & 0xFF, 0) + " " + op + " " + pair((w >> 8) & 0xFF, 0) + ";");
          Line("r_t2 = " + pair(w & 0xFF, 1) + " " + op + " " + pair((w >> 8) & 0xFF, 1) + ";");
          Line(Dst(d) + " = r_t; " + Dst(d + 1) + " = r_t2; scc = (r_t | r_t2) != 0u ? 1u : 0u;");
        };
        switch (in.op) {
          case 0: Line("r_t = " + a + " + " + b + "; scc = r_t < " + a + " ? 1u : 0u; " + Dst(d) + " = r_t;"); return;
          case 1: Line("r_t = " + a + " - " + b + "; scc = " + b + " > " + a + " ? 1u : 0u; " + Dst(d) + " = r_t;"); return;
          case 2: Line(Dst(d) + " = " + a + " + " + b + ";"); return;
          case 3: Line(Dst(d) + " = " + a + " - " + b + ";"); return;
          case 4: Line("r_t = " + a + " + " + b + " + scc; scc = (r_t < " + a + " || (scc != 0u && r_t == " + a + ")) ? 1u : 0u; " + Dst(d) + " = r_t;"); return;
          case 6: Line(Dst(d) + " = uint(min(int(" + a + "), int(" + b + "))); scc = int(" + a + ") < int(" + b + ") ? 1u : 0u;"); return;
          case 7: Line(Dst(d) + " = min(" + a + ", " + b + "); scc = " + a + " < " + b + " ? 1u : 0u;"); return;
          case 8: Line(Dst(d) + " = uint(max(int(" + a + "), int(" + b + "))); scc = int(" + a + ") > int(" + b + ") ? 1u : 0u;"); return;
          case 9: Line(Dst(d) + " = max(" + a + ", " + b + "); scc = " + a + " > " + b + " ? 1u : 0u;"); return;
          case 10: Line(Dst(d) + " = scc != 0u ? " + a + " : " + b + ";"); return;
          case 11:
            Line("r_t = scc != 0u ? " + pair(w & 0xFF, 0) + " : " + pair((w >> 8) & 0xFF, 0) + ";");
            Line("r_t2 = scc != 0u ? " + pair(w & 0xFF, 1) + " : " + pair((w >> 8) & 0xFF, 1) + ";");
            Line(Dst(d) + " = r_t; " + Dst(d + 1) + " = r_t2;");
            return;
          case 14: set(a + " & " + b); return;
          case 15: wide("&"); return;
          case 16: set(a + " | " + b); return;
          case 17: wide("|"); return;
          case 18: set(a + " ^ " + b); return;
          case 19: wide("^"); return;
          case 20: set(a + " & ~" + b); return;
          case 21: wide("& ~"); return;
          case 22: set(a + " | ~" + b); return;
          case 23: wide("| ~"); return;
          case 30: set(a + " << (" + b + " & 31u)"); return;
          case 32: set(a + " >> (" + b + " & 31u)"); return;
          case 34: set("uint(int(" + a + ") >> (" + b + " & 31u))"); return;
          case 36: Line(Dst(d) + " = ((1u << (" + a + " & 31u)) - 1u) << (" + b + " & 31u);"); return;
          case 38: Line(Dst(d) + " = " + a + " * " + b + ";"); return;
          case 39: set("(" + b + " >> 16 & 0x7Fu) == 0u ? 0u : bitfieldExtract(" + a + ", int(" + b + " & 31u), int(min(" + b + " >> 16 & 0x7Fu, 32u - (" + b + " & 31u))))"); return;
          case 40: set("(" + b + " >> 16 & 0x7Fu) == 0u ? 0u : uint(bitfieldExtract(int(" + a + "), int(" + b + " & 31u), int(min(" + b + " >> 16 & 0x7Fu, 32u - (" + b + " & 31u)))))"); return;
        }
        break;
      }
      case Fmt::Sopk: {
        const uint32_t d = (w >> 16) & 0x7F;
        const std::string k = U(uint32_t(int32_t(int16_t(w & 0xFFFF))));
        const std::string ku = U(w & 0xFFFF);
        static const char* kOp[6] = {"==", "!=", ">", ">=", "<", "<="};
        switch (in.op) {
          case 0: Line(Dst(d) + " = " + k + ";"); return;
          case 2: Line("if (scc != 0u) " + Dst(d) + " = " + k + ";"); return;
          case 15: Line(Dst(d) + " += " + k + ";"); return;
          case 16: Line(Dst(d) + " *= " + k + ";"); return;
        }
        if (in.op >= 3 && in.op <= 8) {
          Line("scc = int(" + S(d) + ") " + kOp[in.op - 3] + " int(" + k + ") ? 1u : 0u;");
          return;
        }
        if (in.op >= 9 && in.op <= 14) {
          Line("scc = " + S(d) + " " + kOp[in.op - 9] + " " + ku + " ? 1u : 0u;");
          return;
        }
        break;
      }
      case Fmt::Sopc: {
        const std::string a = src(w & 0xFF), b = src((w >> 8) & 0xFF);
        static const char* kOp[6] = {"==", "!=", ">", ">=", "<", "<="};
        if (in.op <= 5) {
          Line("scc = int(" + a + ") " + kOp[in.op] + " int(" + b + ") ? 1u : 0u;");
          return;
        }
        if (in.op <= 11) {
          Line("scc = " + a + " " + kOp[in.op - 6] + " " + b + " ? 1u : 0u;");
          return;
        }
        if (in.op == 12 || in.op == 13) {
          Line("scc = ((" + a + " >> (" + b + " & 31u)) & 1u) == " + (in.op == 13 ? "1u" : "0u") + " ? 1u : 0u;");
          return;
        }
        break;
      }
      default: break;
    }
    Fail(pc, "a scalar instruction not translated yet");
  }

  void Smrd(gaddr pc, const Instr& in, const Site& site) {
    const uint32_t w = in.w0, d = (w >> 15) & 0x7F, base = ((w >> 9) & 0x3F) * 2;
    if (in.op >= 29) {
      if (in.op == 30) Line(S(d) + " = 0u; " + S(d + 1) + " = 0u;");  // s_memtime
      return;
    }
    const uint32_t n = 1u << (in.op & 7);
    std::string off;
    if (w & 0x100) off = U((w & 0xFF) * 4);
    else if (in.literal_used) off = U(in.literal * 4);
    else off = S(w & 0xFF);
    const std::string addr = in.op >= 8 ? "(" + S(base) + " + " + off + ")" : "(" + S(base) + " + " + off + ")";
    Line("r_a = " + addr + ";");
    for (uint32_t i = 0; i < n; i++) Line(S(d + i) + " = " + Load(site.buffer, "r_a + " + U(4 * i)) + ";");
  }

  void Vector(gaddr pc, const Instr& in) {
    std::string s[3];
    bool abs[3] = {}, neg[3] = {};
    uint32_t omod = 0;
    bool clamp = false;
    std::string d;
    uint32_t sdst = gcn::kVccLo;
    uint32_t op;
    switch (in.fmt) {
      case Fmt::Vop2:
        op = in.op;
        s[0] = Src(in.w0 & 0x1FF, in, pc);
        s[1] = V((in.w0 >> 9) & 0xFF);
        s[2] = "vcc_lo";
        d = V((in.w0 >> 17) & 0xFF);
        break;
      case Fmt::Vop1:
        op = 384 + in.op;
        if (in.op == 0) return;  // v_nop
        if (in.op == 2) {  // v_readfirstlane_b32: this lane is the first
          Line(Dst((in.w0 >> 17) & 0xFF) + " = " + Src(in.w0 & 0x1FF, in, pc) + ";");
          return;
        }
        s[0] = Src(in.w0 & 0x1FF, in, pc);
        d = V((in.w0 >> 17) & 0xFF);
        break;
      case Fmt::Vopc: {
        s[0] = Src(in.w0 & 0x1FF, in, pc);
        s[1] = V((in.w0 >> 9) & 0xFF);
        std::string cond;
        if (!Compare(pc, in.op, s, abs, neg, &cond)) return;
        // Inactive lanes compare false.
        Line("vcc_lo = (exec_lo & 1u) != 0u && " + cond + " ? 1u : 0u; vcc_hi = 0u;");
        if ((in.op >> 4) & 1) Line("exec_lo &= vcc_lo; exec_hi = 0u;");  // cmpx
        return;
      }
      case Fmt::Vop3: {
        const uint32_t w0 = in.w0, w1 = in.w1;
        op = in.op;
        for (int i = 0; i < 3; i++) s[i] = Src((w1 >> (9 * i)) & 0x1FF, in, pc);
        const bool vop3b = (op >= 293 && op <= 298) || op == 365 || op == 366;
        for (int i = 0; i < 3; i++) {
          abs[i] = !vop3b && ((w0 >> (8 + i)) & 1);
          neg[i] = (w1 >> (29 + i)) & 1;
        }
        clamp = (w0 >> 11) & 1;
        omod = (w1 >> 27) & 3;
        if (op < 256) {
          std::string cond;
          if (!Compare(pc, op, s, abs, neg, &cond)) return;
          const uint32_t sd = w0 & 0xFF;
          Line(Dst(sd) + " = (exec_lo & 1u) != 0u && " + cond + " ? 1u : 0u; " + Dst(sd + 1) + " = 0u;");
          if ((op >> 4) & 1) Line("exec_lo = " + Dst(sd) + "; exec_hi = 0u;");
          return;
        }
        d = V(w0 & 0xFF);
        if (vop3b) sdst = (w0 >> 8) & 0x7F;
        if (op >= 256 && op < 320) op -= 256;  // VOP2's (v_cndmask's mask in src2)
        if (op == 384 + 2) {
          Line(Dst(w0 & 0xFF) + " = " + s[0] + ";");
          return;
        }
        break;
      }
      default:
        return Fail(pc, "an instruction not translated yet");
    }
    // The lane's EXEC bit guards every VGPR write.
    const std::string saved = body;
    body.clear();
    Valu(pc, in, op, d, s, abs, neg, omod, clamp, sdst);
    std::string ops = body;
    body = saved;
    Line("if ((exec_lo & 1u) != 0u) {");
    body += ops;
    Line("}");
  }

  void Instruction(gaddr pc, const Instr& in, gaddr next) {
    auto site = res.sites.find(pc);
    switch (in.fmt) {
      case Fmt::Sopp: {
        switch (in.op) {
          case 0: case 12: case 13: case 14: case 15: case 16: case 17: case 19: return;  // s_nop, s_waitcnt, ...
          case 1: End(); return;
          case 10: Line("barrier();"); return;
        }
        if (IsBranch(in)) {
          const gaddr target = next + 4 * Simm16(in);
          const std::string tb = U(uint32_t(block_of.at(target)));
          const std::string nb = in.op == 2 ? "" : U(uint32_t(block_of.at(next)));
          static const char* kCond[10] = {"", "", "true", "", "scc == 0u", "scc != 0u",
                                          "(vcc_lo & 1u) == 0u", "(vcc_lo & 1u) != 0u",
                                          "(exec_lo & 1u) == 0u", "(exec_lo & 1u) != 0u"};
          if (in.op == 2) Line("pc = " + tb + "; continue;");
          else Line(std::string("pc = (") + kCond[in.op] + ") ? " + tb + " : " + nb + "; continue;");
          return;
        }
        return Fail(pc, "a program control instruction not translated yet");
      }
      case Fmt::Sop1: case Fmt::Sop2: case Fmt::Sopk: case Fmt::Sopc:
        return Scalar(pc, in, next);
      case Fmt::Smrd:
        return Smrd(pc, in, site->second);
      case Fmt::Vop1: case Fmt::Vop2: case Fmt::Vopc: case Fmt::Vop3:
        return Vector(pc, in);
      case Fmt::Vintrp:
        return Interp(pc, in);
      case Fmt::Mubuf: case Fmt::Mtbuf:
        if (in.fmt == Fmt::Mubuf && in.op >= 112) return;
        Line("if ((exec_lo & 1u) != 0u) {");
        Buffer(pc, in, site->second);
        Line("}");
        return;
      case Fmt::Mimg:
        Line("if ((exec_lo & 1u) != 0u) {");
        Image(pc, in, site->second);
        Line("}");
        return;
      case Fmt::Exp:
        return Export(pc, in);
      case Fmt::Ds:
        Line("if ((exec_lo & 1u) != 0u) {");
        Lds(pc, in);
        Line("}");
        return;
    }
  }

  void End() {
    if (st.stage == Stage::Pixel && mrts.empty() && !depth) Line("if ((exec_lo & 1u) == 0u) discard;");
    Line("return;");
  }

  // ---- the whole stage

  bool Run(Translated* out) {
    // Bindings: in a compute stage, written ranges are read-write.
    for (const BufferRange& b : res.r.buffers) {
      const bool rw = st.stage == Stage::Compute && b.written;
      ro_of.push_back(rw ? -1 : nro);
      rw_of.push_back(rw ? nrw : -1);
      (rw ? nrw : nro)++;
    }
    for (const BufferRange& b : res.r.buffers)
      if (b.written && st.stage != Stage::Compute) {
        error = "a store outside a compute stage not translated yet";
        return false;
      }
    int n = 0;
    for (gaddr l : res.code.leaders)
      if (res.code.at.count(l)) block_of[l] = n++;
    // Blocks in address order (the fetch shader is a region of its own).
    std::vector<std::pair<gaddr, Instr>> order(res.code.at.begin(), res.code.at.end());
    const bool loop = res.code.branches;
    if (loop) {
      Line("uint pc = " + U(uint32_t(block_of.at(st.code))) + ";");
      Line("for (;;) {");
      Line("switch (pc) {");
    }
    for (size_t i = 0; i < order.size(); i++) {
      const auto& [pc, in] = order[i];
      const gaddr next = pc + 4 * uint64_t(in.dwords);
      if (loop && block_of.count(pc)) Line("case " + U(uint32_t(block_of[pc])) + ": {}");
      Instruction(pc, in, next);
      if (!error.empty()) return false;
      // A block that runs into the next one: on to it (if it is next).
      const bool ends = IsSopp(in, 1) || (in.fmt == Fmt::Sopp && in.op == 2) ||
                        (in.fmt == Fmt::Sop1 && (in.op == 32 || in.op == 33)) || IsBranch(in);
      if (loop && !ends && (i + 1 == order.size() || order[i + 1].first != next) && block_of.count(next))
        Line("pc = " + U(uint32_t(block_of[next])) + "; continue;");
      if (loop && !ends && i + 1 < order.size() && order[i + 1].first != next && !block_of.count(next))
        Line("return;");
    }
    if (loop) {
      Line("default: return;");
      Line("}");
      Line("}");
    }
    out->stage.glsl = Glsl();
    out->stage.buffers = uint32_t(nro);
    out->stage.rw_buffers = uint32_t(nrw);
    out->mrt_mask = 0;
    for (uint32_t m : mrts) out->mrt_mask |= 1u << m;
    out->writes_depth = depth;
    return error.empty();
  }

  std::string Glsl() {
    std::string g;
    char b[256];
    // The uniform block: user data, the code bases, each range's base and
    // size (Uniforms()).
    g += "layout(std140, R_UNIFORMS) uniform RUser {\n  uvec4 r_user[4];\n  uvec4 r_misc;\n  uvec4 r_misc2;\n";
    if (nro) g += "  uvec4 r_region[" + std::to_string(nro) + "];\n";
    if (nrw) g += "  uvec4 r_rwregion[" + std::to_string(nrw) + "];\n";
    g += "};\n";
    if (nro)
      g += "uint r_rd(int b, uint a) {\n  uint o = a - r_region[b].x;\n"
           "  return o < r_region[b].z ? r_mem[r_buf[b].x + (o >> 2)] : 0u;\n}\n"
           "uint r_rdu(int b, uint a) {\n  uint lo = r_rd(b, a & ~3u), hi = r_rd(b, (a & ~3u) + 4u);\n"
           "  uint sh = (a & 3u) * 8u;\n  return sh == 0u ? lo : (lo >> sh) | (hi << (32u - sh));\n}\n";
    if (nrw)
      g += "uint r_rdw(int b, uint a) {\n  uint o = a - r_rwregion[b].x;\n"
           "  return o < r_rwregion[b].z ? r_rw[r_rwbuf[b].x + (o >> 2)] : 0u;\n}\n"
           "void r_wr(int b, uint a, uint v) {\n  uint o = a - r_rwregion[b].x;\n"
           "  if (o < r_rwregion[b].z) r_rw[r_rwbuf[b].x + (o >> 2)] = v;\n}\n";
    g += "float r_mul_legacy(float a, float b) { return a == 0.0 || b == 0.0 ? 0.0 : a * b; }\n"
         "float r_min_legacy(float a, float b) { return a < b ? a : b; }\n"
         "float r_max_legacy(float a, float b) { return a >= b ? a : b; }\n"
         "float r_med3(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }\n"
         "float r_fract(float a) { return min(a - floor(a), uintBitsToFloat(0x3F7FFFFFu)); }\n"
         "float r_rsq_clamp(float a) { return clamp(inversesqrt(a), -3.402823466e38, 3.402823466e38); }\n"
         "int r_f2i(float a) { return isnan(a) ? 0 : int(clamp(a, -2147483648.0, 2147483520.0)); }\n"
         "uint r_f2u(float a) { return isnan(a) ? 0u : uint(clamp(a, 0.0, 4294967040.0)); }\n"
         "uint r_mulhi_u(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n"
         "uint r_mulhi_i(uint a, uint b) { int hi, lo; imulExtended(int(a), int(b), hi, lo); return uint(hi); }\n"
         "uint r_alignbit(uint a, uint b, uint c) { c &= 31u; return c == 0u ? b : (b >> c) | (a << (32u - c)); }\n"
         "uint r_wqm(uint m) { m |= (m >> 1) & 0x55555555u; m |= (m << 1) & 0xAAAAAAAAu;\n"
         "  m |= (m >> 2) & 0x33333333u; m |= (m << 2) & 0xCCCCCCCCu; return m; }\n";
    // Textures and images.
    for (size_t i = 0; i < res.r.images.size(); i++) {
      const ImageRef& ref = res.r.images[i];
      const gfx::TextureKind k = ImageKind(ref.t);
      const char type = ImageType(ref.t);
      const std::string prefix = type == 'f' ? "" : std::string(1, type == 'u' ? 'u' : 'i');
      if (ref.storage) {
        std::snprintf(b, sizeof b, "layout(R_IMAGE(%zu), %s) uniform writeonly %simage%s r_img%zu;\n", i,
                      type == 'u' ? "r32ui" : type == 'i' ? "r32i" : "rgba8", prefix.c_str(),
                      k == gfx::TextureKind::Tex3D ? "3D" : k == gfx::TextureKind::Tex2DArray ? "2DArray" : "2D", i);
        g += b;
        continue;
      }
      const bool shadow = ((ref.s[0] >> 12) & 7) != 0 && type == 'f' && UsesCompare(int(i));
      std::snprintf(b, sizeof b, "layout(R_TEXTURE(%zu)) uniform %ssampler%s%s r_tex%zu;\n", TexSlot(i),
                    prefix.c_str(),
                    k == gfx::TextureKind::Tex3D ? "3D" : k == gfx::TextureKind::Cube ? "Cube"
                    : k == gfx::TextureKind::Tex2DArray ? "2DArray" : "2D",
                    shadow ? "Shadow" : "", i);
      g += b;
    }
    if (st.stage == Stage::Vertex)
      for (uint32_t p : params) {
        std::snprintf(b, sizeof b, "layout(location = %u) out vec4 r_param%u;\n", p, p);
        g += b;
      }
    if (st.stage == Stage::Pixel) {
      for (auto [loc, flat] : inputs) {
        std::snprintf(b, sizeof b, "layout(location = %u) %sin vec4 r_in%u;\n", loc, flat ? "flat " : "", loc);
        g += b;
      }
      for (uint32_t m : mrts) {
        std::snprintf(b, sizeof b, "layout(location = %u) out vec4 r_color%u;\n", m, m);
        g += b;
      }
    }
    if (st.stage == Stage::Compute && uses_lds) {
      const uint32_t lds = ((st.rsrc2 >> 15) & 0x1FF) * 128;
      std::snprintf(b, sizeof b, "shared uint r_lds[%u];\n", std::max(lds, 1u));
      g += b;
    }
    g += "void main() {\n";
    g += "  uint scc = 0u, m0 = 0u, vcc_lo = 0u, vcc_hi = 0u, exec_lo = 1u, exec_hi = 0u;\n";
    g += "  uint r_t, r_t2, r_a, r_w, r_w1, r_w2, r_w3, r_unused; uvec4 r_v4; uint r_c[4], r_d[4];\n";
    for (int i = 0; i < 104; i++) {
      const std::string init = i < 16 ? "r_user[" + std::to_string(i / 4) + "][" + std::to_string(i % 4) + "]" : "0u";
      std::snprintf(b, sizeof b, "%ss%d = %s%s", i % 8 ? " " : "  uint ", i, init.c_str(), i % 8 == 7 ? ";\n" : ",");
      g += b;
    }
    for (int i = 0; i <= max_vgpr; i++) {
      std::snprintf(b, sizeof b, "  uint v%d = 0u;\n", i);
      g += b;
    }
    g += Inputs();
    for (uint32_t m : mrts) g += "  r_color" + std::to_string(m) + " = vec4(0.0);\n";
    for (uint32_t p : params) g += "  r_param" + std::to_string(p) + " = vec4(0.0);\n";
    g += body;
    g += "}\n";
    return g;
  }

  size_t TexSlot(size_t image) const {
    size_t slot = 0;
    for (size_t i = 0; i < image; i++)
      if (!res.r.images[i].storage) slot++;
    return slot;
  }
  bool UsesCompare(int image) const {
    for (auto& [pc, site] : res.sites)
      if (site.image == image) {
        const Instr& in = res.code.at.at(pc);
        if (in.fmt == Fmt::Mimg && in.op >= 32 && in.op <= 63 && ((in.op - 32) & 8)) return true;
      }
    return false;
  }

  // The stage's starting VGPRs and SGPRs beside the user data.
  std::string Inputs() {
    std::string g;
    char b[160];
    const uint32_t user = (st.rsrc2 >> 1) & 0x1F;
    auto vg = [&](uint32_t n, const char* e) {
      max_vgpr = std::max(max_vgpr, int(n));
      std::snprintf(b, sizeof b, "  v%u = %s;\n", n, e);
      g += b;
    };
    switch (st.stage) {
      case Stage::Vertex: {
        vg(0, "uint(gl_VertexIndex)");
        const uint32_t comps = (st.rsrc1 >> 24) & 3;
        if (comps >= 1) vg(1, "uint(gl_InstanceIndex) / max(r_misc2.x, 1u)");
        if (comps >= 2) vg(2, "uint(gl_InstanceIndex) / max(r_misc2.y, 1u)");
        if (comps >= 3) vg(3, "uint(gl_InstanceIndex)");
        break;
      }
      case Stage::Pixel: {
        // VGPRs in SPI_PS_INPUT_ADDR's order; barycentrics are not used
        // (the inputs interpolate), positions and the face are.
        static const uint8_t kCount[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        uint32_t at = 0;
        for (int bit = 0; bit < 16; bit++) {
          if (!(st.ps_input_addr & (1u << bit))) continue;
          switch (bit) {
            case 8: vg(at, "floatBitsToUint(gl_FragCoord.x)"); break;
            case 9: vg(at, "floatBitsToUint(gl_FragCoord.y)"); break;
            case 10: vg(at, "floatBitsToUint(gl_FragCoord.z)"); break;
            case 11: vg(at, "floatBitsToUint(gl_FragCoord.w)"); break;
            case 12: vg(at, "gl_FrontFacing ? 1u : 0u"); break;
            default: break;
          }
          at += kCount[bit];
        }
        break;
      }
      case Stage::Compute: {
        uint32_t s = user;
        for (int i = 0; i < 3; i++)
          if (st.rsrc2 & (1u << (7 + i))) {
            std::snprintf(b, sizeof b, "  s%u = gl_WorkGroupID[%d];\n", s++, i);
            g += b;
          }
        if (st.rsrc2 & (1u << 10)) {
          std::snprintf(b, sizeof b, "  s%u = 0u;\n", s++);
          g += b;
        }
        const uint32_t comps = (st.rsrc2 >> 11) & 3;
        vg(0, "gl_LocalInvocationID.x");
        if (comps >= 1) vg(1, "gl_LocalInvocationID.y");
        if (comps >= 2) vg(2, "gl_LocalInvocationID.z");
        break;
      }
    }
    return g;
  }
};

std::string Hex(const void* p, size_t n) {
  std::string s;
  s.append(static_cast<const char*>(p), n);
  return s;
}

}  // namespace

Resolved Resolve(const StageState& st) {
  Resolver rv(st);
  if (!rv.Run()) return rv.r;
  // The key: the stage, its registers that shape the code, the code's
  // words, each memory instruction's binding and the formats it reads.
  std::string& k = rv.r.key;
  const uint32_t head[] = {uint32_t(st.stage), st.rsrc1, st.rsrc2, st.ps_input_addr,
                           st.col_format, st.z_format, st.threads[0], st.threads[1], st.threads[2]};
  k = Hex(head, sizeof head);
  if (st.stage == Stage::Pixel) k += Hex(st.ps_input_cntl, sizeof st.ps_input_cntl);
  for (auto& [pc, in] : rv.code.at) {
    const uint32_t words[3] = {in.w0, in.w1, in.literal};
    k += Hex(words, sizeof words);
    auto it = rv.sites.find(pc);
    if (it == rv.sites.end()) continue;
    const Site& s = it->second;
    const uint32_t site[3] = {uint32_t(s.buffer), uint32_t(s.image), s.vsharp[3]};
    k += Hex(site, sizeof site);
    if (s.image >= 0) {
      const ImageRef& ref = rv.r.images[size_t(s.image)];
      const uint32_t f[3] = {ref.t[1] & 0x3C000000, ref.t[3] & 0xF0000000, ref.s[0] & 0x7000};
      k += Hex(f, sizeof f);
    }
  }
  for (const BufferRange& b : rv.r.buffers) k += b.written ? 'w' : 'r';
  return rv.r;
}

Translated Translate(const StageState& st, const Resolved& r) {
  Translated t;
  Resolver rv(st);
  if (!rv.Run()) {
    t.error = rv.r.error;
    return t;
  }
  (void)r;
  Translator tr(st, rv);
  if (!tr.Run(&t)) {
    t.error = tr.error;
    return t;
  }
  uint32_t textures = 0, images = 0;
  for (const ImageRef& ref : rv.r.images) {
    if (ref.storage) {
      images++;
    } else {
      if (textures < 16) t.stage.texture_kinds[textures] = ImageKind(ref.t);
      textures++;
    }
  }
  t.stage.textures = textures;
  t.stage.rw_images = images;
  t.stage.uniform_bytes = uint32_t(Uniforms(st, rv.r).size() * 4);
  t.lds_dwords = ((st.rsrc2 >> 15) & 0x1FF) * 128;
  return t;
}

std::vector<uint32_t> Uniforms(const StageState& st, const Resolved& r) {
  std::vector<uint32_t> u(24, 0);
  std::memcpy(u.data(), st.user, sizeof st.user);
  u[16] = uint32_t(st.code);
  u[17] = uint32_t(st.code >> 32);
  u[18] = uint32_t(r.fetch);
  u[19] = uint32_t(r.fetch >> 32);
  // r_misc2: instance step rates (set by the caller after).
  for (int pass = 0; pass < 2; pass++)
    for (const BufferRange& b : r.buffers) {
      const bool rw = st.stage == Stage::Compute && b.written;
      if (rw != (pass == 1)) continue;
      u.push_back(uint32_t(b.base));
      u.push_back(uint32_t(b.base >> 32));
      u.push_back(b.size);
      u.push_back(0);
    }
  return u;
}

}  // namespace recomp::orbis::gpu
