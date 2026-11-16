// d3dcompiler (the D3DCompiler_4x DLLs) for x64 guests: ID3DBlob, and
// D3DGetBlobPart's signature parts, which games pass to CreateInputLayout.
// A DXBC container is "DXBC", a 16-byte checksum, 1, its total size, its
// chunk count and their offsets; each chunk is a FourCC, a size and the
// data. Parts are returned as containers of their own; their checksum is
// left zero (Direct3D 11 here does not check it).

#include <cstring>
#include <memory>
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

}  // namespace recomp::hle
