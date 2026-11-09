// Format/FVF/primitive helpers shared by all backends, and the Device
// methods backends need not implement.

#include "runtime/gfx/device.h"

#include <algorithm>
#include <cstring>
#include "runtime/log.h"

namespace recomp::gfx {

ProgramId Device::CreateProgram(const ProgramDesc&) {
  FatalF("gfx: this backend has no programs (console GPUs need SDL_GPU)");
}
void Device::DestroyProgram(ProgramId) {}
TextureId Device::CreateImage(const ImageDesc&) {
  FatalF("gfx: this backend has no programs (console GPUs need SDL_GPU)");
}
DepthId Device::CreateDepthArray(int, int, int, Format) {
  FatalF("gfx: this backend has no programs (console GPUs need SDL_GPU)");
}
BufferId Device::CreateBuffer(uint32_t) {
  FatalF("gfx: this backend has no programs (console GPUs need SDL_GPU)");
}
void Device::DestroyBuffer(BufferId) {}
void Device::UpdateBuffer(BufferId, uint32_t, const void*, uint32_t) {}
void Device::UpdateImage(TextureId, int, int, const Rect&, const void*, uint32_t) {}
void Device::ReadImage(TextureId, int, int, const Rect&, void*, uint32_t) {}
void Device::ClearImage(TextureId, int, const Color&) {}
void Device::ClearDepth(DepthId, float, int, int) {}
void Device::CopyDepth(DepthId, DepthId, const Rect&) {}
void Device::DrawProgram(const ProgramDraw&) {}
void Device::DispatchProgram(const ProgramDispatch&) {}
uint64_t Device::WritesQueued() { return 0; }
uint64_t Device::LandWrites(bool) { return 0; }
void Device::DrawImage(TextureId, const Rect&) {}

uint32_t BitsPerPixel(Format f) {
  switch (f) {
    case Format::X8R8G8B8: case Format::A8R8G8B8: case Format::D24X8:
    case Format::D24S8: case Format::D32:
      return 32;
    case Format::R8G8B8:
      return 24;
    case Format::R5G6B5: case Format::X1R5G5B5: case Format::A1R5G5B5:
    case Format::A4R4G4B4: case Format::X4R4G4B4: case Format::A8L8:
    case Format::D16: case Format::D15S1:
      return 16;
    case Format::R3G3B2: case Format::A8: case Format::L8: case Format::P8:
      return 8;
    case Format::DXT1:
      return 4;
    case Format::DXT2: case Format::DXT3: case Format::DXT4: case Format::DXT5:
      return 8;
    case Format::R16F:
      return 16;
    case Format::G16R16F: case Format::R32F:
      return 32;
    case Format::A16B16G16R16F: case Format::G32R32F:
      return 64;
    case Format::A32B32G32R32F:
      return 128;
    case Format::R8: case Format::R8Uint:
      return 8;
    case Format::R8G8: case Format::R16: case Format::R8G8Uint: case Format::R16Uint:
      return 16;
    case Format::R8G8B8A8: case Format::R8G8B8A8Srgb: case Format::B8G8R8A8Srgb:
    case Format::R16G16: case Format::R10G10B10A2: case Format::R11G11B10F:
    case Format::R8G8B8A8Snorm: case Format::R16G16Snorm: case Format::R8G8B8A8Uint:
    case Format::R16G16Uint: case Format::R32Uint: case Format::R32Sint:
      return 32;
    case Format::R16G16B16A16: case Format::R16G16B16A16Snorm: case Format::R16G16B16A16Uint:
    case Format::R32G32Uint: case Format::R32G32Sint: case Format::D32FS8:
      return 64;
    case Format::R32G32B32A32Uint: case Format::R32G32B32A32Sint:
      return 128;
    case Format::Bc1: case Format::Bc1Srgb: case Format::Bc4:
      return 4;
    case Format::Bc2: case Format::Bc2Srgb: case Format::Bc3: case Format::Bc3Srgb:
    case Format::Bc5: case Format::Bc6hUfloat: case Format::Bc6hSfloat: case Format::Bc7:
    case Format::Bc7Srgb:
      return 8;
    default:
      return 0;
  }
}

bool IsCompressed(Format f) {
  return f == Format::DXT1 || f == Format::DXT2 || f == Format::DXT3 ||
         f == Format::DXT4 || f == Format::DXT5 || (f >= Format::Bc1 && f <= Format::Bc7Srgb);
}

bool IsFloat(Format f) {
  return f == Format::R16F || f == Format::G16R16F || f == Format::A16B16G16R16F ||
         f == Format::R32F || f == Format::G32R32F || f == Format::A32B32G32R32F;
}

bool IsRaw(Format f) { return f >= Format::R16F; }

uint32_t RowPitch(Format f, uint32_t width) {
  if (IsCompressed(f)) {
    uint32_t blocks = (width + 3) / 4;
    return blocks * BitsPerPixel(f) * 2;  // 16 pixels a block
  }
  return width * BitsPerPixel(f) / 8;
}

uint32_t RowCount(Format f, uint32_t height) {
  return IsCompressed(f) ? (height + 3) / 4 : height;
}

namespace {
uint32_t Expand(uint32_t v, int bits) {
  // Replicate high bits so 0x1F -> 0xFF exactly.
  v <<= (8 - bits);
  return v | (v >> bits);
}
}  // namespace

uint32_t RawPixel(const uint8_t* p, Format f) {
  switch (BitsPerPixel(f)) {
    case 32: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    case 24: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
    case 16: return uint32_t(p[0]) | uint32_t(p[1]) << 8;
    case 8: return p[0];
    default: return 0;
  }
}

void PutRawPixel(uint8_t* p, Format f, uint32_t v) {
  switch (BitsPerPixel(f)) {
    case 32: p[3] = uint8_t(v >> 24); [[fallthrough]];
    case 24: p[2] = uint8_t(v >> 16); [[fallthrough]];
    case 16: p[1] = uint8_t(v >> 8); [[fallthrough]];
    case 8: p[0] = uint8_t(v); break;
    default: break;
  }
}

uint32_t DecodePixel(const uint8_t* p, Format f) {
  uint32_t v = RawPixel(p, f);
  switch (f) {
    case Format::A8R8G8B8: return v;
    case Format::X8R8G8B8: case Format::R8G8B8: return v | 0xFF000000u;
    case Format::R5G6B5:
      return 0xFF000000u | Expand((v >> 11) & 0x1F, 5) << 16 |
             Expand((v >> 5) & 0x3F, 6) << 8 | Expand(v & 0x1F, 5);
    case Format::X1R5G5B5: case Format::A1R5G5B5: {
      uint32_t a = (f == Format::X1R5G5B5 || (v & 0x8000)) ? 0xFF : 0;
      return a << 24 | Expand((v >> 10) & 0x1F, 5) << 16 |
             Expand((v >> 5) & 0x1F, 5) << 8 | Expand(v & 0x1F, 5);
    }
    case Format::A4R4G4B4: case Format::X4R4G4B4: {
      uint32_t a = f == Format::X4R4G4B4 ? 0xFF : ((v >> 12) & 0xF) * 0x11;
      return a << 24 | ((v >> 8) & 0xF) * 0x11 << 16 |
             ((v >> 4) & 0xF) * 0x11 << 8 | (v & 0xF) * 0x11;
    }
    case Format::R3G3B2:
      return 0xFF000000u | ((v >> 5) & 7) * 0x24 << 16 |
             ((v >> 2) & 7) * 0x24 << 8 | (v & 3) * 0x55;
    case Format::A8: return v << 24;
    case Format::L8: return 0xFF000000u | v << 16 | v << 8 | v;
    case Format::A8L8: {
      uint32_t l = v & 0xFF;
      return (v >> 8) << 24 | l << 16 | l << 8 | l;
    }
    default: return 0xFFFF00FFu;  // magenta: unsupported
  }
}

void EncodePixel(uint8_t* p, Format f, uint32_t c) {
  uint32_t a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  uint32_t v = 0;
  switch (f) {
    case Format::A8R8G8B8: case Format::X8R8G8B8: case Format::R8G8B8: v = c; break;
    case Format::R5G6B5: v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); break;
    case Format::X1R5G5B5: case Format::A1R5G5B5:
      v = (a >= 0x80 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
      break;
    case Format::A4R4G4B4: case Format::X4R4G4B4:
      v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
      break;
    case Format::R3G3B2: v = (r >> 5) << 5 | (g >> 5) << 2 | (b >> 6); break;
    case Format::A8: v = a; break;
    case Format::L8: v = (r * 77 + g * 150 + b * 29) >> 8; break;
    case Format::A8L8: v = a << 8 | ((r * 77 + g * 150 + b * 29) >> 8); break;
    default: break;
  }
  PutRawPixel(p, f, v);
}

uint32_t FvfStride(uint32_t fvf) {
  uint32_t s = 0;
  switch (fvf & 0x00E) {
    case 0x002: s = 12; break;  // XYZ
    case 0x004: s = 16; break;  // XYZRHW
    case 0x006: s = 16; break;  // XYZB1
    case 0x008: s = 20; break;
    case 0x00A: s = 24; break;
    case 0x00C: s = 28; break;
    case 0x00E: s = 32; break;
  }
  if ((fvf & 0x400E) == 0x4002) s = 16;  // XYZW (D3D9)
  if (fvf & 0x010) s += 12;  // NORMAL
  if (fvf & 0x020) s += 4;   // RESERVED1 (D3D7) / PSIZE (D3D9)
  if (fvf & 0x040) s += 4;   // DIFFUSE
  if (fvf & 0x080) s += 4;   // SPECULAR
  uint32_t ntex = (fvf >> 8) & 0xF;
  for (uint32_t i = 0; i < ntex; i++) {
    switch ((fvf >> (16 + 2 * i)) & 3) {
      case 0: s += 8; break;
      case 1: s += 12; break;
      case 2: s += 16; break;
      case 3: s += 4; break;
    }
  }
  return s;
}

namespace {
uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

void Identity(float* m) {
  std::memset(m, 0, 64);
  m[0] = m[5] = m[10] = m[15] = 1;
}
}  // namespace

FfState DefaultFfState() {
  FfState s;
  std::memset(&s, 0, sizeof s);
  uint32_t* rs = s.rs;
  rs[8] = 3;    // FILLMODE solid
  rs[9] = 2;    // SHADEMODE gouraud
  rs[14] = 1;   // ZWRITEENABLE
  rs[16] = 1;   // LASTPIXEL
  rs[19] = 2;   // SRCBLEND one
  rs[20] = 1;   // DESTBLEND zero
  rs[22] = 3;   // CULLMODE ccw
  rs[23] = 4;   // ZFUNC lessequal
  rs[25] = 8;   // ALPHAFUNC always
  rs[37] = Bits(1.0f);  // FOGEND
  rs[38] = Bits(1.0f);  // FOGDENSITY
  rs[53] = rs[54] = rs[55] = 1;  // stencil ops keep
  rs[56] = 8;   // STENCILFUNC always
  rs[58] = rs[59] = 0xFFFFFFFFu;
  rs[60] = 0xFFFFFFFFu;  // TEXTUREFACTOR
  rs[136] = 1;  // CLIPPING
  rs[137] = 1;  // LIGHTING
  rs[141] = 1;  // COLORVERTEX
  rs[142] = 1;  // LOCALVIEWER
  rs[145] = 1;  // DIFFUSEMATERIALSOURCE color1
  rs[146] = 2;  // SPECULARMATERIALSOURCE color2
  rs[154] = Bits(1.0f);  // POINTSIZE
  rs[168] = 0xF;  // COLORWRITEENABLE
  rs[171] = 1;  // BLENDOP add
  for (uint32_t st = 0; st < 8; st++) {
    uint32_t* t = s.tss[st];
    t[1] = st == 0 ? 4 : 1;  // COLOROP modulate / disable
    t[2] = 2;                // COLORARG1 texture
    t[3] = 1;                // COLORARG2 current
    t[4] = st == 0 ? 2 : 1;  // ALPHAOP selectarg1 / disable
    t[5] = 2;
    t[6] = 1;
    t[11] = st;              // TEXCOORDINDEX
    t[12] = t[13] = t[14] = 1;  // ADDRESS wrap
    t[16] = t[17] = 1;       // MAG/MINFILTER point
    t[18] = 1;               // MIPFILTER none
    t[21] = 1;               // MAXANISOTROPY
    t[26] = t[27] = t[28] = 1;  // COLORARG0, ALPHAARG0, RESULTARG current
  }
  for (auto& m : s.world) Identity(m);
  Identity(s.view);
  Identity(s.proj);
  for (auto& m : s.tex) Identity(m);
  s.viewport = {0, 0, 1, 1, 0, 1};
  return s;
}

void InterleaveFf(const FfDraw& d, uint8_t* out) {
  struct Attr {
    const FfStream* s;
    uint32_t size;
    bool color;
  };
  Attr attrs[13];
  int n = 0;
  uint32_t pos = 12;
  switch (d.fvf & 0x400E) {
    case 0x004: case 0x4002: pos = 16; break;
    case 0x006: case 0x008: case 0x00A: case 0x00C: case 0x00E:
      pos = 12 + 4 * (((d.fvf & 0xE) - 4) / 2);
      break;
  }
  attrs[n++] = {&d.position, pos, false};
  if (d.fvf & 0x010) attrs[n++] = {&d.normal, 12, false};
  if (d.fvf & 0x020) attrs[n++] = {&d.psize, 4, false};
  if (d.fvf & 0x040) attrs[n++] = {&d.diffuse, 4, d.rgba_colors};
  if (d.fvf & 0x080) attrs[n++] = {&d.specular, 4, d.rgba_colors};
  uint32_t ntex = (d.fvf >> 8) & 0xF;
  for (uint32_t i = 0; i < ntex && i < 8; i++) {
    static constexpr uint32_t kSizes[4] = {8, 12, 16, 4};
    attrs[n++] = {&d.tex[i], kSizes[(d.fvf >> (16 + 2 * i)) & 3], false};
  }
  for (uint32_t v = d.first_vertex; v < d.first_vertex + d.vertex_count; v++) {
    for (int a = 0; a < n; a++) {
      const Attr& at = attrs[a];
      if (!at.s->data) {
        std::memset(out, 0, at.size);
      } else {
        const uint8_t* p = static_cast<const uint8_t*>(at.s->data) + size_t(v) * at.s->stride;
        if (at.color) {
          out[0] = p[2];
          out[1] = p[1];
          out[2] = p[0];
          out[3] = p[3];
        } else {
          std::memcpy(out, p, at.size);
        }
      }
      out += at.size;
    }
  }
}

FfDraw FfInterleaved(uint32_t prim, uint32_t fvf, const void* vertices, uint32_t stride) {
  FfDraw d;
  d.prim = prim;
  d.fvf = fvf;
  auto at = [&](uint32_t off) { return FfStream{static_cast<const uint8_t*>(vertices) + off, stride}; };
  uint32_t off = 12;
  switch (fvf & 0x400E) {
    case 0x004: case 0x4002: off = 16; break;
    case 0x006: case 0x008: case 0x00A: case 0x00C: case 0x00E:
      off = 12 + 4 * (((fvf & 0xE) - 4) / 2);
      break;
  }
  d.position = at(0);
  if (fvf & 0x010) d.normal = at(off), off += 12;
  if (fvf & 0x020) d.psize = at(off), off += 4;
  if (fvf & 0x040) d.diffuse = at(off), off += 4;
  if (fvf & 0x080) d.specular = at(off), off += 4;
  uint32_t ntex = (fvf >> 8) & 0xF;
  for (uint32_t i = 0; i < ntex && i < 8; i++) {
    static constexpr uint32_t kSizes[4] = {8, 12, 16, 4};
    d.tex[i] = at(off);
    off += kSizes[(fvf >> (16 + 2 * i)) & 3];
  }
  return d;
}

float* FfTransform(FfState& s, uint32_t type) {
  if (type == 1) return s.world[0];
  if (type == 2) return s.view;
  if (type == 3) return s.proj;
  if (type >= 4 && type <= 6) return s.world[type - 3];
  if (type >= 16 && type < 24) return s.tex[type - 16];
  return nullptr;
}

uint32_t FfIndex(const FfDraw& d, uint32_t i) {
  uint32_t v = !d.indices ? d.first_vertex + i
             : d.index_size == 4 ? static_cast<const uint32_t*>(d.indices)[i]
                                 : static_cast<const uint16_t*>(d.indices)[i];
  v -= d.first_vertex;  // below first_vertex wraps, then clamps
  return v < d.vertex_count ? v : d.vertex_count - 1;
}

std::vector<Rect> ClipRects(uint32_t count, const Rect* rects, const Rect& area) {
  std::vector<Rect> out;
  auto add = [&](const Rect& r) {
    Rect c{std::max(r.left, area.left), std::max(r.top, area.top), std::min(r.right, area.right),
           std::min(r.bottom, area.bottom)};
    if (c.right > c.left && c.bottom > c.top) out.push_back(c);
  };
  if (!count || !rects) add(area);
  for (uint32_t i = 0; rects && i < count; i++) add(rects[i]);
  return out;
}

uint32_t PrimitiveCountFromVertices(uint32_t prim, uint32_t n) {
  switch (prim) {
    case 1: return n;                     // POINTLIST
    case 2: return n / 2;                 // LINELIST
    case 3: return n > 0 ? n - 1 : 0;     // LINESTRIP
    case 4: return n / 3;                 // TRIANGLELIST
    case 5: case 6: return n > 2 ? n - 2 : 0;  // STRIP, FAN
    default: return 0;
  }
}

}  // namespace recomp::gfx
