// --x87-test: generates the inputs of tests/x86/lift_x87_test.cpp. Every
// distinct x87 instruction form in the EXE is lifted on its own, next to
// the bytes the test runs natively, so the test can run both from the same
// FPU state and compare.
//
// Memory operands (here and in the integer and SSE forms below) keep their
// addressing: base, index, scale, displacement, fs/gs, absolute and
// RIP-relative operands are lifted as written and re-aimed at a buffer the
// test maps at the same low address natively and in the guest arena
// (KeepAddress). Where the native run cannot reproduce the address (the
// stack pointer in it, ...), the operand is rewritten to [R] (ModRM mod=0)
// with R a register the instruction does not use otherwise. Forms are told
// apart by that [R] rewrite; each new addressing shape adds a form too.

#include "x87test.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "emit.h"
#include "function.h"

namespace lifter {
namespace {

// Where the tests' buffer is (the generated file passes these on as
// kFormTarget, kFormSegOffset, kFormRipTarget): plain operands address
// kTarget, fs/gs ones kSegOffset past the segment base (which the tests
// set to kTarget - kSegOffset), RIP-relative ones (x64) kRipTarget: within
// 2 GiB of the snippets and below 4 GiB, so that EIP-relative ones (an
// address-size prefix) reach it too.
constexpr uint64_t kTarget = 0x00201000, kSegOffset = 0x1000, kRipTarget = 0xF0000800;

// Snippets are lifted at consecutive slots from here (x64 code at an
// address above 4 GiB, like its images).
uint64_t SnippetBase(bool x64) { return x64 ? 0x140001000ull : 0x00401000; }

bool IsSegOrAddrPrefix(uint8_t b) {
  return b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
         b == 0x65 || b == 0x67;
}

// Appends raw[0, end) to *b without segment and address-size prefixes.
void CopyNoSegPrefixes(const uint8_t* raw, const ZydisDecodedInstruction& d, int end,
                       std::vector<uint8_t>* b) {
  for (int i = 0; i < end; i++)
    if (i >= d.raw.prefix_count || !IsSegOrAddrPrefix(raw[i])) b->push_back(raw[i]);
}

// Index in a copy made by CopyNoSegPrefixes of the raw byte at `off`.
int CopiedIndex(const uint8_t* raw, const ZydisDecodedInstruction& d, int off) {
  int k = off;
  for (int i = 0; i < off && i < d.raw.prefix_count; i++)
    if (IsSegOrAddrPrefix(raw[i])) k--;
  return k;
}

// Lifts each snippet (bytes, then ret) at its own `slot`-byte slot of a
// scratch program from SnippetBase(x64). Returns the C++ (prologue,
// declarations, functions) and each snippet's address in *vas.
std::string LiftSnippets(const Decoder& dec, const char* prologue,
                         const std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>>& code,
                         std::vector<uint64_t>* vas, bool x64 = false) {
  Program q;
  q.x64 = x64;
  if (!q.mem.Reserve(x64 ? 36 : 32)) return {};
  const uint64_t base = SnippetBase(x64);
  uint32_t size = 0x1000;
  for (const auto& [bytes, slot] : code) size += slot;
  q.mem.Commit(base, size, recomp::Prot::RWX);
  Module& m = q.modules.emplace_back();
  m.text.emplace_back(base, base + size);
  uint64_t va = base;
  for (const auto& [bytes, slot] : code) {
    for (size_t i = 0; i < bytes->size(); i++) q.mem.write<uint8_t>(va + i, (*bytes)[i]);
    q.mem.write<uint8_t>(va + bytes->size(), 0xC3);  // ret
    q.entries.insert(va);
    vas->push_back(va);
    va += slot;
  }
  Emitter em(q, dec);
  std::string body;
  std::set<uint64_t> refs;
  EmitStats st;
  for (uint64_t sva : *vas) {
    Function f;
    BuildFunction(q, dec, sva, &f);
    em.EmitFunction(f, &body, &refs, &st);
  }
  std::string o = prologue;
  char buf[64];
  for (uint64_t sva : *vas) {
    std::snprintf(buf, sizeof buf, "void f_%08llX(::recomp::Ctx& c);\n",
                  (unsigned long long)sva);
    o += buf;
  }
  return o + body + "}  // namespace recomp::gen\n\n";
}

// "0x12,0x34," and a disassembly safe inside a C string literal.
std::string HexBytes(const std::vector<uint8_t>& bytes) {
  std::string s;
  char h[8];
  for (uint8_t x : bytes) {
    std::snprintf(h, sizeof h, "0x%02X,", x);
    s += h;
  }
  return s;
}
std::string Literal(std::string text) {
  for (char& ch : text)
    if (ch == '"' || ch == '\\') ch = '\'';
  return text;
}

int WriteFile(const std::string& path, const std::string& s) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return 1;
  std::fwrite(s.data(), 1, s.size(), fp);
  std::fclose(fp);
  return 0;
}

// GPR number (0..7, 0..15 for x64 code) behind a register of any width,
// or -1.
int GprNum(ZydisRegister r) {
  ZydisRegister l = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
  if (l >= ZYDIS_REGISTER_EAX && l <= ZYDIS_REGISTER_EDI) return int(l - ZYDIS_REGISTER_EAX);
  l = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
  if (l >= ZYDIS_REGISTER_RAX && l <= ZYDIS_REGISTER_R15) return int(l - ZYDIS_REGISTER_RAX);
  return -1;
}

// A form's memory operand as the test aims it at its buffer: it sets GPRs
// base and index (-1: none) so that base + index * scale + disp is
// kTarget, or kSegOffset for an fs/gs operand (seg); an absolute operand
// holds the address itself. rip: offset of a RIP-relative operand's disp32
// in the native bytes (the test points it at a buffer of its own).
struct MemRef {
  bool mem = false;
  int base = -1, index = -1, scale = 0;
  int64_t disp = 0;
  bool seg = false;
  int rip = -1;
};

void PutLE(std::vector<uint8_t>* b, size_t off, uint64_t v, int n) {
  for (int i = 0; i < n; i++) (*b)[off + i] = uint8_t(v >> (8 * i));
}

// The instruction with its memory operand addressed as written, re-aimed
// at the test's buffer: *lifted is lifted, *native runs natively. They
// differ where 64-bit mode reads the bytes otherwise: fs: becomes gs: (the
// test sets gs's base natively; fs's belongs to the C library), a 32-bit
// absolute ModRM operand takes the SIB form (without it, 64-bit mode reads
// RIP-relative) and a 32-bit moffs grows to 8 bytes. A 32-bit displacement
// the native run could not reach the buffer with (its registers are
// zero-extended, its sum does not wrap at 4 GiB) is replaced, as is an
// index-only one (the sum must hit the buffer exactly); a RIP- or
// EIP-relative one is set by AimRip. False when the address uses the stack
// pointer, the same register twice or a register the instruction also uses
// otherwise (`used`), or is 16-bit, or gs: in 32-bit code (the lifter has
// only fs there): the caller rewrites those to [R].
bool KeepAddress(const Insn& in, const uint8_t* raw, bool x64, uint32_t used,
                 std::vector<uint8_t>* lifted, std::vector<uint8_t>* native, MemRef* m) {
  int mi = -1;
  for (int i = 0; i < in.d.operand_count_visible; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) mi = i;
  if (mi < 0 || (!x64 && in.d.address_width != 32)) return false;
  const auto& o = in.ops[mi].mem;
  const bool fs = o.segment == ZYDIS_REGISTER_FS, gs = o.segment == ZYDIS_REGISTER_GS;
  const bool rip = o.base == ZYDIS_REGISTER_RIP || o.base == ZYDIS_REGISTER_EIP;
  if (gs && !x64) return false;
  const int b = o.base == ZYDIS_REGISTER_NONE || rip ? -1 : GprNum(o.base);
  const int x = o.index == ZYDIS_REGISTER_NONE ? -1 : GprNum(o.index);
  if ((b < 0 && o.base != ZYDIS_REGISTER_NONE && !rip) ||
      (x < 0 && o.index != ZYDIS_REGISTER_NONE))
    return false;
  if (b == 4 || x == 4 || (b >= 0 && b == x)) return false;
  if ((b >= 0 && (used >> b & 1)) || (x >= 0 && (used >> x & 1))) return false;
  const int doff = in.d.raw.disp.offset, dsize = in.d.raw.disp.size / 8;
  int64_t disp = o.disp.value;
  if (rip)
    disp = 0;
  else if (b < 0 && x < 0)
    disp = int64_t(fs || gs ? kSegOffset : kTarget);
  else if (!x64 && (b < 0 || disp < -0x800 || disp > 0x800))
    disp &= 0x7F0;
  else if (b < 0)
    disp &= ~int64_t(15);
  lifted->assign(raw, raw + in.d.length);
  if (dsize) PutLE(lifted, doff, uint64_t(disp), dsize);
  *native = *lifted;
  if (fs)
    for (int i = 0; i < in.d.raw.prefix_count; i++)
      if ((*native)[i] == 0x64) (*native)[i] = 0x65;
  if (!x64 && b < 0 && x < 0) {
    if (!(in.d.attributes & ZYDIS_ATTRIB_HAS_MODRM)) {
      native->insert(native->begin() + doff + 4, 4, uint8_t(0));  // moffs64
    } else if (!(in.d.attributes & ZYDIS_ATTRIB_HAS_SIB)) {
      const int mo = in.d.raw.modrm.offset;
      (*native)[mo] = uint8_t(((*native)[mo] & 0xF8) | 4);
      native->insert(native->begin() + mo + 1, uint8_t(0x25));  // no base, no index
    }
  }
  if (native->size() > 16) return false;
  m->mem = true;
  m->base = b;
  m->index = x;
  m->scale = x >= 0 ? o.scale : 0;
  m->disp = disp;
  m->seg = fs || gs;
  m->rip = rip ? doff : -1;
  return true;
}

// String instructions and XLAT address memory through esi/edi and ebx: the
// instruction as it is (*lifted; *native with fs: as gs:, as above) where
// the native run addresses the same memory: in x64 code always, in 32-bit
// code without gs: (unlifted) and without an address-size prefix, which
// means 16-bit addressing there and 32-bit in 64-bit mode; those agree
// only for an operand relative to a segment base (lods and xlat with fs:,
// whose address the test keeps small). *seg: the source has fs: or gs:.
bool KeepImplicit(const Insn& in, const uint8_t* raw, bool x64, std::vector<uint8_t>* lifted,
                  std::vector<uint8_t>* native, bool* seg) {
  ZydisRegister s = ZYDIS_REGISTER_NONE;
  for (int i = 0; i < in.d.operand_count; i++) {
    const auto& o = in.ops[i];
    if (o.type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
    ZydisRegister b = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, o.mem.base);
    if (b == ZYDIS_REGISTER_RSI || b == ZYDIS_REGISTER_RBX) s = o.mem.segment;
  }
  const bool fs = s == ZYDIS_REGISTER_FS, gs = s == ZYDIS_REGISTER_GS;
  if (!x64) {
    if (gs) return false;
    const bool lods_xlat = in.d.mnemonic == ZYDIS_MNEMONIC_XLAT ||
                           in.d.mnemonic == ZYDIS_MNEMONIC_LODSB ||
                           in.d.mnemonic == ZYDIS_MNEMONIC_LODSW ||
                           in.d.mnemonic == ZYDIS_MNEMONIC_LODSD;
    if (in.d.address_width == 16 && !(fs && lods_xlat)) return false;
  }
  lifted->assign(raw, raw + in.d.length);
  *native = *lifted;
  for (int i = 0; i < in.d.raw.prefix_count; i++)
    if ((*native)[i] == 0x64) (*native)[i] = 0x65;
  *seg = fs || gs;
  return true;
}

// A RIP- or EIP-relative operand of a snippet lifted at `va` pointed at
// kRipTarget.
void AimRip(const MemRef& m, uint64_t va, std::vector<uint8_t>* lifted) {
  if (m.rip >= 0) PutLE(lifted, m.rip, kRipTarget - (va + lifted->size()), 4);
}

// The addressing shape of a kept operand: each new one adds a form.
std::string Shape(const Insn& in, const MemRef& m) {
  int bits = 0;
  for (int i = 0; i < in.d.operand_count_visible; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) bits = in.ops[i].size;
  char s[96];
  std::snprintf(s, sizeof s, "%d %d %d %d %d %d %d %d %d %d", m.base >= 0, m.index >= 0, m.scale,
                in.d.raw.disp.size, m.seg, m.rip >= 0, in.d.raw.modrm.mod,
                bool(in.d.attributes & ZYDIS_ATTRIB_HAS_SIB), bits, in.d.address_width);
  return s;
}

// A form line's FormMem (tests/x86/lift_form_mem.h).
std::string MemFields(const MemRef& m) {
  char s[128];
  std::snprintf(s, sizeof s, "{%d, %d, %d, %d, %lld, %d, %d}", int(m.mem), m.base, m.index,
                m.scale, (long long)m.disp, int(m.seg), m.rip);
  return s;
}

// The tests' buffer, for the generated file.
std::string Layout() {
  char s[256];
  std::snprintf(s, sizeof s,
                "extern const uint64_t kFormTarget = 0x%llX, kFormSegOffset = 0x%llX,\n"
                "    kFormRipTarget = 0x%llX;\n",
                (unsigned long long)kTarget, (unsigned long long)kSegOffset,
                (unsigned long long)kRipTarget);
  return s;
}

// An instruction to make a form of: decoded, with its raw bytes.
struct Sample {
  const Insn* in;
  const uint8_t* raw;
};

struct Form {
  std::vector<uint8_t> lifted, bytes;  // lifted; run natively
  std::vector<uint8_t> seq;            // [eax] rewrite, for sequences
  std::string text, seq_text;
  int mem_bytes = 0;
  bool writes_mem = false;
  MemRef m;
  uint64_t example_va = 0;
};

int WriteX87FormTest(const Decoder& dec, bool x64, const std::vector<Sample>& samples,
                     const std::string& out, const char* prologue) {
  std::map<std::vector<uint8_t>, Form> forms;  // by the [eax] rewrite
  std::set<std::string> shapes;
  for (const Sample& sm : samples) {
    const Insn& in = *sm.in;
    const auto mn = in.d.mnemonic;
    // FXSAVE and FXRSTOR (SSE in Zydis' categories) read and write the
    // x87 state too, and lifted x87 runs must end at them.
    const bool fx = mn == ZYDIS_MNEMONIC_FXSAVE || mn == ZYDIS_MNEMONIC_FXRSTOR ||
                    mn == ZYDIS_MNEMONIC_FXSAVE64 || mn == ZYDIS_MNEMONIC_FXRSTOR64;
    if (!in.ok || (in.d.meta.category != ZYDIS_CATEGORY_X87_ALU && !fx)) continue;
    // Environment images differ between 32- and 64-bit mode (pointer
    // fields), so they are not comparable natively.
    if (mn == ZYDIS_MNEMONIC_FNSTENV || mn == ZYDIS_MNEMONIC_FLDENV ||
        mn == ZYDIS_MNEMONIC_FNSAVE || mn == ZYDIS_MNEMONIC_FRSTOR)
      continue;
    const uint8_t* raw = sm.raw;
    std::vector<uint8_t> b;
    int mem_bytes = 0;
    bool has_modrm = in.d.attributes & ZYDIS_ATTRIB_HAS_MODRM;
    int modrm_off = has_modrm ? in.d.raw.modrm.offset : in.d.length;
    CopyNoSegPrefixes(raw, in.d, modrm_off, &b);
    if (has_modrm) {
      if (in.d.raw.modrm.mod == 3) {
        b.push_back(raw[modrm_off]);
      } else {
        // x64: [rax] without SIB, so REX.X and REX.B go.
        if (in.d.attributes & ZYDIS_ATTRIB_HAS_REX)
          b[CopiedIndex(raw, in.d, in.d.raw.rex.offset)] &= 0xFC;
        b.push_back(uint8_t((in.d.raw.modrm.reg << 3) | 0));  // [eax]
        for (int i = 0; i < in.d.operand_count_visible; i++)
          if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) mem_bytes = in.ops[i].size / 8;
      }
    }
    Form fm;
    fm.seq = b;
    fm.text = dec.Format(in);
    Insn rewritten;
    fm.seq_text = dec.DecodeRaw(b.data(), b.size(), in.va, &rewritten) ? dec.Format(rewritten)
                                                                        : fm.text;
    fm.mem_bytes = mem_bytes;
    for (int i = 0; i < in.d.operand_count; i++)
      if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
          (in.ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
        fm.writes_mem = true;
    fm.example_va = in.va;
    // x87 instructions use no general register but ax (fnstsw ax, which
    // has no memory operand). The test sets the first eight, and has no
    // buffer for RIP-relative operands.
    bool kept = mem_bytes && KeepAddress(in, raw, x64, 0, &fm.lifted, &fm.bytes, &fm.m) &&
                fm.m.base < 8 && fm.m.index < 8 && fm.m.rip < 0;
    if (!forms.count(b)) {
      if (!kept) {
        fm.lifted = fm.bytes = b;
        fm.m = {};
        fm.m.mem = mem_bytes > 0;
        fm.m.base = mem_bytes ? 0 : -1;
      }
      if (kept) shapes.insert(Shape(in, fm.m));
      forms[b] = fm;
    } else if (kept && shapes.insert(Shape(in, fm.m)).second && !forms.count(fm.lifted)) {
      forms[fm.lifted] = fm;
    }
  }

  // Sequences: random straight-line runs of forms ([eax] rewrites), lifted
  // as one block, so the lifter's x87 runs (locals, one guard, write-back)
  // are checked against the CPU too. Transcendentals (1-ulp differences
  // would compound), fldcw (could unmask exceptions, which trap natively)
  // and the FCOMI family (C1: Intel clears it, AMD keeps it; the test
  // leaves C1 out for these forms alone) stay out, as does FXRSTOR but
  // where its image is intact (below): it loads the control word too.
  std::vector<const Form*> pool, fxrstors;
  for (const auto& [k, fm] : forms) {
    const std::string& t = fm.text;
    bool skip = k != fm.seq;  // an extra addressing shape
    if (!skip && t.rfind("fxrstor", 0) == 0) fxrstors.push_back(&fm);
    for (const char* n : {"fldcw", "fsin", "fcos", "fptan", "fpatan", "f2xm1", "fyl2x", "fcomi",
                          "fucomi", "fxrstor"})
      if (t.rfind(n, 0) == 0) skip = true;
    if (!skip) pool.push_back(&fm);
  }
  std::vector<Form> seqs;
  std::mt19937 rng(20261011);
  // fnstsw ax rewrites the base of the [eax] operands: last only.
  auto pick = [&](bool last, bool keep_mem) {
    const Form* fm;
    do {
      fm = pool[rng() % pool.size()];
    } while ((!last && fm->text.rfind("fnstsw ax", 0) == 0) || (keep_mem && fm->writes_mem));
    return fm;
  };
  auto add = [&](const std::vector<const Form*>& run) {
    Form sq;
    for (size_t i = 0; i < run.size(); i++) {
      sq.bytes.insert(sq.bytes.end(), run[i]->seq.begin(), run[i]->seq.end());
      sq.text += (i ? "; " : "") + run[i]->seq_text;
      sq.mem_bytes = std::max(sq.mem_bytes, run[i]->mem_bytes);
    }
    sq.lifted = sq.bytes;
    sq.m.mem = sq.mem_bytes > 0;
    sq.m.base = sq.m.mem ? 0 : -1;
    seqs.push_back(sq);
  };
  for (int n = 0; n < 4000 && !pool.empty(); n++) {
    std::vector<const Form*> run;
    int len = 3 + int(rng() % 8);
    for (int i = 0; i < len; i++) run.push_back(pick(i + 1 == len, false));
    add(run);
  }
  // FXRSTOR inside a run, after forms that leave memory (the image the
  // test gives it) alone.
  for (int n = 0; n < 200 && !pool.empty() && !fxrstors.empty(); n++) {
    std::vector<const Form*> run;
    for (int i = int(rng() % 4); i >= 0; i--) run.push_back(pick(false, true));
    run.push_back(fxrstors[rng() % fxrstors.size()]);
    for (int i = int(rng() % 4); i >= 0; i--) run.push_back(pick(i == 0, false));
    add(run);
  }

  // Forms in 16-byte slots, sequences in 128-byte ones.
  std::vector<const Form*> all;
  std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> code;
  for (const auto& [k, fm] : forms) {
    all.push_back(&fm);
    code.push_back({&fm.lifted, 16});
  }
  for (const auto& fm : seqs) {
    all.push_back(&fm);
    code.push_back({&fm.lifted, 128});
  }
  std::vector<uint64_t> vas;
  std::string o = LiftSnippets(dec, prologue, code, &vas, x64);
  if (o.empty()) return 1;
  o += "#include \"lift_x87_forms.h\"\n" + Layout() + "extern const X87Form kX87Forms[] = {\n";
  for (size_t i = 0; i < all.size(); i++) {
    const Form& fm = *all[i];
    char buf[2048];
    std::snprintf(buf, sizeof buf,
                  "  {&recomp::gen::f_%08llX, {%s}, %zu, %d, %s, \"%s\", 0x%08llX},\n",
                  (unsigned long long)vas[i], HexBytes(fm.bytes).c_str(), fm.bytes.size(),
                  fm.mem_bytes, MemFields(fm.m).c_str(), Literal(fm.text).c_str(),
                  (unsigned long long)fm.example_va);
    o += buf;
  }
  o += "};\nextern const int kX87FormCount = " + std::to_string(all.size()) + ";\n";
  if (WriteFile(out, o)) return 1;
  std::printf("x87 forms: %zu (%zu sequences) -> %s\n", all.size(), seqs.size(),
              out.c_str());
  return 0;
}

// The instructions of every function of p.
std::vector<Sample> ProgramSamples(const Program& p, const Decoder& dec,
                                   std::vector<Function>* fns) {
  for (uint64_t e : p.entries) BuildFunction(p, dec, e, &fns->emplace_back());
  std::vector<Sample> samples;
  for (const Function& f : *fns)
    for (const auto& [va, in] : f.insns) samples.push_back({&in, p.Bytes(va)});
  return samples;
}

}  // namespace

int WriteX87Test(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue) {
  std::vector<Function> fns;
  return WriteX87FormTest(dec, p.x64, ProgramSamples(p, dec, &fns), out, prologue);
}

// ---- integer forms ---------------------------------------------------------
//
// Same idea for integer instructions, memory operands kept or rewritten
// to [R] as above; the test points the address registers at its buffer,
// so every register value is identical in both runs. Excluded: stack and
// control flow, esp operands, segment registers, and opcodes that mean
// something else in 64-bit mode.

namespace {

bool BadOperands(const Insn& in);

bool Excluded(const Insn& in) {
  // push and pop run on a stack of the test's; in 32-bit code only their
  // 16-bit forms (the others move 8 bytes in 64-bit mode). The flags and
  // all-register forms stay out.
  if (in.d.meta.category == ZYDIS_CATEGORY_PUSH || in.d.meta.category == ZYDIS_CATEGORY_POP) {
    switch (in.d.mnemonic) {
      case ZYDIS_MNEMONIC_PUSHF: case ZYDIS_MNEMONIC_PUSHFD: case ZYDIS_MNEMONIC_PUSHFQ:
      case ZYDIS_MNEMONIC_POPF: case ZYDIS_MNEMONIC_POPFD: case ZYDIS_MNEMONIC_POPFQ:
      case ZYDIS_MNEMONIC_PUSHA: case ZYDIS_MNEMONIC_PUSHAD: case ZYDIS_MNEMONIC_POPA:
      case ZYDIS_MNEMONIC_POPAD:
        return true;
      default:
        break;
    }
    if (in.d.machine_mode != ZYDIS_MACHINE_MODE_LONG_64 && in.d.operand_width != 16)
      return true;
    return BadOperands(in);
  }
  // SSE/MMX forms (some are DATAXFER/LOGICAL, not the SSE category) and
  // FCMOVcc (CMOV category): the integer test compares only general
  // registers and memory.
  for (int i = 0; i < in.d.operand_count; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER) {
      ZydisRegisterClass rc = ZydisRegisterGetClass(in.ops[i].reg.value);
      if (rc == ZYDIS_REGCLASS_XMM || rc == ZYDIS_REGCLASS_YMM ||
          rc == ZYDIS_REGCLASS_MMX || rc == ZYDIS_REGCLASS_X87)
        return true;
    }
  // SSE/SSE2 forms without an XMM operand (CVTTSD2SI r32, m64) too: they
  // belong to the SSE test.
  switch (in.d.meta.isa_set) {
    case ZYDIS_ISA_SET_SSE: case ZYDIS_ISA_SET_SSE2:
    case ZYDIS_ISA_SET_SSEMXCSR: case ZYDIS_ISA_SET_SSE_PREFETCH:
    case ZYDIS_ISA_SET_SSE3: case ZYDIS_ISA_SET_SSSE3: case ZYDIS_ISA_SET_SSE4:
    case ZYDIS_ISA_SET_SSE42: case ZYDIS_ISA_SET_AES: case ZYDIS_ISA_SET_AVXAES:
    case ZYDIS_ISA_SET_PCLMULQDQ: case ZYDIS_ISA_SET_F16C: case ZYDIS_ISA_SET_AVX:
      return true;
    default:
      break;
  }
  switch (in.d.meta.category) {
    case ZYDIS_CATEGORY_X87_ALU: case ZYDIS_CATEGORY_COND_BR:
    case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL:
    case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_SYSTEM: case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_IO: case ZYDIS_CATEGORY_IOSTRINGOP:
    case ZYDIS_CATEGORY_SEGOP: case ZYDIS_CATEGORY_MMX:
    case ZYDIS_CATEGORY_SSE:
      return true;
    case ZYDIS_CATEGORY_FLAGOP:  // only these are comparable natively
      switch (in.d.mnemonic) {
        case ZYDIS_MNEMONIC_SAHF: case ZYDIS_MNEMONIC_LAHF: case ZYDIS_MNEMONIC_CLC:
        case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CMC: case ZYDIS_MNEMONIC_CLD:
        case ZYDIS_MNEMONIC_STD:
          return false;
        default:
          return true;
      }
    default:
      break;
  }
  switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_CPUID: case ZYDIS_MNEMONIC_RDTSC: case ZYDIS_MNEMONIC_LEAVE:
    case ZYDIS_MNEMONIC_ENTER: case ZYDIS_MNEMONIC_DAA: case ZYDIS_MNEMONIC_DAS:
    case ZYDIS_MNEMONIC_AAA: case ZYDIS_MNEMONIC_AAS: case ZYDIS_MNEMONIC_AAM:
    case ZYDIS_MNEMONIC_AAD: case ZYDIS_MNEMONIC_BOUND: case ZYDIS_MNEMONIC_ARPL:
    case ZYDIS_MNEMONIC_LDS: case ZYDIS_MNEMONIC_LES: case ZYDIS_MNEMONIC_SALC:
    case ZYDIS_MNEMONIC_PUSHAD: case ZYDIS_MNEMONIC_POPAD: case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
    case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_JRCXZ:
    case ZYDIS_MNEMONIC_FWAIT: case ZYDIS_MNEMONIC_XGETBV:
      return true;
    default:
      break;
  }
  if (in.d.opcode == 0x82 && in.d.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT) return true;
  return BadOperands(in);
}

// Operands neither run can reproduce: esp (but push's and pop's own),
// eip, segment registers, and lea forms that would change meaning in
// 64-bit mode.
bool BadOperands(const Insn& in) {
  const bool stack_op = in.d.meta.category == ZYDIS_CATEGORY_PUSH ||
                        in.d.meta.category == ZYDIS_CATEGORY_POP;
  for (int i = 0; i < in.d.operand_count; i++) {
    const auto& o = in.ops[i];
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
      ZydisRegister r = o.reg.value;
      ZydisRegister l = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
      if (stack_op && l == ZYDIS_REGISTER_RSP && o.visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN)
        continue;
      if (l == ZYDIS_REGISTER_RSP || l == ZYDIS_REGISTER_RIP ||
          ZydisRegisterGetClass(r) == ZYDIS_REGCLASS_SEGMENT)
        return true;
    }
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
      // lea computes an address; it keeps its original operand, which must
      // not involve esp, nor be absolute (RIP-relative in 64-bit mode).
      if (in.d.mnemonic == ZYDIS_MNEMONIC_LEA &&
          (o.mem.base == ZYDIS_REGISTER_ESP || o.mem.index == ZYDIS_REGISTER_ESP ||
           o.mem.base == ZYDIS_REGISTER_RSP || o.mem.index == ZYDIS_REGISTER_RSP ||
           o.mem.base == ZYDIS_REGISTER_RIP ||
           (o.mem.base == ZYDIS_REGISTER_NONE && o.mem.index == ZYDIS_REGISTER_NONE)))
        return true;
    }
  }
  return false;
}

// SSE/SSE2 forms for tests/x86/lift_sse_test.cpp: XMM-register forms only
// (no MMX), without ldmxcsr (random memory would #GP or unmask) and
// prefetches (no effect to compare).
bool SseCandidate(const Insn& in) {
  switch (in.d.meta.isa_set) {
    case ZYDIS_ISA_SET_SSE: case ZYDIS_ISA_SET_SSE2: case ZYDIS_ISA_SET_SSEMXCSR:
    case ZYDIS_ISA_SET_SSE3: case ZYDIS_ISA_SET_SSSE3: case ZYDIS_ISA_SET_SSE4:
    case ZYDIS_ISA_SET_SSE42: case ZYDIS_ISA_SET_AES: case ZYDIS_ISA_SET_AVXAES:
    case ZYDIS_ISA_SET_PCLMULQDQ: case ZYDIS_ISA_SET_F16C: case ZYDIS_ISA_SET_AVX:
      break;
    default:
      return false;
  }
  if (in.d.mnemonic == ZYDIS_MNEMONIC_LDMXCSR || in.d.mnemonic == ZYDIS_MNEMONIC_VLDMXCSR)
    return false;
  // Approximate: no host result to match (tests/x86/sse_rsqrt_test instead).
  switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_RSQRTPS: case ZYDIS_MNEMONIC_RSQRTSS: case ZYDIS_MNEMONIC_VRSQRTPS:
    case ZYDIS_MNEMONIC_VRSQRTSS: case ZYDIS_MNEMONIC_RCPPS: case ZYDIS_MNEMONIC_RCPSS:
    case ZYDIS_MNEMONIC_VRCPPS: case ZYDIS_MNEMONIC_VRCPSS:
      return false;
    default:
      break;
  }
  for (int i = 0; i < in.d.operand_count; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        ZydisRegisterGetClass(in.ops[i].reg.value) == ZYDIS_REGCLASS_MMX)
      return false;
  return !BadOperands(in);
}

// The integer and SSE tests share the form extraction: each distinct
// instruction (by its [base] rewrite), its memory operand kept where the
// test can run it as written, and each further addressing shape.
int WriteFormTest(const Decoder& dec, bool x64, const std::vector<Sample>& samples,
                  const std::string& out, const char* prologue, bool sse) {
  struct IForm {
    std::vector<uint8_t> lifted, bytes;  // lifted; run natively
    std::string text;
    MemRef m;
    uint32_t undef = 0;
    uint64_t example_va = 0;
  };
  std::map<std::vector<uint8_t>, IForm> forms;
  std::set<std::string> shapes;
  {
    for (const Sample& sm : samples) {
      const Insn& in = *sm.in;
      const uint64_t va = in.va;
      if (!in.ok || (sse ? !SseCandidate(in) : Excluded(in))) continue;
      const uint8_t* raw = sm.raw;
      std::vector<uint8_t> b;
      int base = -1;
      bool has_modrm = in.d.attributes & ZYDIS_ATTRIB_HAS_MODRM;
      bool mem = false;
      for (int i = 0; i < in.d.operand_count_visible; i++)
        if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            in.d.mnemonic != ZYDIS_MNEMONIC_LEA)
          mem = true;
      // Registers the instruction uses in any role (explicit or implicit).
      uint32_t used = 0;
      for (int i = 0; i < in.d.operand_count; i++)
        if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER) {
          int n = GprNum(in.ops[i].reg.value);
          if (n >= 0) used |= 1u << n;
        }
      bool string_op = in.d.meta.category == ZYDIS_CATEGORY_STRINGOP ||
                       in.d.mnemonic == ZYDIS_MNEMONIC_XLAT;
      bool moffs = !has_modrm && in.d.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT &&
                   in.d.opcode >= 0xA0 && in.d.opcode <= 0xA3;
      if (moffs) {
        // mov al/eax, [moffs32] and back: in 64-bit mode A0-A3 take an
        // 8-byte address, so use the ModRM form (8A/8B/88/89) on [base].
        static const uint8_t op[] = {0x8A, 0x8B, 0x88, 0x89};
        for (int r : {3, 6, 7, 1, 2})
          if (!(used & (1u << r))) {
            base = r;
            break;
          }
        CopyNoSegPrefixes(raw, in.d, in.d.raw.prefix_count, &b);
        if (in.d.attributes & ZYDIS_ATTRIB_HAS_REX)
          b[CopiedIndex(raw, in.d, in.d.raw.rex.offset)] &= 0xFC;
        b.push_back(op[in.d.opcode - 0xA0]);
        b.push_back(uint8_t((0 << 3) | base));  // reg = al/ax/eax
      } else if (string_op) {
        // Implicit [esi]/[edi], [ebx + al]: the test points them into the
        // buffer.
        CopyNoSegPrefixes(raw, in.d, in.d.length, &b);
      } else if (has_modrm && mem) {
        int reg = in.d.raw.modrm.reg;
        for (int r : {3, 6, 7, 1, 2, 0})
          if (r != reg && !(used & (1u << r))) {
            base = r;
            break;
          }
        if (base < 0) continue;
        CopyNoSegPrefixes(raw, in.d, in.d.raw.modrm.offset, &b);
        // x64: [base] without SIB, so REX.X and REX.B go (REX.R keeps the
        // reg field's register); in a 3-byte VEX prefix they are inverted.
        if (in.d.attributes & ZYDIS_ATTRIB_HAS_REX)
          b[CopiedIndex(raw, in.d, in.d.raw.rex.offset)] &= 0xFC;
        if ((in.d.attributes & ZYDIS_ATTRIB_HAS_VEX) && in.d.raw.vex.size == 3)
          b[CopiedIndex(raw, in.d, in.d.raw.vex.offset) + 1] |= 0x60;
        b.push_back(uint8_t((reg << 3) | base));
        // Immediates follow ModRM/SIB/displacement: keep them.
        int imm_off = in.d.raw.imm[0].size ? in.d.raw.imm[0].offset : in.d.length;
        for (int i = imm_off; i < in.d.length; i++) b.push_back(raw[i]);
      } else {
        CopyNoSegPrefixes(raw, in.d, in.d.length, &b);
        // inc/dec r32 (40-4F) are REX prefixes in 64-bit mode: use FF /0, /1.
        if (!x64 && b.size() == 1 && b[0] >= 0x40 && b[0] <= 0x4F)
          b = {0xFF, uint8_t(0xC0 + (b[0] >= 0x48 ? 8 : 0) + (b[0] & 7))};
        else if (!x64 && b.size() == 2 && b[0] == 0x66 && b[1] >= 0x40 && b[1] <= 0x4F)
          b = {0x66, 0xFF, uint8_t(0xC0 + (b[1] >= 0x48 ? 8 : 0) + (b[1] & 7))};
      }
      IForm fm;
      fm.text = dec.Format(in);
      fm.undef = in.d.cpu_flags ? in.d.cpu_flags->undefined : 0;
      fm.example_va = va;
      bool kept = string_op ? KeepImplicit(in, raw, x64, &fm.lifted, &fm.bytes, &fm.m.seg)
                            : (moffs || (has_modrm && mem)) &&
                                  KeepAddress(in, raw, x64, used, &fm.lifted, &fm.bytes, &fm.m);
      // A string instruction or XLAT kept with its prefixes is a form of
      // its own.
      const std::vector<uint8_t> key = kept && string_op ? fm.lifted : b;
      if (!forms.count(key)) {
        if (kept) {
          shapes.insert(Shape(in, fm.m));
        } else {
          fm.lifted = fm.bytes = b;
          fm.m.mem = base >= 0;
          fm.m.base = base;
        }
        forms[key] = fm;
      } else if (kept && shapes.insert(Shape(in, fm.m)).second && !forms.count(fm.lifted)) {
        forms[fm.lifted] = fm;
      }
    }
  }

  // 32-byte slots: the snippet at slot i is lifted at SnippetBase + 32 * i.
  std::vector<const IForm*> all;
  std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> code;
  for (auto& [k, fm] : forms) {
    AimRip(fm.m, SnippetBase(x64) + 32 * all.size(), &fm.lifted);
    all.push_back(&fm);
    code.push_back({&fm.lifted, 32});
  }
  std::vector<uint64_t> vas;
  std::string o = LiftSnippets(dec, prologue, code, &vas, x64);
  if (o.empty()) return 1;
  const std::string kind = sse ? "Sse" : "Int";
  o += std::string("#include \"lift_") + (sse ? "sse" : "int") + "_forms.h\"\n" + Layout() +
       "extern const " + kind + "Form k" + kind + "Forms[] = {\n";
  for (size_t i = 0; i < all.size(); i++) {
    const IForm& fm = *all[i];
    char buf[640];
    std::snprintf(buf, sizeof buf,
                  "  {&recomp::gen::f_%08llX, {%s}, %zu, %s, 0x%X, \"%s\", 0x%08llX},\n",
                  (unsigned long long)vas[i], HexBytes(fm.bytes).c_str(), fm.bytes.size(),
                  MemFields(fm.m).c_str(), fm.undef,
                  Literal(fm.text).c_str(), (unsigned long long)fm.example_va);
    o += buf;
  }
  o += "};\nextern const int k" + kind + "FormCount = " + std::to_string(all.size()) +
       ";\n";
  if (WriteFile(out, o)) return 1;
  std::printf("%s forms: %zu -> %s\n", sse ? "SSE" : "integer", all.size(), out.c_str());
  return 0;
}

int WriteProgramFormTest(const Program& p, const Decoder& dec, const std::string& out,
                         const char* prologue, bool sse) {
  std::vector<Function> fns;
  return WriteFormTest(dec, p.x64, ProgramSamples(p, dec, &fns), out, prologue, sse);
}

}  // namespace

int WriteIntTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue) {
  return WriteProgramFormTest(p, dec, out, prologue, false);
}

int WriteSseTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue) {
  return WriteProgramFormTest(p, dec, out, prologue, true);
}

int WriteFormListTest(const std::string& path, bool code, bool x64, const Decoder& dec,
                      const std::string& out, const char* prologue, FormKind kind) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 1;
  }
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<Insn> insns;
  std::vector<Sample> samples;
  if (code) {
    // Every instruction from the first byte to the last, at the offset it
    // has in the file.
    bytes.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const std::vector<uint8_t>& c = bytes[0];
    insns.reserve(c.size());
    for (size_t off = 0; off < c.size(); off += insns.back().d.length) {
      if (!dec.DecodeRaw(c.data() + off, c.size() - off, off, &insns.emplace_back())) {
        std::fprintf(stderr, "%s: no instruction at offset 0x%zX\n", path.c_str(), off);
        return 1;
      }
      samples.push_back({&insns.back(), c.data() + off});
    }
  } else {
    std::string line;
    int no = 0;
    while (std::getline(f, line)) {
      no++;
      line = line.substr(0, line.find('#'));
      std::vector<uint8_t> b;
      for (size_t i = 0; i < line.size();) {
        if (std::isspace(uint8_t(line[i]))) {
          i++;
          continue;
        }
        if (i + 1 >= line.size() || !std::isxdigit(uint8_t(line[i])) ||
            !std::isxdigit(uint8_t(line[i + 1]))) {
          std::fprintf(stderr, "%s:%d: expected hex bytes\n", path.c_str(), no);
          return 1;
        }
        b.push_back(uint8_t(std::stoul(line.substr(i, 2), nullptr, 16)));
        i += 2;
      }
      if (!b.empty()) bytes.push_back(b);
    }
    // Each encoding at its own (fake) address, so example_va tells them apart.
    insns.resize(bytes.size());
    for (size_t i = 0; i < bytes.size(); i++) {
      if (!dec.DecodeRaw(bytes[i].data(), bytes[i].size(), 0x1000 + 0x20 * i, &insns[i]) ||
          insns[i].d.length != bytes[i].size()) {
        std::fprintf(stderr, "%s: entry %zu does not decode as one instruction\n",
                     path.c_str(), i + 1);
        return 1;
      }
      samples.push_back({&insns[i], bytes[i].data()});
    }
  }
  if (kind == FormKind::X87) return WriteX87FormTest(dec, x64, samples, out, prologue);
  return WriteFormTest(dec, x64, samples, out, prologue, kind == FormKind::Sse);
}

}  // namespace lifter
