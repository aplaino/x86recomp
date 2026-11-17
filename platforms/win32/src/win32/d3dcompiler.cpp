// d3dcompiler (the D3DCompiler_4x DLLs) for x64 guests: ID3DBlob,
// D3DGetBlobPart's signature parts, which games pass to CreateInputLayout,
// and D3DReflect, which games use for their constant buffers' layouts.
// A DXBC container is "DXBC", a 16-byte checksum, 1, its total size, its
// chunk count and their offsets; each chunk is a FourCC, a size and the
// data. Parts are returned as containers of their own; their checksum is
// left zero (Direct3D 11 here does not check it).

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/com.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "d3dcompiler_43.dll";
constexpr uint32_t kS_OK = 0, kE_FAIL = 0x80004005, kE_INVALIDARG = 0x80070057;

constexpr guest::GUID kIID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr guest::GUID kIID_ID3D10Blob = {
    0x8ba5fb08, 0x5195, 0x40e2, {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};

// ID3DBlob: its bytes in guest memory, for GetBufferPointer.
struct Blob : com::Object {
  gaddr data = 0;
  uint64_t size = 0;
  ~Blob() override {
    if (data) HeapFreeProcess(data);
  }
};

gaddr NewBlob(const std::vector<uint8_t>& bytes) {
  auto b = std::make_shared<Blob>();
  b->size = bytes.size();
  b->data = HeapAllocProcess(bytes.size() ? bytes.size() : 1, false);
  std::memcpy(GPtr(b->data), bytes.data(), bytes.size());
  return com::Publish(b, com::Vtable(kDll, "ID3D10Blob",
                                     {"QueryInterface", "AddRef", "Release", "GetBufferPointer",
                                      "GetBufferSize"}));
}

uint32_t FourCC(const char* s) { return uint32_t(s[0]) | s[1] << 8 | s[2] << 16 | uint32_t(s[3]) << 24; }

// The chunks of the container at p (size bytes) whose FourCC is in `want`,
// in their order; false if it is not a container.
bool Chunks(const uint8_t* p, uint64_t size, std::initializer_list<const char*> want,
            std::vector<std::pair<const uint8_t*, uint32_t>>* out) {
  if (size < 32 || std::memcmp(p, "DXBC", 4)) return false;
  uint32_t total, count;
  std::memcpy(&total, p + 24, 4);
  std::memcpy(&count, p + 28, 4);
  if (total > size || 32 + uint64_t(count) * 4 > total) return false;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t at, fourcc, len;
    std::memcpy(&at, p + 32 + 4 * i, 4);
    if (uint64_t(at) + 8 > total) return false;
    std::memcpy(&fourcc, p + at, 4);
    std::memcpy(&len, p + at + 4, 4);
    if (uint64_t(at) + 8 + len > total) return false;
    for (const char* w : want)
      if (fourcc == FourCC(w)) out->push_back({p + at, len + 8});
  }
  return true;
}

// A container of the chunks; empty if it would be bigger than `limit` (a
// part is never bigger than its source).
std::vector<uint8_t> Container(const std::vector<std::pair<const uint8_t*, uint32_t>>& chunks,
                               uint64_t limit) {
  const uint64_t head = 32 + 4 * uint64_t(chunks.size());
  uint64_t sum = head;
  for (const auto& ch : chunks) sum += ch.second;
  if (sum > limit || sum > UINT32_MAX) return {};
  const uint32_t total = uint32_t(sum);
  std::vector<uint8_t> out(total);
  std::memcpy(out.data(), "DXBC", 4);
  const uint32_t one = 1, count = uint32_t(chunks.size());
  std::memcpy(out.data() + 20, &one, 4);
  std::memcpy(out.data() + 24, &total, 4);
  std::memcpy(out.data() + 28, &count, 4);
  uint32_t at = uint32_t(head);
  for (uint32_t i = 0; i < count; i++) {
    std::memcpy(out.data() + 32 + 4 * i, &at, 4);
    std::memcpy(out.data() + at, chunks[i].first, chunks[i].second);
    at += chunks[i].second;
  }
  return out;
}

}  // namespace

#define BLOB_METHOD(name) HLE_NAMED_X(kDll, "ID3D10Blob::" #name, Blob##name, kCdecl, true)

BLOB_METHOD(QueryInterface) {
  if (com::GuidEq(Arg(c, 1), kIID_ID3D10Blob) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr64(Arg(c, 2), Arg(c, 0));
    return kS_OK;
  }
  Wr64(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}
BLOB_METHOD(AddRef) { return com::AddRef(Arg(c, 0)); }
BLOB_METHOD(Release) { return com::Release(Arg(c, 0)); }
BLOB_METHOD(GetBufferPointer) { return com::As<Blob>(Arg(c, 0))->data; }
BLOB_METHOD(GetBufferSize) { return com::As<Blob>(Arg(c, 0))->size; }

// D3DGetBlobPart(data, size, part, flags, &blob): the signature parts
// (D3D_BLOB_INPUT_SIGNATURE_BLOB 0, OUTPUT 1, INPUT_AND_OUTPUT 2, PATCH
// CONSTANT 3, ALL_SIGNATURE 4) as containers of their chunks.
HLE64(kDll, D3DGetBlobPart, 5) {
  const gaddr out = Arg(c, 4);
  Wr64(out, 0);
  const uint32_t part = uint32_t(Arg(c, 2));
  std::initializer_list<const char*> in = {"ISGN", "ISG1"}, outs = {"OSGN", "OSG1", "OSG5"},
                                     both = {"ISGN", "ISG1", "OSGN", "OSG1", "OSG5"},
                                     patch = {"PCSG", "PSG1"},
                                     all = {"ISGN", "ISG1", "OSGN", "OSG1", "OSG5", "PCSG", "PSG1"};
  std::initializer_list<const char*> want;
  switch (part) {
    case 0: want = in; break;
    case 1: want = outs; break;
    case 2: want = both; break;
    case 3: want = patch; break;
    case 4: want = all; break;
    default: FatalF("D3DGetBlobPart: part %u is not supported", part);
  }
  std::vector<std::pair<const uint8_t*, uint32_t>> chunks;
  if (!Chunks(GPtr(Arg(c, 0)), Arg(c, 1), want, &chunks)) return kE_INVALIDARG;
  std::vector<uint8_t> bytes = chunks.empty() ? std::vector<uint8_t>() : Container(chunks, Arg(c, 1));
  if (bytes.empty()) return kE_FAIL;
  Wr64(out, NewBlob(bytes));
  return kS_OK;
}

// ---- D3DReflect: ID3D11ShaderReflection ---------------------------------------------------
//
// Over a guest copy of the shader, so the names it hands out (pointers
// into its chunks) live as long as it does. RDEF: constant buffer count
// and offset, binding count and offset, target, flags, creator, then (shader
// model 5) "RD11" and its record sizes. A constant buffer record is name,
// variable count and offset, size, flags, type; a variable name, offset,
// size, flags, type, default value (then, with RD11, the texture and
// sampler ranges); a type class, type, rows, columns, elements, member
// count (16 bits each) and member offset (then, with RD11, four words and
// its name); a member name, type, offset; a binding name, type, return
// type, dimension, samples, bind point, count, flags. Offsets are from the
// chunk's data. The constant buffers, variables and types are interfaces
// without IUnknown, owned by the reflection; a lookup out of range gives
// one whose GetDesc fails, as on Windows. The structures are d3dcompiler
// 43's (no minimum precision in a signature parameter).

namespace {

constexpr guest::GUID kIID_ID3D11ShaderReflection43 = {
    0x0a233719, 0x3960, 0x4578, {0x9d, 0x7c, 0x20, 0x3b, 0x8b, 0x1d, 0x9c, 0xc1}};
constexpr guest::GUID kIID_ID3D11ShaderReflection47 = {
    0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};

uint32_t Le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t Le32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

struct ReflType : com::Object {
  bool valid = false;
  uint32_t cls = 0, type = 0, rows = 0, cols = 0, elements = 0, offset = 0;
  gaddr name = 0;
  std::vector<std::string> member_names;
  std::vector<gaddr> member_name_ptrs, members;
  ~ReflType() override {
    for (gaddr m : members) com::Release(m);
  }
};

struct ReflVariable : com::Object {
  bool valid = false;
  std::string name_s;
  gaddr name = 0, buffer = 0, type = 0, default_value = 0;
  uint32_t offset = 0, size = 0, flags = 0, tex_start = ~0u, tex_size = 0, smp_start = ~0u,
           smp_size = 0;
  ~ReflVariable() override {
    if (type) com::Release(type);
  }
};

struct ReflBuffer : com::Object {
  bool valid = false;
  std::string name_s;
  gaddr name = 0;
  uint32_t type = 0, size = 0, flags = 0;
  std::vector<gaddr> variables;
  ~ReflBuffer() override {
    for (gaddr v : variables) com::Release(v);
  }
};

struct ReflBinding {
  std::string name_s;
  gaddr name = 0;
  uint32_t type = 0, ret = 0, dim = 0, samples = 0, point = 0, count = 0, flags = 0;
};

struct ReflParam {
  gaddr name = 0;
  uint32_t index = 0, reg = 0, sv = 0, comp = 0, stream = 0;
  uint8_t mask = 0, rw = 0;
};

struct Reflection : com::Object {
  gaddr blob = 0, creator = 0;
  uint32_t version = 0, flags = 0;
  std::vector<gaddr> buffers;
  std::vector<ReflBinding> bindings;
  std::vector<ReflParam> inputs, outputs, patch;
  ~Reflection() override {
    for (gaddr b : buffers) com::Release(b);
    if (blob) HeapFreeProcess(blob);
  }
};

gaddr TypeVtable() {
  return com::Vtable(kDll, "ID3D11ShaderReflectionType",
                     {"GetDesc", "GetMemberTypeByIndex", "GetMemberTypeByName", "GetMemberTypeName",
                      "IsEqual", "GetSubType", "GetBaseClass", "GetNumInterfaces",
                      "GetInterfaceByIndex", "IsOfType", "ImplementsInterface"});
}
gaddr VariableVtable() {
  return com::Vtable(kDll, "ID3D11ShaderReflectionVariable",
                     {"GetDesc", "GetType", "GetBuffer", "GetInterfaceSlot"});
}
gaddr BufferVtable() {
  return com::Vtable(kDll, "ID3D11ShaderReflectionConstantBuffer",
                     {"GetDesc", "GetVariableByIndex", "GetVariableByName"});
}

// The objects lookups out of range return: published once, never released.
gaddr NullType() {
  static const gaddr g = com::Publish(std::make_shared<ReflType>(), TypeVtable());
  return g;
}
gaddr NullVariable() {
  static const gaddr g = com::Publish(std::make_shared<ReflVariable>(), VariableVtable());
  return g;
}
gaddr NullBuffer() {
  static const gaddr g = com::Publish(std::make_shared<ReflBuffer>(), BufferVtable());
  return g;
}

// A chunk of the shader: its data on the host, and where the guest copy
// has it.
struct ChunkView {
  const uint8_t* p = nullptr;
  uint32_t size = 0;
  gaddr guest = 0;
  bool In(uint64_t off, uint64_t n) const { return off + n <= size; }
  // The guest pointer to the NUL-terminated name at off (0 if there is
  // none), and the name.
  gaddr Str(uint32_t off, std::string* s) const {
    if (!off || off >= size) return 0;
    const void* nul = std::memchr(p + off, 0, size - off);
    if (!nul) return 0;
    if (s) s->assign(reinterpret_cast<const char*>(p + off), static_cast<const uint8_t*>(nul) - (p + off));
    return guest + off;
  }
};

gaddr NewType(const ChunkView& r, bool rd11, uint32_t off, uint32_t member_offset, int depth) {
  auto t = std::make_shared<ReflType>();
  if (depth < 16 && r.In(off, rd11 ? 36 : 16)) {
    const uint8_t* e = r.p + off;
    t->valid = true;
    t->cls = Le16(e);
    t->type = Le16(e + 2);
    t->rows = Le16(e + 4);
    t->cols = Le16(e + 6);
    t->elements = Le16(e + 8);
    t->offset = member_offset;
    if (rd11) t->name = r.Str(Le32(e + 32), nullptr);
    const uint32_t n = Le16(e + 10), members = Le32(e + 12);
    for (uint32_t i = 0; i < n && r.In(members + 12ull * i, 12); i++) {
      const uint8_t* m = r.p + members + 12 * i;
      std::string s;
      t->member_name_ptrs.push_back(r.Str(Le32(m), &s));
      t->member_names.push_back(s);
      t->members.push_back(NewType(r, rd11, Le32(m + 4), Le32(m + 8), depth + 1));
    }
  }
  return com::Publish(t, TypeVtable());
}

// The parameters of a signature chunk (as dxbc.cpp's Signature reads them).
void Params(const ChunkView& s, uint32_t fourcc, std::vector<ReflParam>* out) {
  const uint32_t stride = fourcc == FourCC("OSG5") ? 28
                          : fourcc == FourCC("ISG1") || fourcc == FourCC("OSG1") ||
                                    fourcc == FourCC("PSG1")
                              ? 32
                              : 24;
  if (!s.In(0, 8)) return;
  const uint32_t n = Le32(s.p);
  for (uint32_t i = 0; i < n && s.In(8 + uint64_t(stride) * (i + 1), 0); i++) {
    const uint8_t* e = s.p + 8 + stride * i;
    ReflParam p;
    if (stride != 24) {
      p.stream = Le32(e);
      e += 4;
    }
    p.name = s.Str(Le32(e), nullptr);
    p.index = Le32(e + 4);
    p.sv = Le32(e + 8);
    p.comp = Le32(e + 12);
    p.reg = Le32(e + 16);
    p.mask = e[20];
    p.rw = e[21];
    out->push_back(p);
  }
}

Reflection* Refl(Ctx& c) { return com::As<Reflection>(Arg(c, 0)); }

// D3D11_SIGNATURE_PARAMETER_DESC (32 bytes in d3dcompiler 43).
uint64_t WriteParam(const std::vector<ReflParam>& v, uint32_t i, gaddr d) {
  if (i >= v.size()) return kE_INVALIDARG;
  const ReflParam& p = v[i];
  std::memset(GPtr(d), 0, 32);
  Wr64(d, p.name);
  Wr32(d + 8, p.index);
  Wr32(d + 12, p.reg);
  Wr32(d + 16, p.sv);
  Wr32(d + 20, p.comp);
  Wr8(d + 24, p.mask);
  Wr8(d + 25, p.rw);
  Wr32(d + 28, p.stream);
  return kS_OK;
}

// D3D11_SHADER_INPUT_BIND_DESC (40 bytes).
void WriteBinding(const ReflBinding& b, gaddr d) {
  std::memset(GPtr(d), 0, 40);
  Wr64(d, b.name);
  Wr32(d + 8, b.type);
  Wr32(d + 12, b.point);
  Wr32(d + 16, b.count);
  Wr32(d + 20, b.flags);
  Wr32(d + 24, b.ret);
  Wr32(d + 28, b.dim);
  Wr32(d + 32, b.samples);
}

gaddr VariableNamed(const std::vector<gaddr>& vars, const std::string& name) {
  for (gaddr v : vars)
    if (com::As<ReflVariable>(v)->name_s == name) return v;
  return 0;
}

}  // namespace

// D3DReflect(data, size, riid, &reflector)
HLE64(kDll, D3DReflect, 4) {
  const gaddr out = Arg(c, 3);
  Wr64(out, 0);
  if (!com::GuidEq(Arg(c, 2), kIID_ID3D11ShaderReflection43) &&
      !com::GuidEq(Arg(c, 2), kIID_ID3D11ShaderReflection47))
    return com::kE_NOINTERFACE;
  const uint8_t* src = GPtr(Arg(c, 0));
  const uint64_t size = Arg(c, 1);
  std::vector<std::pair<const uint8_t*, uint32_t>> chunks;
  if (!Chunks(src, size, {"RDEF", "ISGN", "ISG1", "OSGN", "OSG1", "OSG5", "PCSG", "PSG1", "SHDR", "SHEX"},
              &chunks))
    return kE_INVALIDARG;
  const uint32_t total = Le32(src + 24);
  auto r = std::make_shared<Reflection>();
  r->blob = HeapAllocProcess(total, false);
  std::memcpy(GPtr(r->blob), src, total);
  for (const auto& [at, len] : chunks) {
    const uint32_t fourcc = Le32(at);
    const ChunkView v{at + 8, len - 8, r->blob + gaddr(at + 8 - src)};
    if (fourcc == FourCC("SHDR") || fourcc == FourCC("SHEX")) {
      if (v.In(0, 4)) r->version = Le32(v.p);
    } else if (fourcc == FourCC("ISGN") || fourcc == FourCC("ISG1")) {
      Params(v, fourcc, &r->inputs);
    } else if (fourcc == FourCC("PCSG") || fourcc == FourCC("PSG1")) {
      Params(v, fourcc, &r->patch);
    } else if (fourcc != FourCC("RDEF")) {
      Params(v, fourcc, &r->outputs);
    } else if (v.In(0, 28)) {
      const bool rd11 = v.In(0, 32) && Le32(v.p + 28) == FourCC("RD11");
      const uint32_t var_size = rd11 ? 40 : 24;
      r->flags = Le32(v.p + 20);
      r->creator = v.Str(Le32(v.p + 24), nullptr);
      const uint32_t ncb = Le32(v.p), cbs = Le32(v.p + 4), nb = Le32(v.p + 8), bs = Le32(v.p + 12);
      for (uint32_t i = 0; i < ncb && v.In(cbs + 24ull * i, 24); i++) {
        const uint8_t* e = v.p + cbs + 24 * i;
        auto b = std::make_shared<ReflBuffer>();
        b->valid = true;
        b->name = v.Str(Le32(e), &b->name_s);
        b->size = Le32(e + 12);
        b->flags = Le32(e + 16);
        b->type = Le32(e + 20);
        const gaddr bg = com::Publish(b, BufferVtable());
        const uint32_t nv = Le32(e + 4), vars = Le32(e + 8);
        for (uint32_t j = 0; j < nv && v.In(vars + uint64_t(var_size) * j, var_size); j++) {
          const uint8_t* ve = v.p + vars + var_size * j;
          auto var = std::make_shared<ReflVariable>();
          var->valid = true;
          var->buffer = bg;
          var->name = v.Str(Le32(ve), &var->name_s);
          var->offset = Le32(ve + 4);
          var->size = Le32(ve + 8);
          var->flags = Le32(ve + 12);
          var->type = NewType(v, rd11, Le32(ve + 16), 0, 0);
          if (const uint32_t d = Le32(ve + 20); d && v.In(d, var->size)) var->default_value = v.guest + d;
          if (rd11) {
            var->tex_start = Le32(ve + 24);
            var->tex_size = Le32(ve + 28);
            var->smp_start = Le32(ve + 32);
            var->smp_size = Le32(ve + 36);
          }
          b->variables.push_back(com::Publish(var, VariableVtable()));
        }
        r->buffers.push_back(bg);
      }
      for (uint32_t i = 0; i < nb && v.In(bs + 32ull * i, 32); i++) {
        const uint8_t* e = v.p + bs + 32 * i;
        ReflBinding b;
        b.name = v.Str(Le32(e), &b.name_s);
        b.type = Le32(e + 4);
        b.ret = Le32(e + 8);
        b.dim = Le32(e + 12);
        b.samples = Le32(e + 16);
        b.point = Le32(e + 20);
        b.count = Le32(e + 24);
        b.flags = Le32(e + 28);
        r->bindings.push_back(b);
      }
    }
  }
  Wr64(out, com::Publish(r, com::Vtable(kDll, "ID3D11ShaderReflection",
                                        {"QueryInterface", "AddRef", "Release", "GetDesc",
                                         "GetConstantBufferByIndex", "GetConstantBufferByName",
                                         "GetResourceBindingDesc", "GetInputParameterDesc",
                                         "GetOutputParameterDesc", "GetPatchConstantParameterDesc",
                                         "GetVariableByName", "GetResourceBindingDescByName",
                                         "GetMovInstructionCount", "GetMovcInstructionCount",
                                         "GetConversionInstructionCount",
                                         "GetBitwiseInstructionCount", "GetGSInputPrimitive",
                                         "IsSampleFrequencyShader", "GetNumInterfaceSlots",
                                         "GetMinFeatureLevel", "GetThreadGroupSize"})));
  return kS_OK;
}

#define REFL_METHOD(iface, name) \
  HLE_NAMED_X(kDll, #iface "::" #name, iface##_##name, kCdecl, true)

REFL_METHOD(ID3D11ShaderReflection, QueryInterface) {
  if (com::GuidEq(Arg(c, 1), kIID_ID3D11ShaderReflection43) ||
      com::GuidEq(Arg(c, 1), kIID_ID3D11ShaderReflection47) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr64(Arg(c, 2), Arg(c, 0));
    return kS_OK;
  }
  Wr64(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}
REFL_METHOD(ID3D11ShaderReflection, AddRef) { return com::AddRef(Arg(c, 0)); }
REFL_METHOD(ID3D11ShaderReflection, Release) { return com::Release(Arg(c, 0)); }

// D3D11_SHADER_DESC (160 bytes): the counts; the instruction statistics
// stay zero.
REFL_METHOD(ID3D11ShaderReflection, GetDesc) {
  const Reflection* r = Refl(c);
  const gaddr d = Arg(c, 1);
  std::memset(GPtr(d), 0, 160);
  Wr32(d, r->version);
  Wr64(d + 8, r->creator);
  Wr32(d + 16, r->flags);
  Wr32(d + 20, uint32_t(r->buffers.size()));
  Wr32(d + 24, uint32_t(r->bindings.size()));
  Wr32(d + 28, uint32_t(r->inputs.size()));
  Wr32(d + 32, uint32_t(r->outputs.size()));
  Wr32(d + 124, uint32_t(r->patch.size()));
  return kS_OK;
}
REFL_METHOD(ID3D11ShaderReflection, GetConstantBufferByIndex) {
  const Reflection* r = Refl(c);
  const uint32_t i = uint32_t(Arg(c, 1));
  return i < r->buffers.size() ? r->buffers[i] : NullBuffer();
}
REFL_METHOD(ID3D11ShaderReflection, GetConstantBufferByName) {
  const std::string name = GStr(Arg(c, 1));
  for (gaddr b : Refl(c)->buffers)
    if (com::As<ReflBuffer>(b)->name_s == name) return b;
  return NullBuffer();
}
REFL_METHOD(ID3D11ShaderReflection, GetResourceBindingDesc) {
  const Reflection* r = Refl(c);
  const uint32_t i = uint32_t(Arg(c, 1));
  if (i >= r->bindings.size()) return kE_INVALIDARG;
  WriteBinding(r->bindings[i], Arg(c, 2));
  return kS_OK;
}
REFL_METHOD(ID3D11ShaderReflection, GetResourceBindingDescByName) {
  const std::string name = GStr(Arg(c, 1));
  for (const ReflBinding& b : Refl(c)->bindings)
    if (b.name_s == name) {
      WriteBinding(b, Arg(c, 2));
      return kS_OK;
    }
  return kE_INVALIDARG;
}
REFL_METHOD(ID3D11ShaderReflection, GetInputParameterDesc) {
  return WriteParam(Refl(c)->inputs, uint32_t(Arg(c, 1)), Arg(c, 2));
}
REFL_METHOD(ID3D11ShaderReflection, GetOutputParameterDesc) {
  return WriteParam(Refl(c)->outputs, uint32_t(Arg(c, 1)), Arg(c, 2));
}
REFL_METHOD(ID3D11ShaderReflection, GetPatchConstantParameterDesc) {
  return WriteParam(Refl(c)->patch, uint32_t(Arg(c, 1)), Arg(c, 2));
}
REFL_METHOD(ID3D11ShaderReflection, GetVariableByName) {
  const std::string name = GStr(Arg(c, 1));
  for (gaddr b : Refl(c)->buffers)
    if (gaddr v = VariableNamed(com::As<ReflBuffer>(b)->variables, name)) return v;
  return NullVariable();
}

// D3D11_SHADER_BUFFER_DESC (24 bytes)
REFL_METHOD(ID3D11ShaderReflectionConstantBuffer, GetDesc) {
  const ReflBuffer* b = com::As<ReflBuffer>(Arg(c, 0));
  if (!b->valid) return kE_FAIL;
  const gaddr d = Arg(c, 1);
  Wr64(d, b->name);
  Wr32(d + 8, b->type);
  Wr32(d + 12, uint32_t(b->variables.size()));
  Wr32(d + 16, b->size);
  Wr32(d + 20, b->flags);
  return kS_OK;
}
REFL_METHOD(ID3D11ShaderReflectionConstantBuffer, GetVariableByIndex) {
  const ReflBuffer* b = com::As<ReflBuffer>(Arg(c, 0));
  const uint32_t i = uint32_t(Arg(c, 1));
  return i < b->variables.size() ? b->variables[i] : NullVariable();
}
REFL_METHOD(ID3D11ShaderReflectionConstantBuffer, GetVariableByName) {
  const gaddr v = VariableNamed(com::As<ReflBuffer>(Arg(c, 0))->variables, GStr(Arg(c, 1)));
  return v ? v : NullVariable();
}

// D3D11_SHADER_VARIABLE_DESC (48 bytes)
REFL_METHOD(ID3D11ShaderReflectionVariable, GetDesc) {
  const ReflVariable* v = com::As<ReflVariable>(Arg(c, 0));
  if (!v->valid) return kE_FAIL;
  const gaddr d = Arg(c, 1);
  std::memset(GPtr(d), 0, 48);
  Wr64(d, v->name);
  Wr32(d + 8, v->offset);
  Wr32(d + 12, v->size);
  Wr32(d + 16, v->flags);
  Wr64(d + 24, v->default_value);
  Wr32(d + 32, v->tex_start);
  Wr32(d + 36, v->tex_size);
  Wr32(d + 40, v->smp_start);
  Wr32(d + 44, v->smp_size);
  return kS_OK;
}
REFL_METHOD(ID3D11ShaderReflectionVariable, GetType) {
  const ReflVariable* v = com::As<ReflVariable>(Arg(c, 0));
  return v->type ? v->type : NullType();
}
REFL_METHOD(ID3D11ShaderReflectionVariable, GetBuffer) {
  const ReflVariable* v = com::As<ReflVariable>(Arg(c, 0));
  return v->buffer ? v->buffer : NullBuffer();
}

// D3D11_SHADER_TYPE_DESC (40 bytes)
REFL_METHOD(ID3D11ShaderReflectionType, GetDesc) {
  const ReflType* t = com::As<ReflType>(Arg(c, 0));
  if (!t->valid) return kE_FAIL;
  const gaddr d = Arg(c, 1);
  std::memset(GPtr(d), 0, 40);
  Wr32(d, t->cls);
  Wr32(d + 4, t->type);
  Wr32(d + 8, t->rows);
  Wr32(d + 12, t->cols);
  Wr32(d + 16, t->elements);
  Wr32(d + 20, uint32_t(t->members.size()));
  Wr32(d + 24, t->offset);
  Wr64(d + 32, t->name);
  return kS_OK;
}
REFL_METHOD(ID3D11ShaderReflectionType, GetMemberTypeByIndex) {
  const ReflType* t = com::As<ReflType>(Arg(c, 0));
  const uint32_t i = uint32_t(Arg(c, 1));
  return i < t->members.size() ? t->members[i] : NullType();
}
REFL_METHOD(ID3D11ShaderReflectionType, GetMemberTypeByName) {
  const ReflType* t = com::As<ReflType>(Arg(c, 0));
  const std::string name = GStr(Arg(c, 1));
  for (size_t i = 0; i < t->members.size(); i++)
    if (t->member_names[i] == name) return t->members[i];
  return NullType();
}
REFL_METHOD(ID3D11ShaderReflectionType, GetMemberTypeName) {
  const ReflType* t = com::As<ReflType>(Arg(c, 0));
  const uint32_t i = uint32_t(Arg(c, 1));
  return i < t->members.size() ? t->member_name_ptrs[i] : 0;
}

}  // namespace recomp::hle
