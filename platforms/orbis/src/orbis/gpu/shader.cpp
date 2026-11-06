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
#include <memory>
#include <set>
#include <unordered_map>

#include "orbis/gpu/gcn.h"
#include "orbis/gpu/renderer.h"
#include "x86/runtime.h"

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
  // The words decoded, in runs of consecutive ones: the code is still
  // what was walked while they are.
  struct Run {
    gaddr at;
    std::vector<uint32_t> words;
  };
  std::vector<Run> runs;
  // The words of `at` in order, as a number (CodeId; 0: not yet).
  mutable uint32_t id = 0;
  // The instructions Resolver::Step acts on (Steps), in address order:
  // the stage's own, and the fetch shader's (Index).
  std::vector<std::pair<gaddr, Instr>> steps, fetch_steps;
};

constexpr uint32_t kMaxInstructions = 1 << 16;

// Whether n bytes at a are guest memory (draws can point anywhere); the
// dispatch results still to land there land first.
bool Readable(gaddr a, uint64_t n) {
  if (!g_proc->vm.mem().IsCommitted(a) || !g_proc->vm.mem().IsCommitted(a + n - 1)) return false;
  BeforeRead(a, n);
  return true;
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
      if (c.runs.empty() || c.runs.back().at + 4 * c.runs.back().words.size() != pc)
        c.runs.push_back({pc, {}});
      const auto* words = reinterpret_cast<const uint32_t*>(GPtr(pc));
      c.runs.back().words.insert(c.runs.back().words.end(), words, words + in.dwords);
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

// Whether the code walked is still in memory: its words, and the bytes
// after each run that Walk found mapped (an instruction's 12).
bool Unchanged(const Code& c) {
  for (const Code::Run& run : c.runs) {
    const uint64_t n = 4 * uint64_t(run.words.size());
    if (!Readable(run.at, n + 8) || std::memcmp(GPtr(run.at), run.words.data(), n)) return false;
  }
  return true;
}

// Walks from a stage's code (fetch 0), or of its code with the fetch
// shader at fetch merged in (Resolver::Step), kept while their code is
// unchanged: draws run the same few shaders over and over, and walking
// them again for each was most of the GPU thread's time.
using Walks = std::map<std::pair<gaddr, gaddr>, std::shared_ptr<const Code>>;
Walks& WalkCache() {
  static Walks* w = new Walks;  // under the renderer's lock, until exit
  return *w;
}
std::shared_ptr<const Code> CachedWalk(gaddr entry, gaddr fetch) {
  Walks& w = WalkCache();
  auto it = w.find({entry, fetch});
  if (it == w.end()) return nullptr;
  if (Unchanged(*it->second)) return it->second;
  w.erase(it);
  return nullptr;
}
void KeepWalk(gaddr entry, gaddr fetch, std::shared_ptr<const Code> c) {
  Walks& w = WalkCache();
  if (w.size() >= 1024) w.clear();
  w[{entry, fetch}] = std::move(c);
}

// ---- the scalar evaluation (Resolve)

// Whether Resolver::Step does anything for in: the scalar and memory
// instructions, and the vector ones that write SGPRs (VCC, a
// v_readfirstlane, a VOP3 with a scalar destination); in code with buffer
// accesses at 64-bit addresses (vgprs), every one that writes VGPRs too
// (Resolver::vgpr).
bool Steps(const Instr& in, bool vgprs) {
  switch (in.fmt) {
    case Fmt::Sop1: case Fmt::Sopk: case Fmt::Sop2: case Fmt::Smrd: case Fmt::Mubuf: case Fmt::Mtbuf:
    case Fmt::Mimg: case Fmt::Vopc:
      return true;
    case Fmt::Vop1: return vgprs || in.op == 2;
    case Fmt::Vop2: return vgprs || in.op == 1 || (in.op >= 37 && in.op <= 42);
    case Fmt::Vop3:
      return vgprs || in.op < 256 || in.op == 257 || in.op == 365 || in.op == 366 || in.op == 384 + 2 ||
             (in.op >= 293 && in.op <= 298);
    case Fmt::Ds: case Fmt::Vintrp: return vgprs;
    default: return false;
  }
}

// Fills c's steps and fetch_steps from its instructions: draws resolve
// the same code over and over, and going through all of `at` for each
// was most of Resolve's time.
void Index(Code& c) {
  c.steps.clear();
  c.fetch_steps.clear();
  bool vgprs = false;
  for (auto& [pc, in] : c.at) vgprs |= (in.fmt == Fmt::Mubuf || in.fmt == Fmt::Mtbuf) && (in.w0 & 0x8000);
  for (auto& [pc, in] : c.at)
    if (Steps(in, vgprs)) (c.fetch.count(pc) ? c.fetch_steps : c.steps).emplace_back(pc, in);
}

// An SGPR not known when the stage is resolved, but one value X away from
// known ones: X is what a v_readfirstlane reads (id: its address), the
// same in every lane, which the code can use as an index.
//   kValue: c + stride * X (32-bit arithmetic);
//   kLow: a pointer's low half P_lo (p) plus kValue's c + stride * X, its
//     carry not known;
//   kHigh: the pointer's high half P_hi (p) plus that carry, so that the
//     two halves are the 64-bit address P + (uint32_t)(c + stride * X);
//   kLoaded: word c of what dynamic load id (Resolver::dyn) read.
// kCarry: a VCC (or SGPR pair) holding the carries of a VGPR's kLow sum
// (Resolver::vgpr).
struct Var {
  enum Kind : uint8_t { kNone, kValue, kLow, kHigh, kLoaded, kCarry } kind = kNone;
  uint32_t c = 0, stride = 0, p = 0;
  gaddr id = 0;
};

// SGPRs 0-103, VCC 106-107, M0 124 and EXEC 126-127, and whether each is
// known (or, if not, what is known of it).
struct Scalars {
  uint32_t v[128] = {};
  bool known[128] = {};
  Var var[128];
  // SCC, while it is a known carry or borrow (s_add_u32, s_sub_u32 and
  // their chains): what s_addc_u32 and s_subb_u32 read for 64-bit sums;
  // or the carry of a kLow sum (carry.kind kLow).
  bool scc = false, scc_known = false;
  Var carry;
  bool Get(uint32_t r, uint32_t* out) const {
    if (r >= 128 || !known[r]) return false;
    *out = v[r];
    return true;
  }
  void Set(uint32_t r, uint32_t x) {
    if (r < 128) v[r] = x, known[r] = true, var[r] = {};
  }
  void Forget(uint32_t r) {
    if (r < 128) known[r] = false, var[r] = {};
  }
  void SetVar(uint32_t r, const Var& x) {
    if (r < 128) known[r] = false, var[r] = x;
  }
  // An operand as c + stride * X (kNone if it is not one).
  Var Value(uint32_t code) const {
    if (code >= 128 || known[code]) return {};
    Var x = var[code];
    if (x.kind == Var::kLow) x.kind = Var::kValue, x.c += x.p, x.p = 0;
    return x.kind == Var::kValue ? x : Var{};
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
  // A scalar load at a run-time offset (Resolver::Dyn), or a buffer
  // access at a 64-bit address from VGPRs (addr64, Resolver::Wide):
  // checked against its range as it runs.
  bool dynamic = false, addr64 = false;
  // An image access whose T# or S# was loaded at a run-time offset: the
  // descriptors it can find there (images) and where their words are in
  // the uniforms (match, in uvec4s: T#, then S#, three for each); the
  // translation picks the one the loaded words equal.
  std::vector<int> images;
  uint32_t match = 0;
};

struct Resolver {
  const StageState& st;
  std::shared_ptr<const Code> walked;  // the code (shared with WalkCache)
  const Code& code() const { return *walked; }
  Resolved r;
  Scalars s;
  std::vector<std::pair<gaddr, Site>> sites;  // by address once Run is done
  // Scalar loads at a run-time offset: from pointer P at
  // (uint32_t)(c + stride * X) + off, n dwords. They read the range the
  // stage reads from P at known offsets (blocks: P to the end of its last
  // such read), size bytes.
  struct Dyn {
    gaddr pc, P;
    uint32_t c, stride, off, n, size = 0;
    gaddr id;
  };
  std::vector<Dyn> dyn;
  std::map<gaddr, uint32_t> blocks;
  // VGPRs the scalar run knows of: a uniform value (kValue, no stride), a
  // lane's offset added to a known pointer's low half (kLow, p: that half)
  // and the carry of that sum added to its high half (kHigh, p: that half;
  // the same id, the sum's address). Buffer accesses at such a 64-bit
  // address (ADDR64) read the range the stage reads from the pointer at
  // known offsets (blocks), as Dyn's loads do.
  Var vgpr[256];
  struct Wide {
    gaddr pc, P;
  };
  std::vector<Wide> wides;
  // Image accesses with a T# (and S#) from such loads (load -1: known).
  struct DynImage {
    gaddr pc;
    int tload = -1, sload = -1;
    uint32_t tword = 0, sword = 0;
    ImageRef known;
    int words = 8;
  };
  std::vector<DynImage> dyn_images;

  explicit Resolver(const StageState& st) : st(st) {}

  Site& NewSite(gaddr pc) {
    for (auto& [at, site] : sites)
      if (at == pc) return site;
    return sites.emplace_back(pc, Site{}).second;
  }
  const std::pair<gaddr, Site>* SiteAt(gaddr pc) const {
    auto it = std::lower_bound(sites.begin(), sites.end(), pc,
                               [](const auto& e, gaddr a) { return e.first < a; });
    return it != sites.end() && it->first == pc ? &*it : nullptr;
  }

  int Buffer(gaddr base, uint32_t size, bool written, const uint32_t* vsharp = nullptr) {
    for (size_t i = 0; i < r.buffers.size(); i++)
      if (r.buffers[i].base == base && r.buffers[i].size == size) {
        r.buffers[i].written |= written;
        return int(i);
      }
    r.buffers.push_back({base, size, written});
    if (vsharp) std::memcpy(r.buffers.back().vsharp, vsharp, sizeof r.buffers.back().vsharp);
    return int(r.buffers.size() - 1);
  }

  bool Fail(gaddr pc, const char* what) {
    char b[160];
    std::snprintf(b, sizeof b, "%s at %llX (%s)", what, (unsigned long long)pc,
                  code().at.count(pc) ? gcn::Disassemble(code().at.at(pc)).c_str() : "?");
    r.error = b;
    return false;
  }

  bool Vsharp(uint32_t first, uint32_t v[4]) {
    for (int i = 0; i < 4; i++)
      if (!s.Get(first + uint32_t(i), &v[i])) return false;
    return true;
  }

  // A V#'s range: stride x records, or records bytes without a stride.
  // Addresses are 40 bits: the bits above (set in the ring V#s) are not
  // part of the address.
  static void Range(const uint32_t v[4], gaddr* base, uint32_t* size) {
    *base = v[0] | uint64_t(v[1] & 0xFF) << 32;
    const uint32_t stride = (v[1] >> 16) & 0x3FFF;
    const uint64_t bytes = stride ? uint64_t(stride) * v[2] : v[2];
    *size = uint32_t(std::min<uint64_t>(bytes, 256u << 20));
  }

  // Runs the scalar instruction at pc.
  bool Step(gaddr pc, const Instr& in) {
    const uint32_t w = in.w0;
    // Scalar ALU instructions other than the carries below may write SCC.
    const bool carry_known = s.scc_known, carry_in = s.scc;
    const Var carry_var = s.carry;
    if (in.fmt == Fmt::Sop1 || in.fmt == Fmt::Sop2 || in.fmt == Fmt::Sopk || in.fmt == Fmt::Sopc)
      s.scc_known = false, s.carry = {};
    switch (in.fmt) {
      case Fmt::Sop1: {
        const uint32_t d = (w >> 16) & 0x7F;
        uint32_t a, b;
        switch (in.op) {
          case 3:  // s_mov_b32
            if (Source(s, in, w & 0xFF, &a)) s.Set(d, a);
            else if ((w & 0xFF) < 128) s.SetVar(d, s.var[w & 0xFF]);
            else s.Forget(d);
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
            } else if (c < 127) {
              const Var lo = s.var[c], hi = s.var[c + 1];
              s.SetVar(d, lo);
              s.SetVar(d + 1, hi);
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
            if (code().base[1] && code().base[1] != target)
              return Fail(pc, "calls to more than one fetch shader");
            r.fetch = target;
            s.Set(d, uint32_t(pc + 4));
            s.Set(d + 1, uint32_t((pc + 4) >> 32));
            if (auto c = CachedWalk(st.code, target)) {
              walked = std::move(c);
            } else {
              Code fc;
              if (!Walk(target, fc, &r.error)) return false;
              if (fc.branches && !std::any_of(fc.at.begin(), fc.at.end(), [](auto& e) {
                    return e.second.fmt == Fmt::Sop1 && e.second.op == 32;
                  }))
                return Fail(pc, "a fetch shader without a return");
              auto merged = std::make_shared<Code>(code());
              for (auto& [a, i] : fc.at) {
                if (IsBranch(i)) return Fail(a, "a branch in a fetch shader");
                merged->at[a] = i;
                merged->fetch.insert(a);
              }
              merged->base[1] = target;
              merged->leaders.insert(target);
              merged->runs.insert(merged->runs.end(), fc.runs.begin(), fc.runs.end());
              Index(*merged);
              KeepWalk(st.code, target, merged);
              walked = std::move(merged);
            }
            for (auto& [a, i] : code().fetch_steps)
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
        else if ((in.op == 15 || in.op == 16) && s.Value(d).kind) {
          Var x = s.Value(d);
          const uint32_t k = uint32_t(int32_t(int16_t(w & 0xFFFF)));
          if (in.op == 15) x.c += k;
          else x.c *= k, x.stride *= k;
          s.SetVar(d, x);
        }
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
        if ((!k || (in.op == 4 && carry_var.kind == Var::kLow)) && !wide && SumOf(in, d, carry_var))
          return true;
        if (!k || wide) {
          s.Forget(d);
          if (wide) s.Forget(d + 1);
          return true;
        }
        switch (in.op) {
          case 0:  // s_add_u32: SCC the carry
            s.Set(d, a + b);
            s.scc = a + b < a, s.scc_known = true;
            break;
          case 1:  // s_sub_u32: SCC the borrow
            s.Set(d, a - b);
            s.scc = b > a, s.scc_known = true;
            break;
          case 2: s.Set(d, a + b); break;
          case 3: s.Set(d, a - b); break;
          case 4: case 5: {  // s_addc_u32, s_subb_u32: with SCC's carry or borrow
            if (!carry_known) {
              s.Forget(d);
              break;
            }
            const uint64_t r = in.op == 4 ? uint64_t(a) + b + carry_in : uint64_t(a) - b - carry_in;
            s.Set(d, uint32_t(r));
            s.scc = (r >> 32) != 0, s.scc_known = true;
            break;
          }
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
        Site& site = NewSite(pc);
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
          if (!s.Get(base, &lo) || !s.Get(base + 1, &hi)) {
            const Var l = s.var[base], h = s.var[base + 1];
            if (l.kind != Var::kLow || h.kind != Var::kHigh || h.id != l.id)
              return Fail(pc, "a scalar load from an unknown address");
            if (!RunsAsCompute(st.stage) || st.stage != Stage::Compute)
              return Fail(pc, "a scalar load at a run-time offset outside a compute stage not translated yet");
            dyn.push_back({pc, l.p | uint64_t(h.p & 0xFFFF) << 32, l.c, l.stride, off, n, 0, l.id});
            site.dynamic = true;
            for (uint32_t i = 0; i < n; i++) s.SetVar(d + i, {Var::kLoaded, i, 0, 0, gaddr(dyn.size() - 1)});
            return true;
          }
          const gaddr ptr = lo | uint64_t(hi & 0xFFFF) << 32;
          blocks[ptr] = std::max(blocks[ptr], off + 4 * n);
          a = ptr + off;
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
        if (!store)
          for (uint32_t i = 0; i < 4; i++) vgpr[((in.w1 >> 8) + i) & 0xFF] = {};
        if (in.w0 & 0x8000) {  // ADDR64: the address in VGPRs, the V# only its base (0) and no range
          const uint32_t va = in.w1 & 0xFF;
          const Var l = vgpr[va], h = vgpr[(va + 1) & 0xFF];
          if (l.kind != Var::kLow || h.kind != Var::kHigh || h.id != l.id)
            return Fail(pc, "a buffer access at a 64-bit address not known to the scalar run");
          if (v[0] || (v[1] & 0xFF) || v[2] != ~0u || (v[1] >> 31) || ((v[3] >> 23) & 1) || (in.w0 & 0x3000))
            return Fail(pc, "a buffer access at a 64-bit address with a V# range not translated yet");
          if (store) return Fail(pc, "a buffer store at a 64-bit address not translated yet");
          if (st.stage != Stage::Compute)
            return Fail(pc, "a buffer access at a 64-bit address outside a compute stage not translated yet");
          wides.push_back({pc, l.p | uint64_t(h.p & 0xFFFF) << 32});
          Site& site = NewSite(pc);
          site.dynamic = site.addr64 = true;
          std::memcpy(site.vsharp, v, sizeof v);
          return true;
        }
        gaddr base;
        uint32_t size;
        Range(v, &base, &size);
        Site& site = NewSite(pc);
        site.buffer = Buffer(base, size, store, v);
        std::memcpy(site.vsharp, v, sizeof v);
        return true;
      }
      case Fmt::Mimg: {
        if (!(in.op >= 8 && in.op <= 11))
          for (uint32_t i = 0; i < 5; i++) vgpr[((in.w1 >> 8) + i) & 0xFF] = {};
        ImageRef ref;
        const uint32_t t = ((in.w1 >> 16) & 0x1F) * 4, smp = ((in.w1 >> 21) & 0x1F) * 4;
        const int words = (in.w0 & 0x8000) ? 4 : 8;
        ref.storage = in.op >= 8 && in.op <= 11;
        DynImage di{pc};
        di.words = words;
        if (!Loaded(t, words, &di.tload, &di.tword)) {
          for (int i = 0; i < words; i++)
            if (!s.Get(t + uint32_t(i), &ref.t[i])) return Fail(pc, "an image access with an unknown T#");
        }
        if (in.op >= 32 && !Loaded(smp, 4, &di.sload, &di.sword)) {  // samples, gathers, levels: with a sampler
          for (int i = 0; i < 4; i++)
            if (!s.Get(smp + uint32_t(i), &ref.s[i])) return Fail(pc, "a sample with an unknown S#");
        }
        if (di.tload >= 0 || di.sload >= 0) {
          if (ref.storage) return Fail(pc, "a store to an image loaded at a run-time offset not translated yet");
          if (di.tload >= 0 && di.sload >= 0 && dyn[size_t(di.tload)].id != dyn[size_t(di.sload)].id)
            return Fail(pc, "a T# and an S# loaded at unrelated run-time offsets");
          di.known = ref;
          NewSite(pc);
          dyn_images.push_back(di);
          return true;
        }
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
        NewSite(pc).image = at;
        return true;
      }
      case Fmt::Vop1:
        if (in.op == 2) {
          s.SetVar((w >> 17) & 0xFF, {Var::kValue, 0, 1, 0, pc});  // v_readfirstlane_b32
          return true;
        }
        return Valu(pc, in, 384 + in.op, (w >> 17) & 0xFF, w & 0x1FF, 0, 0, gcn::kVccLo);
      case Fmt::Vopc:
        s.Forget(gcn::kVccLo), s.Forget(gcn::kVccHi);
        return true;
      case Fmt::Vop2:
        if (in.op == 1) {  // v_readlane_b32
          s.Forget((w >> 17) & 0xFF);
          return true;
        }
        return Valu(pc, in, in.op, (w >> 17) & 0xFF, w & 0x1FF, gcn::kVgpr + ((w >> 9) & 0xFF), gcn::kVccLo,
                    gcn::kVccLo);
      case Fmt::Vop3: {
        const uint32_t d = w & 0xFF;
        if (in.op < 256 || in.op == 257 || in.op == 365 || in.op == 366) {
          s.Forget(d), s.Forget(d + 1);
          return true;
        }
        if (in.op == 384 + 2) {
          s.SetVar(d, {Var::kValue, 0, 1, 0, pc});
          return true;
        }
        const uint32_t sd = in.op >= 293 && in.op <= 298 ? (w >> 8) & 0x7F : gcn::kVccLo;
        const uint32_t op = in.op >= 256 && in.op < 320 ? in.op - 256 : in.op;
        return Valu(pc, in, op, d, in.w1 & 0x1FF, (in.w1 >> 9) & 0x1FF, (in.w1 >> 18) & 0x1FF, sd);
      }
      case Fmt::Ds: {  // reads: their VGPRs unknown
        const uint32_t vdst = in.w1 >> 24;
        if (in.op != 13 && in.op != 14 && in.op != 10)
          for (uint32_t i = 0; i < 4; i++) vgpr[(vdst + i) & 0xFF] = {};
        return true;
      }
      case Fmt::Vintrp:
        vgpr[(w >> 18) & 0xFF] = {};
        return true;
      default:
        return true;
    }
  }

  // A VALU instruction's VGPR (and carry) facts (vgpr): op in VOP2/VOP3
  // numbering (VOP1 + 384), sources s0-s2 (9-bit codes), destination d,
  // carry out sd.
  bool Valu(gaddr pc, const Instr& in, uint32_t op, uint32_t d, uint32_t s0, uint32_t s1, uint32_t s2,
            uint32_t sd) {
    auto uniform = [&](uint32_t code, uint32_t* k) {
      if (code < gcn::kVgpr) return Source(s, in, code, k);
      const Var& v = vgpr[code - gcn::kVgpr];
      if (v.kind != Var::kValue || v.stride) return false;
      *k = v.c;
      return true;
    };
    uint32_t a, b;
    const bool ka = uniform(s0, &a), kb = uniform(s1, &b);
    Var out;
    switch (op) {
      case 384 + 1:  // v_mov_b32
        if (ka) out = {Var::kValue, a, 0, 0, pc};
        break;
      case 384 + 66: case 384 + 68:  // v_movreld: a VGPR M0 picks
        for (Var& v : vgpr) v = {};
        return true;
      case 37:  // v_add_i32: a pointer's low half plus a lane's offset
        if (ka && kb) out = {Var::kValue, a + b, 0, 0, pc};
        else if (ka || kb) out = {Var::kLow, 0, 0, ka ? a : b, pc};
        s.Forget(sd + 1);
        if (out.kind == Var::kLow) s.SetVar(sd, {Var::kCarry, 0, 0, 0, pc});
        else s.Forget(sd);
        break;
      case 40: {  // v_addc_u32: its high half plus that sum's carry
        const Var carry = s2 < 128 ? s.var[s2] : Var{};
        if (carry.kind == Var::kCarry && ka && kb && (a == 0 || b == 0))
          out = {Var::kHigh, 0, 0, a | b, carry.id};
        s.Forget(sd), s.Forget(sd + 1);
        break;
      }
      case 38: case 39: case 41: case 42:
        s.Forget(sd), s.Forget(sd + 1);
        break;
    }
    vgpr[d & 0xFF] = out;
    return true;
  }

  bool Run() {
    walked = CachedWalk(st.code, 0);
    if (!walked) {
      auto c = std::make_shared<Code>();
      c->base[0] = st.code;
      walked = c;
      if (!Walk(st.code, *c, &r.error)) return false;
      Index(*c);
      KeepWalk(st.code, 0, c);
    }
    const std::shared_ptr<const Code> own = walked;  // Step may swap in the fetch shader's
    const uint32_t user = (st.rsrc2 >> 1) & 0x1F;
    for (uint32_t i = 0; i < user && i < 16; i++) s.Set(i, st.user[i]);
    s.Set(gcn::kExecLo, 1);
    s.Set(gcn::kExecHi, 0);
    // Program order: the stage's code by address, a call running the
    // fetch shader where it is.
    for (auto& [pc, in] : own->steps)
      if (!Step(pc, in)) return false;
    if (!Dynamic()) return false;
    if (st.stage == Stage::Compute && CrossesLanes()) r.wave = r.faults = true;
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
  }

  // Whether the code reads other lanes than its own: v_readlane,
  // v_writelane, v_readfirstlane, ds_swizzle_b32 across quads. A compute
  // stage that does runs in waves (Resolved::wave).
  bool CrossesLanes() const {
    for (auto& [pc, in] : code().at) {
      if ((in.fmt == Fmt::Vop2 && (in.op == 1 || in.op == 2)) || (in.fmt == Fmt::Vop1 && in.op == 2) ||
          (in.fmt == Fmt::Vop3 && (in.op == 257 || in.op == 258 || in.op == 384 + 2)))
        return true;
      if (in.fmt != Fmt::Ds || in.op != 53 || (in.w0 & 0x8000)) continue;
      const uint32_t o = in.w0 & 0xFFFF;
      if (((o & 0x1F) & 0x1C) != 0x1C || ((o >> 5) & 0x1C) || ((o >> 10) & 0x1C)) return true;
    }
    return false;
  }

  // Whether n SGPRs from first are words in a row of one dynamic load.
  bool Loaded(uint32_t first, int n, int* load, uint32_t* word) const {
    const Var& v0 = s.var[first];
    if (first + uint32_t(n) > 128 || v0.kind != Var::kLoaded) return false;
    for (int i = 1; i < n; i++) {
      const Var& v = s.var[first + uint32_t(i)];
      if (v.kind != Var::kLoaded || v.id != v0.id || v.c != v0.c + uint32_t(i)) return false;
    }
    *load = int(v0.id);
    *word = v0.c;
    return true;
  }

  // SOP2 sums and products with an operand c + stride * X (Var). Under
  // s_add_u32 one with a known operand starts a pointer (kLow: that
  // operand is P_lo), whose carry s_addc_u32 of known operands adds to
  // P_hi (kHigh).
  bool SumOf(const Instr& in, uint32_t d, const Var& carry) {
    const uint32_t w = in.w0, ca = w & 0xFF, cb = (w >> 8) & 0xFF;
    uint32_t a, b;
    const bool ka = Source(s, in, ca, &a), kb = Source(s, in, cb, &b);
    const Var va = s.Value(ca), vb = s.Value(cb);
    Var x;
    switch (in.op) {
      case 0: case 2:  // s_add_u32, s_add_i32
        if (ka && vb.kind) x = vb;
        else if (kb && va.kind) x = va, a = b;
        else return false;
        if (in.op == 0) {
          x.kind = Var::kLow;
          x.p = a;
          s.SetVar(d, x);
          s.carry = x;
          return true;
        }
        x.c += a;
        break;
      case 1: case 3:  // s_sub_u32, s_sub_i32
        if (ka && vb.kind) x = vb, x.c = a - vb.c, x.stride = 0u - vb.stride;
        else if (kb && va.kind) x = va, x.c -= b;
        else return false;
        break;
      case 4:  // s_addc_u32: a pointer's high half
        if (!ka || !kb || carry.kind != Var::kLow) return false;
        x = {Var::kHigh, carry.c, carry.stride, a + b, carry.id};
        break;
      case 38:  // s_mul_i32
        if (ka && vb.kind) x = vb, x.c *= a, x.stride *= a;
        else if (kb && va.kind) x = va, x.c *= b, x.stride *= b;
        else return false;
        break;
      case 30:  // s_lshl_b32
        if (!kb || !va.kind) return false;
        x = va, x.c <<= (b & 31), x.stride <<= (b & 31);
        break;
      default:
        return false;
    }
    s.SetVar(d, x);
    return true;
  }

  // The values of X for which load L's dwords lie in its range, its
  // offset c + stride * X taken without wrapping.
  bool Xs(const Dyn& L, std::vector<int64_t>* out) const {
    out->clear();
    const int64_t c = int32_t(L.c), st = int32_t(L.stride), lim = int64_t(L.size) - L.off - 4 * L.n;
    if (lim < 0) return true;
    if (!st) {
      if (c >= 0 && c <= lim) out->push_back(0);
      return true;
    }
    // c + st * X in [0, lim].
    auto floordiv = [](int64_t p, int64_t q) { return p / q - ((p % q) != 0 && ((p < 0) != (q < 0))); };
    auto ceildiv = [&](int64_t p, int64_t q) { return -floordiv(-p, q); };
    const int64_t lo = st > 0 ? ceildiv(-c, st) : ceildiv(c - lim, -st);
    const int64_t hi = st > 0 ? floordiv(lim - c, st) : floordiv(c, -st);
    if (hi - lo >= 65536) return false;
    for (int64_t x = lo; x <= hi; x++) out->push_back(x);
    return true;
  }

  // The dynamic loads' ranges, and the descriptors their image accesses
  // can find (Site::images, Resolved::match).
  bool Dynamic() {
    for (Dyn& L : dyn) {
      auto it = blocks.find(L.P);
      if (it == blocks.end())
        return Fail(L.pc, "a scalar load at a run-time offset from a pointer the stage reads nowhere else");
      L.size = it->second;
      if (!Readable(L.P, L.size)) return Fail(L.pc, "a scalar load at a run-time offset outside memory");
      NewSite(L.pc).buffer = Buffer(L.P, L.size, false);
      r.faults = true;
    }
    for (const Wide& x : wides) {
      auto it = blocks.find(x.P);
      if (it == blocks.end())
        return Fail(x.pc, "a buffer access at a 64-bit address from a pointer the stage reads nowhere else");
      if (!Readable(x.P, it->second)) return Fail(x.pc, "a buffer access at a 64-bit address outside memory");
      NewSite(x.pc).buffer = Buffer(x.P, it->second, false);
      r.faults = true;
    }
    for (const DynImage& di : dyn_images) {
      const Dyn& first = dyn[size_t(di.tload >= 0 ? di.tload : di.sload)];
      std::vector<int64_t> xs;
      if (!Xs(first, &xs)) return Fail(di.pc, "too many run-time offsets of a descriptor");
      Site& site = NewSite(di.pc);
      for (int64_t x : xs) {
        ImageRef ref = di.known;
        bool ok = true;
        auto read = [&](int load, uint32_t word, uint32_t* out, int n) {
          const Dyn& L = dyn[size_t(load)];
          const int64_t o = int64_t(int32_t(L.c)) + int64_t(int32_t(L.stride)) * x;
          if (o < 0 || o + L.off + 4 * L.n > L.size) return void(ok = false);
          for (int i = 0; i < n; i++) out[i] = Rd32(L.P + uint64_t(o) + L.off + 4 * (word + uint32_t(i)));
        };
        if (di.tload >= 0) read(di.tload, di.tword, ref.t, di.words);
        if (di.sload >= 0) read(di.sload, di.sword, ref.s, 4);
        if (!ok || (ref.t[3] >> 28) < 8) continue;  // no image there
        int at = -1;
        for (size_t i = 0; i < r.images.size(); i++)
          if (!std::memcmp(r.images[i].t, ref.t, sizeof ref.t) &&
              !std::memcmp(r.images[i].s, ref.s, sizeof ref.s) && !r.images[i].storage)
            at = int(i);
        if (at < 0) {
          r.images.push_back(ref);
          at = int(r.images.size() - 1);
        }
        if (std::find(site.images.begin(), site.images.end(), at) == site.images.end())
          site.images.push_back(at);
      }
      site.match = uint32_t(r.match.size() / 4);
      for (int i : site.images) {
        const ImageRef& ref = r.images[size_t(i)];
        r.match.insert(r.match.end(), ref.t, ref.t + 8);
        r.match.insert(r.match.end(), ref.s, ref.s + 4);
      }
      r.faults = true;
    }
    size_t textures = 0;
    for (const ImageRef& ref : r.images) textures += !ref.storage;
    if (textures > 16) {
      r.error = "more than 16 textures (with those found at run-time offsets)";
      return false;
    }
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
  bool depth = false, uses_lds = false, uses_quad = false, uses_shuffle = false, uses_ballot = false;
  // In waves (Resolved::wave): the half of the wave (lanes 32 * half on)
  // whose VGPRs the instruction being emitted works on.
  bool wave = false;
  int half = 0;
  std::map<gaddr, int> block_of;
  std::vector<int> ro_of, rw_of;       // binding -> RO / RW index (-1: not that kind)
  int nro = 0, nrw = 0;
  int ids = -1, emits = -1;            // Export: the vertex indices; Geometry: its emits
  int fault = -1;                      // the fault word's range (Resolved::faults)

  Translator(const StageState& st, const Resolver& res) : st(st), res(res), wave(res.r.wave) {}

  void Fail(gaddr pc, const char* what) {
    if (!error.empty()) return;
    char b[200];
    auto it = res.code().at.find(pc);
    std::snprintf(b, sizeof b, "%s at %llX (%s)", what, (unsigned long long)pc,
                  it == res.code().at.end() ? "?" : gcn::Disassemble(it->second).c_str());
    error = b;
  }

  // ---- operands (every value is a uint)

  std::string V(uint32_t n) {
    max_vgpr = std::max(max_vgpr, int(n));
    return "v" + std::to_string(n) + (wave ? "_" + std::to_string(half) : "");
  }
  // Whether the lane's EXEC bit is set; the lane's index in its wave.
  std::string Active() const {
    return wave ? std::string("((exec_") + (half ? "hi" : "lo") + " >> r_sl) & 1u) != 0u" : "(exec_lo & 1u) != 0u";
  }
  std::string Lane() const { return wave ? (half ? "(r_sl + 32u)" : "r_sl") : "r_lane"; }
  // Whether a mask (an SGPR pair: VCC, EXEC) is empty: the wave's, or in a
  // lane of its own, that lane's bit.
  static std::string Empty(const char* mask, bool wave) {
    const std::string m = mask;
    return wave ? "(" + m + "_lo | " + m + "_hi) == 0u" : "(" + m + "_lo & 1u) == 0u";
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
      case gcn::kVccz: return "(" + Empty("vcc", wave) + " ? 1u : 0u)";
      case gcn::kExecz: return "(" + Empty("exec", wave) + " ? 1u : 0u)";
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
    if (omod == 1) f = "((" + f + ") * 2.0)";
    else if (omod == 2) f = "((" + f + ") * 4.0)";
    else if (omod == 3) f = "((" + f + ") * 0.5)";
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
    // Carry-out ops write sdst (VCC for VOP2); in waves, the lane's ballot
    // (Vector).
    auto carry = [&](const std::string& e) {
      if (wave) Line("r_b" + std::to_string(half) + " = " + e + ";");
      else Line(Dst(sdst) + " = (" + e + ") ? 1u : 0u; " + Dst(sdst + 1) + " = 0u;");
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
      // v_addc_u32, v_subb_u32, v_subbrev_u32: the lane's bit of s2 (VCC
      // in VOP2) carries in, read before the carry out is written (both may
      // be VCC).
      case 40:
        Line("r_t2 = " + s[2] + " & 1u; r_t = " + s[0] + " + " + s[1] + " + r_t2;");
        carry("r_t < " + s[0] + " || (r_t2 != 0u && r_t == " + s[0] + ")");
        io("r_t");
        return true;
      case 41:
        Line("r_t2 = " + s[2] + " & 1u; r_t = " + s[0] + " - " + s[1] + " - r_t2;");
        carry(s[1] + " > " + s[0] + " || (r_t2 != 0u && " + s[1] + " == " + s[0] + ")");
        io("r_t");
        return true;
      case 42:
        Line("r_t2 = " + s[2] + " & 1u; r_t = " + s[1] + " - " + s[0] + " - r_t2;");
        carry(s[0] + " > " + s[1] + " || (r_t2 != 0u && " + s[0] + " == " + s[1] + ")");
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
      // The cube map instructions: the face of direction (s0, s1, s2) its
      // major axis selects (z, then y, then x on ties), the face's
      // coordinates and twice the major axis component.
      case 324: fo("r_cubeid(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return true;
      case 325: fo("r_cubesc(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return true;
      case 326: fo("r_cubetc(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return true;
      case 327: fo("r_cubema(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return true;
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
      // v_sad_u32: |S0 - S1| of unsigned values, plus S2 (as LLVM selects it
      // for umax - umin + c).
      case 349: io("(max(" + s[0] + ", " + s[1] + ") - min(" + s[0] + ", " + s[1] + ")) + " + s[2]); return true;
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
      // v_rcp_f32; v_rcp_iflag_f32 (integer division's) differs only in a
      // flag nothing here reads.
      case 384 + 42: case 384 + 43: fo("1.0 / " + f(0)); return true;
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
  // and offset VGPRs, its V# in SGPRs (v: as resolved). ADD_TID_ENABLE
  // adds the lane to the index; SWIZZLE_ENABLE interleaves the lanes'
  // elements (of ELEMENT_SIZE bytes, INDEX_STRIDE lanes in a row): index i
  // and offset o at ((i / is) * stride + (o / es) * es) * is + (i % is) * es
  // + o % es, the stride in bytes per run of is indices' elements.
  std::string BufAddr(const Instr& in, gaddr pc, const uint32_t v[4], uint32_t* stride_out) {
    const uint32_t w0 = in.w0, w1 = in.w1;
    const uint32_t vaddr = w1 & 0xFF, srsrc = ((w1 >> 16) & 0x1F) * 4, soff = w1 >> 24;
    const bool offen = w0 & 0x1000, idxen = w0 & 0x2000;
    if (w0 & 0x8000) Fail(pc, "a 64-bit buffer address not translated yet");
    const std::string stride = "((" + S(srsrc + 1) + " >> 16) & 0x3FFFu)";
    std::string a = S(srsrc) + " + " + U(w0 & 0xFFF) + " + " + Src(soff, in, pc);
    *stride_out = 0;
    const bool swizzle = v[1] >> 31, tid = (v[3] >> 23) & 1;
    if (swizzle || tid) {
      std::string index = tid ? "r_lane" : "0u", offset = U(w0 & 0xFFF);
      uint32_t next = vaddr;
      if (idxen) index += " + " + V(next++);
      if (offen) offset += " + " + V(next);
      a = S(srsrc) + " + " + Src(soff, in, pc);
      if (!swizzle) return "(" + a + " + (" + index + ") * " + stride + " + " + offset + ")";
      const std::string es = U(2u << ((v[3] >> 19) & 3)), is = U(8u << ((v[3] >> 21) & 3));
      Line("r_i = " + index + "; r_o = " + offset + ";");
      return "(" + a + " + ((r_i / " + is + ") * " + stride + " + (r_o / " + es + ") * " + es + ") * " + is +
             " + (r_i % " + is + ") * " + es + " + r_o % " + es + ")";
    }
    uint32_t next = vaddr;
    if (idxen) {
      a += " + " + V(next) + " * " + stride;
      next++;
    }
    if (offen) a += " + " + V(next);
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
    if (site.addr64) return Wide(pc, in, site);
    uint32_t stride;
    const std::string addr = BufAddr(in, pc, site.vsharp, &stride);
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
      case 4: case 5: case 6: case 7:  // buffer_store_format_x(y, yz, yzw)
      case 28: case 29: case 30: case 31: {  // buffer_store_dword(x2, x4, x3)
        if (rw_of[size_t(b)] < 0) return Fail(pc, "a store outside a compute stage not translated yet");
        const uint32_t n = in.op <= 7 ? in.op - 3 : in.op == 28 ? 1 : in.op == 29 ? 2 : in.op == 30 ? 4 : 3;
        if (in.op == 4 && ((v[3] >> 15) & 0xF) != 4) return Fail(pc, "a format store not translated yet");
        if (in.op >= 5 && in.op <= 7) {
          // Formats of 32-bit components (32_32, 32_32_32, 32_32_32_32) as
          // integers or floats: the registers' bits, one dword each, the
          // V#'s selects in order. Other formats convert: not translated.
          const uint32_t dfmt = (v[3] >> 15) & 0xF, nfmt = (v[3] >> 12) & 7;
          const uint32_t comps = dfmt == 11 ? 2 : dfmt == 13 ? 3 : dfmt == 14 ? 4 : 0;
          if (!comps || (nfmt != 4 && nfmt != 5 && nfmt != 7) || comps != n ||
              (v[3] & 0xFFF & ((1u << (3 * n)) - 1)) != ((4u | 5u << 3 | 6u << 6 | 7u << 9) & ((1u << (3 * n)) - 1)))
            return Fail(pc, "a format store not translated yet");
        }
        Line("r_a = " + addr + ";");
        for (uint32_t i = 0; i < n; i++)
          Line("r_wr(" + std::to_string(rw_of[size_t(b)]) + ", r_a + " + U(4 * i) + ", " + V(vdata + i) + ");");
        return;
      }
    }
    Fail(pc, "a buffer instruction not translated yet");
  }

  // A buffer load at a 64-bit address (ADDR64: VGPRs vaddr, vaddr + 1,
  // plus the offsets; the V# adds nothing): from the range resolved for it
  // (Resolver::Wide), checked.
  void Wide(gaddr pc, const Instr& in, const Site& site) {
    const uint32_t vdata = (in.w1 >> 8) & 0xFF, vaddr = in.w1 & 0xFF, soff = in.w1 >> 24;
    const int b = ro_of[size_t(site.buffer)];
    if (in.fmt != Fmt::Mubuf || in.op < 12 || in.op > 15)
      return Fail(pc, "a buffer instruction at a 64-bit address not translated yet");
    if (b < 0) return Fail(pc, "a buffer load at a 64-bit address from a written range not translated yet");
    const uint32_t n = in.op == 12 ? 1 : in.op == 13 ? 2 : in.op == 14 ? 4 : 3;
    Line("r_t = " + U(in.w0 & 0xFFF) + " + " + Src(soff, in, pc) + "; r_a = " + V(vaddr) + " + r_t;");
    Line("if (" + V(vaddr + 1) + " + (r_a < r_t ? 1u : 0u) != r_region[" + std::to_string(b) + "].y) r_fault(" +
         U(uint32_t(pc)) + ");");
    for (uint32_t i = 0; i < n; i++)
      Line(V(vdata + i) + " = r_rdc(" + std::to_string(b) + ", r_a + " + U(4 * i) + ", " + U(uint32_t(pc)) + ");");
  }

  // An image access with descriptors loaded at a run-time offset: the
  // access for the candidate (Site::images) whose words the loaded T# (and
  // S#) equal, a fault if none.
  void DynamicImage(gaddr pc, const Instr& in, const Site& site) {
    const uint32_t t = ((in.w1 >> 16) & 0x1F) * 4, smp = ((in.w1 >> 21) & 0x1F) * 4;
    const int words = (in.w0 & 0x8000) ? 4 : 8;
    for (size_t k = 0; k < site.images.size(); k++) {
      const uint32_t m = site.match + 3 * uint32_t(k);
      std::string cond;
      auto word = [&](uint32_t reg, uint32_t at) {
        cond += (cond.empty() ? "" : " && ") + S(reg) + " == r_match[" + std::to_string(m + at / 4) + "]." +
                "xyzw"[at % 4];
      };
      for (int i = 0; i < words; i++) word(t + uint32_t(i), uint32_t(i));
      if (in.op >= 32)
        for (uint32_t i = 0; i < 4; i++) word(smp + i, 8 + i);
      Line(std::string(k ? "} else " : "") + "if (" + cond + ") {");
      Image(pc, in, site.images[k]);
    }
    if (!site.images.empty()) Line("} else {");
    Line("r_fault(" + U(uint32_t(pc)) + ");");
    if (!site.images.empty()) Line("}");
  }

  void Image(gaddr pc, const Instr& in, int image) {
    const ImageRef& ref = res.r.images[size_t(image)];
    const std::string t = "r_tex" + std::to_string(image);
    const uint32_t vaddr = in.w1 & 0xFF, vdata = (in.w1 >> 8) & 0xFF, dmask = (in.w0 >> 8) & 0xF;
    const gfx::TextureKind kind = ImageKind(ref.t);
    const char type = ImageType(ref.t);
    const int dims = kind == gfx::TextureKind::Tex2D ? 2 : 3;
    const std::string cast = type == 'f' ? "floatBitsToUint" : "uvec4";
    // The T#'s destination selects (0, 1, or a component), then DMASK.
    auto write = [&](const std::string& value) {
      Line("r_v4 = " + value + ";");
      if (in.op != 14 && (ref.t[3] & 0xFFF) != (4 | 5 << 3 | 6 << 6 | 7 << 9) &&
          !(in.op >= 32 && in.op <= 63 && ((in.op - 32) & 8))) {
        static const char* kSel[8] = {"0u", "1u", "0u", "0u", "r_t4.x", "r_t4.y", "r_t4.z", "r_t4.w"};
        const std::string one = type == 'f' ? "0x3F800000u" : "1u";
        std::string sw = "uvec4(";
        for (int i = 0; i < 4; i++) {
          const uint32_t sel = (ref.t[3] >> (3 * i)) & 7;
          sw += std::string(i ? ", " : "") + (sel == 1 ? one : kSel[sel]);
        }
        Line("r_t4 = r_v4; r_v4 = " + sw + ");");
      }
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
    if (in.op == 0 || in.op == 1) {  // image_load, image_load_mip
      if (kind == gfx::TextureKind::Cube) return Fail(pc, "a cube image load not translated yet");
      std::string c = dims == 2 ? "ivec2(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "))"
                                : "ivec3(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "), int(" + V(vaddr + 2) + "))";
      // Out of the image or its levels: zeros, as the GPU returns.
      const std::string lod = in.op == 1 ? "int(" + V(vaddr + uint32_t(dims)) + ")" : "0";
      Line("r_v4 = uvec4(0u);");
      Line("if (" + lod + " < textureQueryLevels(" + t + ") && all(lessThan(uvec" + std::to_string(dims) +
           "(" + c + "), uvec" + std::to_string(dims) + "(textureSize(" + t + ", " + lod + "))))) r_v4 = " + cast +
           "(texelFetch(" + t + ", " + c + ", " + lod + "));");
      write("r_v4");
      return;
    }
    if (in.op == 8) {  // image_store (compute)
      gfx::Format f;
      if (!TextureFormat(ref.t, &f) || !StorageQualifier(f))
        return Fail(pc, "an image store format not translated yet");
      std::string c = dims == 2 ? "ivec2(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "))"
                                : "ivec3(int(" + V(vaddr) + "), int(" + V(vaddr + 1) + "), int(" + V(vaddr + 2) + "))";
      std::string v[4] = {"0u", "0u", "0u", "0u"};
      uint32_t at = vdata;
      for (int i = 0; i < 4; i++)
        if (dmask & (1u << i)) v[i] = V(at++);
      const std::string img = "r_img" + std::to_string(image);
      const std::string val = "uvec4(" + v[0] + ", " + v[1] + ", " + v[2] + ", " + v[3] + ")";
      Line("imageStore(" + img + ", " + c + ", " + (type == 'f' ? "uintBitsToFloat(" + val + ")" : type == 'i' ? "ivec4(" + val + ")" : val) + ");");
      return;
    }
    if (in.op < 32 || in.op > 95) return Fail(pc, "an image instruction not translated yet");
    // Samples and gathers: [offset] [bias] [compare] [derivatives]
    // coordinates [lod].
    const bool gather = in.op >= 64;
    const uint32_t v = in.op - (gather ? 64 : 32);
    const bool offset = v >= 16, compare = (v & 8) != 0;
    const uint32_t mode = v & 7;  // 0 plain, 1 cl, 2 d, 3 d_cl, 4 l, 5 b, 6 b_cl, 7 lz
    if (mode == 1 || mode == 3 || mode == 6) return Fail(pc, "a sample with a level clamp not translated yet");
    const bool cube = kind == gfx::TextureKind::Cube, volume = kind == gfx::TextureKind::Tex3D;
    // Derivatives and offsets in the coordinates of a face, a slice or the volume.
    const int n = kind == gfx::TextureKind::Tex2D ? 2 : 3, nd = volume ? 3 : 2;
    uint32_t at = vaddr;
    std::string off, bias, ref_z, dx, dy;
    if (offset) off = V(at++);
    if (mode == 5) bias = F(V(at++));
    if (compare) ref_z = F(V(at++));
    if (mode == 2) {
      if (cube) return Fail(pc, "a cube sample with derivatives not translated yet");
      std::string g[6];
      for (int i = 0; i < 2 * nd; i++) g[i] = F(V(at++));
      dx = nd == 2 ? "vec2(" + g[0] + ", " + g[1] + ")" : "vec3(" + g[0] + ", " + g[1] + ", " + g[2] + ")";
      dy = nd == 2 ? "vec2(" + g[2] + ", " + g[3] + ")" : "vec3(" + g[3] + ", " + g[4] + ", " + g[5] + ")";
    }
    std::string c[3];
    for (int i = 0; i < n; i++) c[i] = F(V(at + uint32_t(i)));
    at += uint32_t(n);
    const std::string lod = mode == 4 ? F(V(at)) : "0.0";
    // Offsets (6 bits, signed, per coordinate, in the VGPR) are in texels
    // of the level sampled: added to the coordinates here, as GLSL takes
    // only constant ones; so only where that level is known.
    if (offset) {
      const uint32_t base = (ref.t[3] >> 12) & 0xF, last = (ref.t[3] >> 16) & 0xF;
      if (cube) return Fail(pc, "a cube sample with offsets not translated yet");
      if (base != last && mode != 7) return Fail(pc, "offsets into a texture of several levels, its level not known, not translated yet");
      const std::string size = "vec" + std::to_string(nd) + "(textureSize(" + t + ", 0)" + (nd == 2 ? ".xy" : "") + ")";
      for (int i = 0; i < nd; i++)
        c[i] = "(" + c[i] + " + float(bitfieldExtract(int(" + off + "), " + std::to_string(8 * i) + ", 6)) / " + size + "[" + std::to_string(i) + "])";
    }
    if (kind == gfx::TextureKind::Tex2DArray) c[2] = "roundEven(" + c[2] + ")";
    const std::string vec = cube ? "r_cubedir(" + c[0] + ", " + c[1] + ", " + c[2] + ")"
                            : n == 2 ? "vec2(" + c[0] + ", " + c[1] + ")"
                                     : "vec3(" + c[0] + ", " + c[1] + ", " + c[2] + ")";
    // Outside a pixel shader there are no derivatives: the level is 0,
    // biased or not.
    const bool pixel = st.stage == Stage::Pixel;
    if (gather) {
      // The 2x2 texels' one component (DMASK's one bit, through the T#'s
      // destination select), or their comparisons, in the order GLSL's
      // textureGather returns them, into four VGPRs. GLSL gathers from
      // the first level: so only at level 0 (lz, or no derivatives) or
      // from a texture of one level.
      const uint32_t base = (ref.t[3] >> 12) & 0xF, last = (ref.t[3] >> 16) & 0xF;
      if (volume) return Fail(pc, "a gather from a 3D texture");
      if (mode != 7 && base != last && (pixel || mode == 4 || mode == 5))
        return Fail(pc, "a gather from a texture of several levels, its level not known, not translated yet");
      std::string value;
      if (compare) {
        if (((ref.s[0] >> 12) & 7) == 0) {
          value = "floatBitsToUint(vec4(0.0))";
        } else {
          value = "floatBitsToUint(textureGather(" + t + ", " + vec + ", " + ref_z + "))";
        }
      } else {
        if (dmask == 0 || (dmask & (dmask - 1))) return Fail(pc, "a gather of other than one component");
        const uint32_t sel = (ref.t[3] >> (3 * uint32_t(std::countr_zero(dmask)))) & 7;
        if (sel >= 4) value = cast + "(textureGather(" + t + ", " + vec + ", " + std::to_string(sel - 4) + "))";
        else value = sel == 1 ? (type == 'f' ? "uvec4(0x3F800000u)" : "uvec4(1u)") : "uvec4(0u)";
      }
      Line("r_v4 = " + value + ";");
      for (uint32_t i = 0; i < 4; i++) Line(V(vdata + i) + " = r_v4[" + std::to_string(i) + "];");
      return;
    }
    std::string call;
    if (compare) {
      if (volume) return Fail(pc, "a compare sample of a 3D texture");
      // A sampler comparing with NEVER fails every texel: the result is 0
      // (its texture is declared without the comparison).
      if (((ref.s[0] >> 12) & 7) == 0) return write("floatBitsToUint(vec4(0.0))");
      const std::string sv = n == 2 ? "vec3(" + vec + ", " + ref_z + ")" : "vec4(" + vec + ", " + ref_z + ")";
      // Shadow arrays and cubes have no explicit level: zero derivatives
      // sample level 0.
      const std::string zero = cube ? "vec3(0.0)" : "vec2(0.0)";
      if (mode == 2) {
        call = "textureGrad(" + t + ", " + sv + ", " + dx + ", " + dy + ")";
      } else if (mode == 7 || (!pixel && mode != 4)) {
        call = n == 2 ? "textureLod(" + t + ", " + sv + ", 0.0)" : "textureGrad(" + t + ", " + sv + ", " + zero + ", " + zero + ")";
      } else if (mode == 4) {
        if (n != 2) return Fail(pc, "a compare sample of an array or cube at a level not translated yet");
        call = "textureLod(" + t + ", " + sv + ", " + lod + ")";
      } else if (mode == 5) {
        if (kind == gfx::TextureKind::Tex2DArray)
          return Fail(pc, "a biased compare sample of an array not translated yet");
        call = "texture(" + t + ", " + sv + ", " + bias + ")";
      } else {
        call = "texture(" + t + ", " + sv + ")";
      }
      write("floatBitsToUint(vec4(" + call + "))");
      return;
    }
    if (mode == 2) call = "textureGrad(" + t + ", " + vec + ", " + dx + ", " + dy + ")";
    else if (mode == 4 || mode == 7) call = "textureLod(" + t + ", " + vec + ", " + lod + ")";
    else if (mode == 5 && pixel) call = "texture(" + t + ", " + vec + ", " + bias + ")";
    else call = pixel ? "texture(" + t + ", " + vec + ")" : "textureLod(" + t + ", " + vec + ", 0.0)";
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
        if (compr && fmt == 5) {
          for (int i = 0; i < 4; i++)
            c[i] = "unpackUnorm2x16(" + v[i / 2] + ")." + "xy"[i % 2];
        }
        if (compr && fmt != 4 && fmt != 5) {
          Fail(pc, "a packed export format not translated yet");
        } else {
          int from[4];
          ExportOrder(st.col_swap, target, from);
          mrts.insert(target);
          for (int i = 0; i < 4; i++)
            if (compr || (en & (1u << from[i])))
              Line("r_color" + std::to_string(target) + "[" + std::to_string(i) + "] = " + c[from[i]] + ";");
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
      // r_misc2.z bit 0: the viewport's y scale is positive (the image
      // flips); bit 1: OpenGL's clip space (z from -w to w), which the
      // host's (0 to w) takes halved.
      Line("gl_Position = " + vec + ";");
      Line("if ((r_misc2.z & 1u) != 0u) gl_Position.y = -gl_Position.y;");
      Line("if ((r_misc2.z & 2u) != 0u) gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;");
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
      // Addresses past the LDS: writes dropped, reads zero.
      case 13: Line("r_lds_wr(" + a + " + " + U(off0 | off1 << 8) + ", " + V(d0) + ");"); return;
      case 14:
        Line("r_lds_wr(" + a + " + " + U(off0 * 4) + ", " + V(d0) + ");");
        Line("r_lds_wr(" + a + " + " + U(off1 * 4) + ", " + V(d1) + ");");
        return;
      case 10: Line("r_lds_or(" + a + " + " + U(off0 | off1 << 8) + ", " + V(d0) + ");"); return;
      case 54: Line(V(vdst) + " = r_lds_rd(" + a + " + " + U(off0 | off1 << 8) + ");"); return;
      case 55:
        Line(V(vdst) + " = r_lds_rd(" + a + " + " + U(off0 * 4) + ");");
        Line(V(vdst + 1) + " = r_lds_rd(" + a + " + " + U(off1 * 4) + ");");
        return;
      case 119:  // ds_read2_b64
        for (uint32_t i = 0; i < 4; i++)
          Line(V(vdst + i) + " = r_lds_rd(" + a + " + " + U((i < 2 ? off0 : off1) * 8 + (i & 1) * 4) + ");");
        return;
    }
    Fail(pc, "an LDS instruction not translated yet");
  }

  // ds_swizzle_b32 in its quad mode (offset bit 15): each lane of a quad
  // reads the lane of its quad that its 2-bit field of offset[7:0] selects.
  // Host quads are laid out as the GPU's (0-3: top left, top right, bottom
  // left, bottom right). Every invocation takes part, whatever its EXEC
  // bit: the swizzle reads the source lanes' registers.
  void Swizzle(gaddr pc, const Instr& in) {
    const uint32_t offset = in.w0 & 0xFFFF;
    const uint32_t addr = in.w1 & 0xFF, vdst = in.w1 >> 24;
    uint32_t sel[4] = {offset & 3, (offset >> 2) & 3, (offset >> 4) & 3, (offset >> 6) & 3};
    if (!(offset & 0x8000)) {
      // Bit-mask mode, in groups of 32 lanes: lane ((l & and) | or) ^ xor.
      // One that keeps each lane in its quad (lane bits 2-4 as they are)
      // is a quad permutation; others run in waves (a half of the wave is
      // a group).
      const uint32_t and_mask = offset & 0x1F, or_mask = (offset >> 5) & 0x1F, xor_mask = (offset >> 10) & 0x1F;
      if ((and_mask & 0x1C) != 0x1C || (or_mask & 0x1C) || (xor_mask & 0x1C)) {
        if (!wave) return Fail(pc, "ds_swizzle_b32 across quads outside a compute stage not translated yet");
        uses_shuffle = true;
        Line("r_t = subgroupShuffle(" + V(addr) + ", ((r_sl & " + U(and_mask) + ") | " + U(or_mask) + ") ^ " +
             U(xor_mask) + ");");
        Line("if (" + Active() + ") " + V(vdst) + " = r_t;");
        return;
      }
      for (uint32_t q = 0; q < 4; q++) sel[q] = (((q & and_mask) | or_mask) ^ xor_mask) & 3;
    }
    uses_quad = true;
    if (sel[0] == sel[1] && sel[0] == sel[2] && sel[0] == sel[3]) {
      Line("r_t = subgroupQuadBroadcast(" + V(addr) + ", " + U(sel[0]) + ");");
    } else {
      Line("r_q = gl_SubgroupInvocationID & 3u;");
      Line("r_t = r_q == 0u ? subgroupQuadBroadcast(" + V(addr) + ", " + U(sel[0]) + ") : r_q == 1u ? "
           "subgroupQuadBroadcast(" + V(addr) + ", " + U(sel[1]) + ") : r_q == 2u ? subgroupQuadBroadcast(" +
           V(addr) + ", " + U(sel[2]) + ") : subgroupQuadBroadcast(" + V(addr) + ", " + U(sel[3]) + ");");
    }
    Line("if (" + Active() + ") " + V(vdst) + " = r_t;");
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
            const int region = res.code().fetch.count(pc) ? 1 : 0;
            const gaddr rel = next - res.code().base[region];
            Line(Dst(d) + " = r_misc[" + std::to_string(2 * region) + "] + " + U(uint32_t(rel)) + ";");
            Line(Dst(d + 1) + " = r_misc[" + std::to_string(2 * region + 1) + "];");
            return;
          }
          case 33: {  // s_swappc_b64: call the fetch shader; the return block in sdst
            Line(Dst(d) + " = " + U(uint32_t(block_of.at(next))) + "; " + Dst(d + 1) + " = 0xFFFFFFFFu;");
            Line("pc = " + U(uint32_t(block_of.at(res.code().base[1]))) + "; continue;");
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
    if (site.dynamic) {  // a run-time offset: checked against its range
      if (ro_of[size_t(site.buffer)] < 0) return Fail(pc, "a scalar load at a run-time offset from a written range");
      for (uint32_t i = 0; i < n; i++)
        Line(S(d + i) + " = r_rdc(" + std::to_string(ro_of[size_t(site.buffer)]) + ", r_a + " + U(4 * i) + ", " +
             U(uint32_t(pc)) + ");");
      return;
    }
    for (uint32_t i = 0; i < n; i++) Line(S(d + i) + " = " + Load(site.buffer, "r_a + " + U(4 * i)) + ";");
  }

  // In waves: lane r_sl's bit of the half's word of a mask source (an SGPR
  // pair or a constant, 64 bits as SOP's operands).
  std::string MaskBit(uint32_t code, const Instr& in, gaddr pc) {
    if (code >= gcn::kVgpr) {
      Fail(pc, "a VGPR as a lane mask");
      return "0u";
    }
    if (code < 104 || code == gcn::kVccLo || code == gcn::kExecLo)
      return "((" + S(code + uint32_t(half)) + " >> r_sl) & 1u)";
    uint32_t k;
    Scalars none;
    if (!Source(none, in, code, &k)) {
      Fail(pc, "a lane mask not translated yet");
      return "0u";
    }
    if (half) k = code != gcn::kLiteral && int32_t(k) < 0 ? 0xFFFFFFFFu : 0u;
    return "((" + U(k) + " >> r_sl) & 1u)";
  }

  // In waves: v_readlane_b32, v_writelane_b32 and v_readfirstlane_b32
  // (false: another instruction). An empty EXEC reads lane 0.
  bool CrossLane(gaddr pc, const Instr& in) {
    const bool vop3 = in.fmt == Fmt::Vop3;
    uint32_t op = in.op;
    if (vop3) {
      if (op != 257 && op != 258 && op != 384 + 2) return false;
      op = op == 384 + 2 ? 2 + 384 : op - 256;
    } else if (!((in.fmt == Fmt::Vop2 && (op == 1 || op == 2)) || (in.fmt == Fmt::Vop1 && op == 2))) {
      return false;
    } else if (in.fmt == Fmt::Vop1) {
      op += 384;
    }
    const uint32_t dst = vop3 ? in.w0 & 0xFF : (in.w0 >> 17) & 0xFF;
    const uint32_t src0 = vop3 ? in.w1 & 0x1FF : in.w0 & 0x1FF;
    const uint32_t lane = vop3 ? (in.w1 >> 9) & 0x1FF : (in.w0 >> 9) & 0xFF;
    // A lane's value of src0 (r_l: the lane), all invocations taking part.
    auto read = [&]() -> std::string {
      if (src0 < gcn::kVgpr) return Src(src0, in, pc);
      uses_shuffle = true;
      half = 0;
      const std::string lo = V(src0 - gcn::kVgpr);
      half = 1;
      const std::string hi = V(src0 - gcn::kVgpr);
      half = 0;
      Line("r_t = subgroupShuffle(" + lo + ", r_l & 31u); r_t2 = subgroupShuffle(" + hi + ", r_l & 31u);");
      return "(r_l < 32u ? r_t : r_t2)";
    };
    switch (op) {
      case 1:
        Line("r_l = " + Src(lane, in, pc) + " & 63u;");
        Line(Dst(dst) + " = " + read() + ";");
        return true;
      case 384 + 2:
        Line("r_l = exec_lo != 0u ? uint(findLSB(exec_lo)) : exec_hi != 0u ? 32u + uint(findLSB(exec_hi)) : 0u;");
        Line(Dst(dst) + " = " + read() + ";");
        return true;
      case 2:  // whatever the lane's EXEC bit
        Line("r_l = " + Src(lane, in, pc) + " & 63u; r_t = " + Src(src0, in, pc) + ";");
        for (half = 0; half < 2; half++) Line("if (r_l == " + Lane() + ") " + V(dst) + " = r_t;");
        half = 0;
        return true;
    }
    return false;
  }

  // v_movreld_b32, v_movrels_b32, v_movrelsd_b32: VGPRs indexed by M0
  // (vdst + M0, src0 + M0). Past the stage's VGPRs (Sea Islands ISA,
  // "Out-of-Range GPRs") a source reads VGPR0 and a destination is not
  // written.
  void Movrel(gaddr pc, const Instr& in, uint32_t op) {
    const bool vop3 = in.fmt == Fmt::Vop3;
    const uint32_t vdst = vop3 ? in.w0 & 0xFF : (in.w0 >> 17) & 0xFF;
    const uint32_t src0 = vop3 ? in.w1 & 0x1FF : in.w0 & 0x1FF;
    const uint32_t count = ((st.rsrc1 & 0x3F) + 1) * 4;
    auto indexed = [&](uint32_t base, bool write) {
      Line("r_x = " + U(base) + " + m0;");
      Line("switch (r_x) {");
      for (uint32_t i = 0; i < count; i++)
        Line("case " + U(i) + ": " + (write ? V(i) + " = r_t" : "r_t = " + V(i)) + "; break;");
      Line(std::string("default: ") + (write ? "break;" : "r_t = " + V(0) + "; break;"));
      Line("}");
    };
    if (op != 66 && src0 < gcn::kVgpr) return Fail(pc, "v_movrels of a scalar");
    Line("if (" + Active() + ") {");
    if (op == 66) Line("r_t = " + Src(src0, in, pc) + ";");
    else indexed(src0 - gcn::kVgpr, false);
    if (op == 67) Line(V(vdst) + " = r_t;");
    else indexed(vdst, true);
    Line("}");
  }

  // A vector instruction: in waves, for each half, then the ballot of an
  // SGPR pair it writes (comparisons, carries; inactive lanes 0).
  void Vector(gaddr pc, const Instr& in) {
    if (!wave) return VectorLanes(pc, in, nullptr);
    if (CrossLane(pc, in)) return;
    int ballot = -1;
    for (half = 0; half < 2; half++) VectorLanes(pc, in, &ballot);
    half = 0;
    if (ballot < 0) return;
    uses_ballot = true;
    Line("r_t = subgroupBallot(r_b0).x; r_t2 = subgroupBallot(r_b1).x;");
    Line(Dst(uint32_t(ballot) & 0xFF) + " = r_t; " + Dst((uint32_t(ballot) & 0xFF) + 1) + " = r_t2;");
    if (ballot & 0x100) Line("exec_lo = r_t; exec_hi = r_t2;");  // cmpx
  }

  // A vector instruction for the lanes of one invocation (in waves: half
  // `half`, the SGPR pair it ballots in *ballot, 0x100 set for cmpx).
  void VectorLanes(gaddr pc, const Instr& in, int* ballot) {
    std::string s[3];
    bool abs[3] = {}, neg[3] = {};
    uint32_t omod = 0;
    bool clamp = false;
    std::string d;
    uint32_t sdst = gcn::kVccLo;
    uint32_t op;
    const std::string b = "r_b" + std::to_string(half);
    switch (in.fmt) {
      case Fmt::Vop2:
        op = in.op;
        s[0] = Src(in.w0 & 0x1FF, in, pc);
        s[1] = V((in.w0 >> 9) & 0xFF);
        s[2] = wave ? MaskBit(gcn::kVccLo, in, pc) : "vcc_lo";
        d = V((in.w0 >> 17) & 0xFF);
        break;
      case Fmt::Vop1:
        op = 384 + in.op;
        if (in.op == 0) return;  // v_nop
        if (in.op == 2) {  // v_readfirstlane_b32: this lane is the first
          Line(Dst((in.w0 >> 17) & 0xFF) + " = " + Src(in.w0 & 0x1FF, in, pc) + ";");
          return;
        }
        if (in.op >= 66 && in.op <= 68) return Movrel(pc, in, in.op);
        s[0] = Src(in.w0 & 0x1FF, in, pc);
        d = V((in.w0 >> 17) & 0xFF);
        break;
      case Fmt::Vopc: {
        s[0] = Src(in.w0 & 0x1FF, in, pc);
        s[1] = V((in.w0 >> 9) & 0xFF);
        std::string cond;
        if (!Compare(pc, in.op, s, abs, neg, &cond)) return;
        // Inactive lanes compare false.
        if (wave) {
          Line(b + " = " + Active() + " && " + cond + ";");
          *ballot = int(gcn::kVccLo) | ((in.op >> 4) & 1 ? 0x100 : 0);
          return;
        }
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
          if (wave) {
            Line(b + " = " + Active() + " && " + cond + ";");
            *ballot = int(sd) | ((op >> 4) & 1 ? 0x100 : 0);
            return;
          }
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
        if (op >= 384 + 66 && op <= 384 + 68) return Movrel(pc, in, op - 384);
        if (wave && (op == 0 || (op >= 40 && op <= 42))) s[2] = MaskBit((w1 >> 18) & 0x1FF, in, pc);
        break;
      }
      default:
        return Fail(pc, "an instruction not translated yet");
    }
    // Carries out: inactive lanes' bits are 0.
    if (wave && op >= 37 && op <= 42) {
      Line(b + " = false;");
      *ballot = int(sdst);
    }
    // The lane's EXEC bit guards every VGPR write.
    const std::string saved = body;
    body.clear();
    Valu(pc, in, op, d, s, abs, neg, omod, clamp, sdst);
    std::string ops = body;
    body = saved;
    Line("if (" + Active() + ") {");
    body += ops;
    Line("}");
  }

  void Instruction(gaddr pc, const Instr& in, gaddr next) {
    const auto* site = res.SiteAt(pc);
    switch (in.fmt) {
      case Fmt::Sopp: {
        switch (in.op) {
          case 0: case 12: case 13: case 14: case 15: case 17: case 19: return;  // s_nop, s_waitcnt, ...
          case 1: End(); return;
          case 16: return SendMsg(in);
          case 10: Line("barrier();"); return;
        }
        if (IsBranch(in)) {
          const gaddr target = next + 4 * Simm16(in);
          const std::string tb = U(uint32_t(block_of.at(target)));
          const std::string nb = in.op == 2 ? "" : U(uint32_t(block_of.at(next)));
          const std::string kCond[10] = {"", "", "true", "", "scc == 0u", "scc != 0u",
                                         Empty("vcc", wave), "!(" + Empty("vcc", wave) + ")",
                                         Empty("exec", wave), "!(" + Empty("exec", wave) + ")"};
          if (in.op == 2) Line("pc = " + tb + "; continue;");
          else Line("pc = (" + kCond[in.op] + ") ? " + tb + " : " + nb + "; continue;");
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
      case Fmt::Exp:
        return Export(pc, in);
      default:
        break;
    }
    // Memory instructions: lane by lane (in waves, a half at a time).
    for (half = 0; half < (wave ? 2 : 1); half++) {
      switch (in.fmt) {
        case Fmt::Mubuf: case Fmt::Mtbuf:
          if (in.fmt == Fmt::Mubuf && in.op >= 112) break;
          Line("if (" + Active() + ") {");
          Buffer(pc, in, site->second);
          Line("}");
          break;
        case Fmt::Mimg:
          Line("if (" + Active() + ") {");
          if (site->second.image >= 0) Image(pc, in, site->second.image);
          else DynamicImage(pc, in, site->second);
          Line("}");
          break;
        case Fmt::Ds:
          if (in.op == 53) {
            Swizzle(pc, in);
            break;
          }
          Line("if (" + Active() + ") {");
          Lds(pc, in);
          Line("}");
          break;
        default:
          break;
      }
    }
    half = 0;
  }

  // s_sendmsg: a GS's cuts and emits on stream 0 (the one drawn) as its
  // emit words (Uniforms()); other messages are not acted on.
  void SendMsg(const Instr& in) {
    const uint32_t imm = in.w0 & 0xFFFF, op = (imm >> 4) & 3;
    if (st.stage != Stage::Geometry || (imm & 0xF) != 2 || ((imm >> 8) & 3) != 0) return;
    Line("if ((exec_lo & 1u) != 0u) {");
    if (op & 2) {
      Line("if (r_nv < r_misc3.z) r_wr(" + std::to_string(emits) +
           ", (r_id * r_misc3.z + r_nv) * 4u, 1u | (r_cut << 1));");
      Line("r_nv++; r_cut = 0u;");
    }
    if (op & 1) Line("r_cut = 1u;");
    Line("}");
  }

  void End() {
    if (st.stage == Stage::Pixel && mrts.empty() && !depth) Line("if ((exec_lo & 1u) == 0u) discard;");
    Line("return;");
  }

  // ---- the whole stage

  bool Run(Translated* out) {
    // Bindings: in a compute stage, written ranges are read-write.
    for (const BufferRange& b : res.r.buffers) {
      const bool rw = RunsAsCompute(st.stage) && b.written;
      ro_of.push_back(rw ? -1 : nro);
      rw_of.push_back(rw ? nrw : -1);
      (rw ? nrw : nro)++;
    }
    if (st.stage == Stage::Export) ids = nro++;
    if (st.stage == Stage::Geometry) emits = nrw++;
    if (res.r.faults) fault = nrw++;
    for (const BufferRange& b : res.r.buffers)
      if (b.written && !RunsAsCompute(st.stage)) {
        error = "a store outside a compute stage not translated yet";
        return false;
      }
    int n = 0;
    for (gaddr l : res.code().leaders)
      if (res.code().at.count(l)) block_of[l] = n++;
    // Blocks in address order (the fetch shader is a region of its own).
    std::vector<std::pair<gaddr, Instr>> order(res.code().at.begin(), res.code().at.end());
    const bool loop = res.code().branches;
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
    if (uses_quad) g += "#extension GL_KHR_shader_subgroup_quad : require\n";
    if (uses_shuffle) g += "#extension GL_KHR_shader_subgroup_shuffle : require\n";
    if (uses_ballot) g += "#extension GL_KHR_shader_subgroup_ballot : require\n";
    if (wave) g += "#extension GL_KHR_shader_subgroup_basic : require\n";
    // The uniform block: user data, the code bases, each range's base and
    // size (Uniforms()).
    g += "layout(std140, R_UNIFORMS) uniform RUser {\n  uvec4 r_user[4];\n  uvec4 r_misc;\n  uvec4 r_misc2;\n"
         "  uvec4 r_misc3;\n";
    if (nro) g += "  uvec4 r_region[" + std::to_string(nro) + "];\n";
    if (nrw) g += "  uvec4 r_rwregion[" + std::to_string(nrw) + "];\n";
    if (!res.r.match.empty()) g += "  uvec4 r_match[" + std::to_string(res.r.match.size() / 4) + "];\n";
    g += "};\n";
    if (nro)
      g += "uint r_rd(int b, uint a) {\n  uint o = a - r_region[b].x;\n"
           "  return o < r_region[b].z ? r_ld(uint(b), o >> 2) : 0u;\n}\n"
           "uint r_rdu(int b, uint a) {\n  uint lo = r_rd(b, a & ~3u), hi = r_rd(b, (a & ~3u) + 4u);\n"
           "  uint sh = (a & 3u) * 8u;\n  return sh == 0u ? lo : (lo >> sh) | (hi << (32u - sh));\n}\n";
    if (nrw)
      g += "uint r_rdw(int b, uint a) {\n  uint o = a - r_rwregion[b].x;\n"
           "  return o < r_rwregion[b].z ? r_rw[r_rwbuf[b].x + (o >> 2)] : 0u;\n}\n"
           "void r_wr(int b, uint a, uint v) {\n  uint o = a - r_rwregion[b].x;\n"
           "  if (o < r_rwregion[b].z) r_rw[r_rwbuf[b].x + (o >> 2)] = v;\n}\n";
    if (fault >= 0) {
      g += "void r_fault(uint at) { r_rw[r_rwbuf[" + std::to_string(fault) + "].x] = at; }\n";
      if (nro)
        g += "uint r_rdc(int b, uint a, uint at) {\n  uint o = a - r_region[b].x;\n"
             "  if (o >= r_region[b].z) { r_fault(at); return 0u; }\n  return r_ld(uint(b), o >> 2);\n}\n";
    }
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
         "float r_cubeid(float x, float y, float z) {\n"
         "  if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0 ? 5.0 : 4.0;\n"
         "  if (abs(y) >= abs(x)) return y < 0.0 ? 3.0 : 2.0;\n"
         "  return x < 0.0 ? 1.0 : 0.0;\n}\n"
         "float r_cubesc(float x, float y, float z) {\n"
         "  if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0 ? -x : x;\n"
         "  if (abs(y) >= abs(x)) return x;\n"
         "  return x < 0.0 ? z : -z;\n}\n"
         "float r_cubetc(float x, float y, float z) {\n"
         "  if (abs(z) >= abs(x) && abs(z) >= abs(y)) return -y;\n"
         "  if (abs(y) >= abs(x)) return y < 0.0 ? -z : z;\n"
         "  return -y;\n}\n"
         "float r_cubema(float x, float y, float z) {\n"
         "  if (abs(z) >= abs(x) && abs(z) >= abs(y)) return 2.0 * z;\n"
         "  if (abs(y) >= abs(x)) return 2.0 * y;\n"
         "  return 2.0 * x;\n}\n"
         // A cube sample's face coordinates (1 to 2 across a face, as
         // r_cubesc / |r_cubema| + 1.5 gives them) and face back to a
         // direction.
         "vec3 r_cubedir(float s, float t, float face) {\n"
         "  float u = 2.0 * s - 3.0, v = 2.0 * t - 3.0;\n"
         "  switch (int(roundEven(face))) {\n"
         "    case 0: return vec3(1.0, -v, -u);\n"
         "    case 1: return vec3(-1.0, -v, u);\n"
         "    case 2: return vec3(u, 1.0, v);\n"
         "    case 3: return vec3(u, -1.0, -v);\n"
         "    case 4: return vec3(u, -v, 1.0);\n"
         "    default: return vec3(-u, -v, -1.0);\n"
         "  }\n}\n"
         "uint r_wqm(uint m) { m |= (m >> 1) & 0x55555555u; m |= (m << 1) & 0xAAAAAAAAu;\n"
         "  m |= (m >> 2) & 0x33333333u; m |= (m << 2) & 0xCCCCCCCCu; return m; }\n";
    // Textures and images.
    for (size_t i = 0; i < res.r.images.size(); i++) {
      const ImageRef& ref = res.r.images[i];
      const gfx::TextureKind k = ImageKind(ref.t);
      const char type = ImageType(ref.t);
      const std::string prefix = type == 'f' ? "" : std::string(1, type == 'u' ? 'u' : 'i');
      if (ref.storage) {
        gfx::Format f;
        const char* q = TextureFormat(ref.t, &f) ? StorageQualifier(f) : nullptr;
        std::snprintf(b, sizeof b, "layout(R_IMAGE(%zu), %s) uniform writeonly %simage%s r_img%zu;\n", ImgSlot(i),
                      q ? q : "rgba8", prefix.c_str(),
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
      g += "uint r_lds_rd(uint a) { return (a >> 2) < uint(r_lds.length()) ? r_lds[a >> 2] : 0u; }\n"
           "void r_lds_wr(uint a, uint v) { if ((a >> 2) < uint(r_lds.length())) r_lds[a >> 2] = v; }\n"
           "void r_lds_or(uint a, uint v) { if ((a >> 2) < uint(r_lds.length())) atomicOr(r_lds[a >> 2], v); }\n";
    }
    g += st.rects ? "void r_vs(uint r_vid) {\n" : "void main() {\n";
    g += "  uint scc = 0u, m0 = 0u, vcc_lo = 0u, vcc_hi = 0u, exec_lo = 1u, exec_hi = 0u;\n";
    g += "  uint r_t, r_t2, r_a, r_w, r_w1, r_w2, r_w3, r_unused; uvec4 r_v4, r_t4; uint r_c[4], r_d[4], r_q;\n"
         "  uint r_i, r_o, r_lane = 0u, r_l, r_x, r_sl = 0u; bool r_b0, r_b1;\n";
    for (int i = 0; i < 104; i++) {
      const std::string init = i < 16 ? "r_user[" + std::to_string(i / 4) + "][" + std::to_string(i % 4) + "]" : "0u";
      std::snprintf(b, sizeof b, "%ss%d = %s%s", i % 8 ? " " : "  uint ", i, init.c_str(), i % 8 == 7 ? ";\n" : ",");
      g += b;
    }
    const std::string inputs = Inputs();  // can name more VGPRs
    for (int i = 0; i <= max_vgpr; i++) {
      std::snprintf(b, sizeof b, wave ? "  uint v%d_0 = 0u, v%d_1 = 0u;\n" : "  uint v%d = 0u;\n", i, i);
      g += b;
    }
    g += inputs;
    for (uint32_t m : mrts) g += "  r_color" + std::to_string(m) + " = vec4(0.0);\n";
    for (uint32_t p : params) g += "  r_param" + std::to_string(p) + " = vec4(0.0);\n";
    g += body;
    g += "}\n";
    if (st.rects) {
      g += "void main() {\n"
           "  uint r_k = uint(gl_VertexIndex) % 6u, r_r = uint(gl_VertexIndex) / 6u * 3u;\n"
           "  uint r_c = r_k < 3u ? r_k : r_k == 3u ? 1u : r_k == 4u ? 3u : 2u;\n"
           "  if (r_c < 3u) {\n    r_vs(r_r + r_c);\n    return;\n  }\n"
           "  r_vs(r_r);\n  vec4 r_p0 = gl_Position;\n";
      for (uint32_t p : params) g += "  vec4 r_q0_" + std::to_string(p) + " = r_param" + std::to_string(p) + ";\n";
      g += "  r_vs(r_r + 1u);\n  vec4 r_p1 = gl_Position;\n";
      for (uint32_t p : params) g += "  vec4 r_q1_" + std::to_string(p) + " = r_param" + std::to_string(p) + ";\n";
      g += "  r_vs(r_r + 2u);\n  gl_Position += r_p1 - r_p0;\n";
      for (uint32_t p : params) {
        const std::string n = std::to_string(p);
        g += "  r_param" + n + " += r_q1_" + n + " - r_q0_" + n + ";\n";
      }
      g += "}\n";
    }
    return g;
  }

  size_t ImgSlot(size_t image) const {
    size_t slot = 0;
    for (size_t i = 0; i < image; i++)
      if (res.r.images[i].storage) slot++;
    return slot;
  }
  size_t TexSlot(size_t image) const {
    size_t slot = 0;
    for (size_t i = 0; i < image; i++)
      if (!res.r.images[i].storage) slot++;
    return slot;
  }
  bool UsesCompare(int image) const {
    for (auto& [pc, site] : res.sites)
      if (site.image == image ||
          std::find(site.images.begin(), site.images.end(), image) != site.images.end()) {
        const Instr& in = res.code().at.at(pc);
        if (in.fmt == Fmt::Mimg && in.op >= 32 && in.op <= 95 && ((in.op - 32) & 8)) return true;
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
    auto sg = [&](uint32_t n, const char* e) {
      std::snprintf(b, sizeof b, "  s%u = %s;\n", n, e);
      g += b;
    };
    switch (st.stage) {
      case Stage::Vertex: {
        vg(0, st.rects ? "r_vid" : "uint(gl_VertexIndex)");
        const uint32_t comps = (st.rsrc1 >> 24) & 3;
        if (comps >= 1) vg(1, "uint(gl_InstanceIndex) / max(r_misc2.x, 1u)");
        if (comps >= 2) vg(2, "uint(gl_InstanceIndex) / max(r_misc2.y, 1u)");
        if (comps >= 3) vg(3, "uint(gl_InstanceIndex)");
        // SO_EN: the stream-out configuration (stream 0: a copy shader
        // exports that stream), write index and enabled buffers' offsets.
        if (st.rsrc2 & (1u << 12)) {
          uint32_t s = user;
          sg(s++, "0u");
          sg(s++, "0u");
          for (int i = 0; i < 4; i++)
            if (st.rsrc2 & (1u << (8 + i))) sg(s++, "0u");
        }
        break;
      }
      // The passes' invocations: one per vertex (Export) or primitive
      // (Geometry), in waves of 64, each wave's place in its ring (ESGS,
      // GSVS) the next SGPR after the user data. A GS's vertex offsets are
      // the dwords of its vertices' ES lanes in the ESGS ring.
      case Stage::Export: case Stage::Geometry: {
        g += "  uint r_id = gl_GlobalInvocationID.x;\n  if (r_id >= r_misc2.z) return;\n"
             "  r_lane = gl_LocalInvocationID.x;\n";
        if (st.stage == Stage::Export) {
          sg(user, "gl_WorkGroupID.x * r_misc3.x");
          vg(0, ("r_rd(" + std::to_string(ids) + ", r_id * 4u)").c_str());
          break;
        }
        g += "  uint r_nv = 0u, r_cut = 1u;\n";
        sg(user, "gl_WorkGroupID.x * r_misc3.y");
        sg(user + 1, "gl_WorkGroupID.x");
        static const uint32_t kVertexVgpr[6] = {0, 1, 3, 4, 5, 6};
        for (uint32_t i = 0; i < 6; i++) {
          const std::string at = "(r_id * r_misc2.w + " + U(i) + ")";
          vg(kVertexVgpr[i], ("r_misc2.w > " + U(i) + " ? (" + at + " >> 6) * (r_misc3.x >> 2) + (" + at +
                              " & 63u) : 0u").c_str());
        }
        vg(2, "r_misc3.w + r_id");
        vg(7, "0u");
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
        // r_misc2.w: the first group in x of a dispatch run in parts.
        for (int i = 0; i < 3; i++)
          if (st.rsrc2 & (1u << (7 + i))) {
            std::snprintf(b, sizeof b, "  s%u = gl_WorkGroupID[%d]%s;\n", s++, i, i ? "" : " + r_misc2.w");
            g += b;
          }
        if (st.rsrc2 & (1u << 10)) {
          std::snprintf(b, sizeof b, "  s%u = 0u;\n", s++);
          g += b;
        }
        // Waves take a group's invocations in order, 64 at a time.
        const uint32_t comps = (st.rsrc2 >> 11) & 3;
        if (!wave) {
          g += "  r_lane = gl_LocalInvocationIndex % 64u;\n";
          vg(0, "gl_LocalInvocationID.x");
          if (comps >= 1) vg(1, "gl_LocalInvocationID.y");
          if (comps >= 2) vg(2, "gl_LocalInvocationID.z");
          break;
        }
        // In waves: host invocation i runs lanes i % 32 and i % 32 + 32 of
        // wave i / 32, the group's invocations past its size inactive.
        const uint32_t tx = std::max(st.threads[0], 1u), ty = std::max(st.threads[1], 1u),
                       n = tx * ty * std::max(st.threads[2], 1u);
        g += "  r_sl = gl_SubgroupInvocationID;\n";
        g += "  if (gl_SubgroupSize != 32u || gl_LocalInvocationIndex % 32u != r_sl ||\n"
             "      gl_SubgroupID != gl_LocalInvocationIndex / 32u) r_fault(" + U(uint32_t(st.code)) + ");\n";
        g += "  r_i = min(" + U(n) + " - gl_LocalInvocationIndex / 32u * 64u, 64u);\n"
             "  exec_lo = r_i >= 32u ? 0xFFFFFFFFu : (1u << r_i) - 1u;\n"
             "  exec_hi = r_i >= 64u ? 0xFFFFFFFFu : r_i > 32u ? (1u << (r_i - 32u)) - 1u : 0u;\n";
        for (half = 0; half < 2; half++) {
          const std::string i = "(gl_LocalInvocationIndex / 32u * 64u + r_sl + " + U(32u * uint32_t(half)) + ")";
          g += "  " + V(0) + " = " + i + " % " + U(tx) + ";\n";
          if (comps >= 1) g += "  " + V(1) + " = " + i + " / " + U(tx) + " % " + U(ty) + ";\n";
          if (comps >= 2) g += "  " + V(2) + " = " + i + " / " + U(tx * ty) + ";\n";
        }
        half = 0;
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

// A number for the code's words: equal words, equal numbers.
uint32_t CodeId(const Code& c) {
  if (c.id) return c.id;
  static auto* ids = new std::unordered_map<std::string, uint32_t>;  // as WalkCache
  std::string w;
  w.reserve(12 * c.at.size());
  for (auto& [pc, in] : c.at) {
    const uint32_t words[3] = {in.w0, in.w1, in.literal};
    w += Hex(words, sizeof words);
  }
  c.id = ids->try_emplace(std::move(w), uint32_t(ids->size() + 1)).first->second;
  return c.id;
}

}  // namespace

Resolved Resolve(const StageState& st) {
  Resolver rv(st);
  if (!rv.Run()) return rv.r;
  // The key: the stage, its registers that shape the code, the code's
  // words (CodeId), each memory instruction's binding and the formats it reads or writes.
  std::string& k = rv.r.key;
  const auto add = [&k](const void* p, size_t n) { k.append(static_cast<const char*>(p), n); };
  k.reserve(176 + 28 * rv.sites.size() + rv.r.buffers.size());
  const uint32_t head[] = {uint32_t(st.stage), st.rsrc1, st.rsrc2, st.ps_input_addr,
                           st.col_format, st.z_format, st.col_swap, st.threads[0], st.threads[1],
                           st.threads[2], uint32_t(st.rects)};
  add(head, sizeof head);
  if (st.stage == Stage::Pixel) add(st.ps_input_cntl, sizeof st.ps_input_cntl);
  const uint32_t id = CodeId(rv.code());
  add(&id, sizeof id);
  for (auto& [pc, s] : rv.sites) {
    const uint32_t site[4] = {uint32_t(s.buffer), uint32_t(s.image), s.vsharp[3], s.vsharp[1] >> 31};
    add(site, sizeof site);
    std::vector<int> images = s.images;
    if (s.image >= 0) images = {s.image};
    const uint32_t dyn[3] = {uint32_t(s.dynamic), uint32_t(s.images.size()), s.match};
    if (s.dynamic || !s.images.empty()) add(dyn, sizeof dyn);
    for (int i : images) {
      const ImageRef& ref = rv.r.images[size_t(i)];
      const uint32_t f[4] = {ref.t[1] & (ref.storage ? 0x3FF00000 : 0x3C000000), ref.t[3] & 0xF00FFFFF,
                             ref.s[0] & 0x7000, uint32_t(i)};
      add(f, sizeof f);
    }
  }
  k += rv.r.faults ? 'f' : '-';
  k += rv.r.wave ? 'w' : '-';
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
  rv.r.images = r.images;  // the caller's (its destination selects can differ)
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
  for (int i = 0; i < 3; i++) t.local_size[i] = std::max(st.threads[i], 1u);
  if (rv.r.wave) {
    const uint32_t n = t.local_size[0] * t.local_size[1] * t.local_size[2];
    t.local_size[0] = (n + 63) / 64 * 32;
    t.local_size[1] = t.local_size[2] = 1;
  }
  return t;
}

std::vector<uint32_t> Uniforms(const StageState& st, const Resolved& r) {
  std::vector<uint32_t> u(24, 0);
  std::memcpy(u.data(), st.user, sizeof st.user);
  u[16] = uint32_t(st.code);
  u[17] = uint32_t(st.code >> 32);
  u[18] = uint32_t(r.fetch);
  u[19] = uint32_t(r.fetch >> 32);
  // r_misc2: instance step rates (set by the caller after); the passes'
  // invocations and vertices per primitive. r_misc3: the passes' rings.
  if (st.stage == Stage::Export || st.stage == Stage::Geometry) {
    u[22] = st.invocations;
    u[23] = st.prim_verts;
  }
  u.insert(u.end(), {st.es_wave_bytes, st.gs_wave_bytes, st.max_vert, st.first_prim});
  for (int pass = 0; pass < 2; pass++) {
    for (const BufferRange& b : r.buffers) {
      const bool rw = RunsAsCompute(st.stage) && b.written;
      if (rw != (pass == 1)) continue;
      u.push_back(uint32_t(b.base));
      u.push_back(uint32_t(b.base >> 32));
      u.push_back(b.size);
      u.push_back(0);
    }
    if (pass == 0 && st.stage == Stage::Export) u.insert(u.end(), {0, 0, st.invocations * 4, 0});
    if (pass == 1 && st.stage == Stage::Geometry) u.insert(u.end(), {0, 0, st.invocations * st.max_vert * 4, 0});
    if (pass == 1 && r.faults) u.insert(u.end(), {0, 0, 4, 0});
  }
  u.insert(u.end(), r.match.begin(), r.match.end());
  return u;
}

void ExportOrder(uint32_t col_swap, uint32_t target, int from[4]) {
  // Memory X (and Y) by COMP_SWAP: STD R (RG), ALT G (RA), STD_REV B
  // (GR), ALT_REV A (AR).
  static const int kOne[4] = {0, 1, 2, 3}, kTwo[4][2] = {{0, 1}, {0, 3}, {1, 0}, {3, 0}};
  const uint32_t sw = (col_swap >> (4 * target)) & 0xF, swap = sw & 3;
  for (int i = 0; i < 4; i++) from[i] = i;
  if (sw >> 2 == 1) from[0] = kOne[swap];
  if (sw >> 2 == 2) from[0] = kTwo[swap][0], from[1] = kTwo[swap][1];
}

// A texture's format from a T# (IMG_DATA_FORMAT, IMG_NUM_FORMAT), its
// components in memory order (the destination selects reorder them).
bool TextureFormat(const uint32_t t[8], gfx::Format* out) {
  using F = gfx::Format;
  const uint32_t dfmt = (t[1] >> 20) & 0x3F, nfmt = (t[1] >> 26) & 0xF;
  const bool un = nfmt == 0, fl = nfmt == 7, srgb = nfmt == 9, ui = nfmt == 4, si = nfmt == 5;
  switch (dfmt) {
    case 1: if (un) return *out = F::R8, true; if (ui) return *out = F::R8Uint, true; break;
    case 2: if (fl) return *out = F::R16F, true; if (un) return *out = F::R16, true;
            if (ui) return *out = F::R16Uint, true; break;
    case 3: if (un) return *out = F::R8G8, true; break;
    case 4: if (fl) return *out = F::R32F, true; if (ui) return *out = F::R32Uint, true;
            if (si) return *out = F::R32Sint, true; break;
    case 5: if (fl) return *out = F::G16R16F, true; if (un) return *out = F::R16G16, true;
            if (nfmt == 1) return *out = F::R16G16Snorm, true; break;
    case 7: if (fl) return *out = F::R11G11B10F, true; break;
    case 9: if (un) return *out = F::R10G10B10A2, true; break;
    case 10:
      if (un) return *out = F::R8G8B8A8, true;
      if (srgb) return *out = F::R8G8B8A8Srgb, true;
      if (nfmt == 1) return *out = F::R8G8B8A8Snorm, true;
      if (ui) return *out = F::R8G8B8A8Uint, true;
      break;
    case 11: if (fl) return *out = F::G32R32F, true; break;
    case 12: if (fl) return *out = F::A16B16G16R16F, true; if (un) return *out = F::R16G16B16A16, true; break;
    case 14: if (fl) return *out = F::A32B32G32R32F, true; if (ui) return *out = F::R32G32B32A32Uint, true; break;
    case 35: return *out = srgb ? F::Bc1Srgb : F::Bc1, true;
    case 36: return *out = srgb ? F::Bc2Srgb : F::Bc2, true;
    case 37: return *out = srgb ? F::Bc3Srgb : F::Bc3, true;
    case 38: return *out = F::Bc4, true;
    case 39: return *out = F::Bc5, true;
    case 40: return *out = nfmt == 1 || si ? F::Bc6hSfloat : F::Bc6hUfloat, true;
    case 41: return *out = srgb ? F::Bc7Srgb : F::Bc7, true;
  }
  return false;
}

// The GLSL format qualifier of a storage image of format f; null for one
// not written yet.
const char* StorageQualifier(gfx::Format f) {
  using F = gfx::Format;
  switch (f) {
    case F::R8: return "r8";
    case F::R8Uint: return "r8ui";
    case F::R16F: return "r16f";
    case F::R16: return "r16";
    case F::R16Uint: return "r16ui";
    case F::R8G8: return "rg8";
    case F::R32F: return "r32f";
    case F::R32Uint: return "r32ui";
    case F::R32Sint: return "r32i";
    case F::G16R16F: return "rg16f";
    case F::R16G16: return "rg16";
    case F::R16G16Snorm: return "rg16_snorm";
    case F::R11G11B10F: return "r11f_g11f_b10f";
    case F::R10G10B10A2: return "rgb10_a2";
    case F::R8G8B8A8: return "rgba8";
    case F::R8G8B8A8Snorm: return "rgba8_snorm";
    case F::R8G8B8A8Uint: return "rgba8ui";
    case F::G32R32F: return "rg32f";
    case F::A16B16G16R16F: return "rgba16f";
    case F::R16G16B16A16: return "rgba16";
    case F::A32B32G32R32F: return "rgba32f";
    case F::R32G32B32A32Uint: return "rgba32ui";
    default: return nullptr;
  }
}

}  // namespace recomp::orbis::gpu
