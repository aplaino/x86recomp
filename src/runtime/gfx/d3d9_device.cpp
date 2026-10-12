// gfx::Device on Direct3D 9 through dxvk-native (Linux bring-up backend).
//
// This file is the only one that sees dxvk's Windows/D3D9 headers. D3D7
// semantics map almost 1:1 onto D3D9's fixed-function pipeline; the
// differences handled here:
//   - texture addressing/filtering moved from texture stage states to
//     sampler states, with renumbered filter enums;
//   - WORLD transforms are 256+ in D3D9;
//   - D3D7's integer ZBIAS becomes D3D9's float DEPTHBIAS;
//   - render states D3D9 dropped are ignored (logged once).

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "runtime/gfx/device.h"
#include "runtime/log.h"

namespace recomp::gfx {

namespace {

D3DFORMAT ToD3D(Format f) {
  switch (f) {
    case Format::X8R8G8B8: return D3DFMT_X8R8G8B8;
    case Format::A8R8G8B8: return D3DFMT_A8R8G8B8;
    case Format::R8G8B8: return D3DFMT_R8G8B8;
    case Format::R5G6B5: return D3DFMT_R5G6B5;
    case Format::X1R5G5B5: return D3DFMT_X1R5G5B5;
    case Format::A1R5G5B5: return D3DFMT_A1R5G5B5;
    case Format::A4R4G4B4: return D3DFMT_A4R4G4B4;
    case Format::X4R4G4B4: return D3DFMT_X4R4G4B4;
    case Format::R3G3B2: return D3DFMT_R3G3B2;
    case Format::A8: return D3DFMT_A8;
    case Format::L8: return D3DFMT_L8;
    case Format::A8L8: return D3DFMT_A8L8;
    case Format::P8: return D3DFMT_P8;
    case Format::D16: return D3DFMT_D16;
    case Format::D24X8: return D3DFMT_D24X8;
    case Format::D24S8: return D3DFMT_D24S8;
    case Format::D32: return D3DFMT_D32;
    case Format::D15S1: return D3DFMT_D15S1;
    case Format::DXT1: return D3DFMT_DXT1;
    case Format::DXT2: return D3DFMT_DXT2;
    case Format::DXT3: return D3DFMT_DXT3;
    case Format::DXT4: return D3DFMT_DXT4;
    case Format::DXT5: return D3DFMT_DXT5;
    default: return D3DFMT_UNKNOWN;
  }
}

void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) FatalF("d3d9: %s failed (%08lx)", what, (unsigned long)hr);
}

class D3D9Device final : public Device {
 public:
  explicit D3D9Device(SDL_Window* w) : window_(w) {
    // dxvk-native refuses to start without a WSI backend name.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    // Without vsync, present in mailbox mode (no tearing) rather than
    // immediate (DS1 FINDINGS.md "Freeze on a quick focus flip"). A tearFree
    // setting the user made wins.
    const char* conf = std::getenv("DXVK_CONFIG");
    if (!conf || !std::strstr(conf, "tearFree")) {
      std::string v = conf && *conf ? std::string(conf) + ";" : std::string();
      v += "dxvk.tearFree = True";
      setenv("DXVK_CONFIG", v.c_str(), 1);
    }
    d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) FatalF("Direct3DCreate9 failed");
  }

  ~D3D9Device() override {
    for (auto& [id, t] : textures_) t->Release();
    for (auto& [id, d] : depths_) d.surf->Release();
    if (staging_) staging_->Release();
    if (readback_) readback_->Release();
    ReleaseTargets();
    if (dev_) dev_->Release();
    if (d3d_) d3d_->Release();
  }

  // Always X8R8G8B8; 16-bit modes convert on blit.
  void ResetSwapChain(int w, int h, bool vsync) override {
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = UINT(w);
    pp.BackBufferHeight = UINT(h);
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = reinterpret_cast<HWND>(window_);
    pp.PresentationInterval =
        vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ReleaseTargets();
    bool reset = false;
    if (!dev_) {
      Check(d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                               reinterpret_cast<HWND>(window_),
                               D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                   D3DCREATE_FPU_PRESERVE |
                                   D3DCREATE_MULTITHREADED,
                               &pp, &dev_),
            "CreateDevice");
      D3DADAPTER_IDENTIFIER9 id{};
      d3d_->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
      std::fprintf(stderr, "gfx: d3d9 (dxvk-native) on %s, %dx%d\n",
                   id.Description, w, h);
    } else {
      // Default-pool resources must go before Reset. Render-target
      // textures come back empty; the game redraws them every frame.
      for (auto& [id, d] : depths_) d.surf->Release();
      for (auto& [id, r] : targets_) textures_[id]->Release();
      if (staging_) {
        staging_->Release();
        staging_ = nullptr;
      }
      if (readback_) {
        readback_->Release();
        readback_ = nullptr;
      }
      Check(dev_->Reset(&pp), "Reset");
      reset = true;
      for (auto& [id, d] : depths_)
        Check(dev_->CreateDepthStencilSurface(UINT(d.w), UINT(d.h), ToD3D(d.fmt),
                                              D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &d.surf, nullptr),
              "CreateDepthStencilSurface");
      for (auto& [id, r] : targets_)
        Check(dev_->CreateTexture(UINT(r.w), UINT(r.h), UINT(r.levels), D3DUSAGE_RENDERTARGET,
                                  ToD3D(r.fmt), D3DPOOL_DEFAULT, &textures_[id], nullptr),
              "CreateTexture(render target)");
    }
    Check(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &swap_),
          "GetBackBuffer");
    // The game draws into its own back buffer; Present copies that to the
    // front buffer, which is then shown. Keeping the front separate lets
    // the game read and draw on it between frames without touching the
    // next frame (as with a DirectDraw primary surface).
    for (IDirect3DSurface9** t : {&backbuffer_, &front_}) {
      Check(dev_->CreateRenderTarget(UINT(w), UINT(h), D3DFMT_X8R8G8B8,
                                     D3DMULTISAMPLE_NONE, 0, FALSE, t, nullptr),
            "CreateRenderTarget");
      dev_->ColorFill(*t, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    }
    dev_->SetRenderTarget(0, backbuffer_);
    if (reset) {
      // Reset also unbinds the targets.
      SetRenderTarget(current_color_, current_depth_);
      RestoreState();
    }
  }

  TextureId CreateTexture(int w, int h, int levels, Format fmt,
                          bool rt) override {
    IDirect3DTexture9* t = nullptr;
    HRESULT hr = dev_->CreateTexture(UINT(w), UINT(h), UINT(levels),
                                     rt ? D3DUSAGE_RENDERTARGET : 0,
                                     ToD3D(fmt),
                                     rt ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
                                     &t, nullptr);
    if (FAILED(hr)) {
      WarnF("gfx: CreateTexture %dx%d x%d fmt %u failed (%08lx)\n", w, h,
            levels, unsigned(fmt), (unsigned long)hr);
      return 0;
    }
    TextureId id = next_id_++;
    textures_[id] = t;
    if (rt) targets_[id] = {w, h, levels, fmt};
    return id;
  }

  void DestroyTexture(TextureId id) override {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    it->second->Release();
    textures_.erase(it);
    targets_.erase(id);
  }

  void FillTexture(TextureId id, const Rect& r, uint32_t argb) override {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    IDirect3DSurface9* s = nullptr;
    it->second->GetSurfaceLevel(0, &s);
    RECT rc{r.left, r.top, r.right, r.bottom};
    dev_->ColorFill(s, &rc, argb);
    s->Release();
  }

  void UpdateTexture(TextureId id, int level, const Rect& r, const void* src,
                     uint32_t pitch) override {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    D3DSURFACE_DESC desc;
    it->second->GetLevelDesc(UINT(level), &desc);
    Format f = FromD3D(desc.Format);
    RECT rc{r.left, r.top, r.right, r.bottom};
    D3DLOCKED_RECT lr;
    if (FAILED(it->second->LockRect(UINT(level), &lr, &rc, 0))) return;
    uint32_t row = RowPitch(f, uint32_t(r.right - r.left));
    uint32_t rows = RowCount(f, uint32_t(r.bottom - r.top));
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(lr.pBits);
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(d + size_t(y) * uint32_t(lr.Pitch), s + size_t(y) * pitch, row);
    it->second->UnlockRect(UINT(level));
  }

  DepthId CreateDepth(int w, int h, Format fmt) override {
    Depth d{nullptr, w, h, fmt};
    if (FAILED(dev_->CreateDepthStencilSurface(UINT(w), UINT(h), ToD3D(fmt),
                                               D3DMULTISAMPLE_NONE, 0, FALSE,
                                               &d.surf, nullptr))) {
      WarnF("gfx: CreateDepth %dx%d fmt %u failed\n", w, h, unsigned(fmt));
      return 0;
    }
    DepthId id = next_id_++;
    depths_[id] = d;
    return id;
  }

  void DestroyDepth(DepthId id) override {
    auto it = depths_.find(id);
    if (it == depths_.end()) return;
    if (current_depth_ == id) {
      dev_->SetDepthStencilSurface(nullptr);
      current_depth_ = 0;
    }
    it->second.surf->Release();
    depths_.erase(it);
  }

  void SetRenderTarget(TextureId color, DepthId depth) override {
    current_color_ = color;
    if (color) {
      auto it = textures_.find(color);
      if (it != textures_.end()) {
        IDirect3DSurface9* s = nullptr;
        it->second->GetSurfaceLevel(0, &s);
        dev_->SetRenderTarget(0, s);
        s->Release();
      }
    } else {
      dev_->SetRenderTarget(0, backbuffer_);
    }
    auto d = depths_.find(depth);
    dev_->SetDepthStencilSurface(d == depths_.end() ? nullptr : d->second.surf);
    current_depth_ = depth;
  }

  void Blit(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
            const Rect& dr, bool colorkey, uint32_t key) override {
    DrawImage(src, pitch, fmt, sr, dr, colorkey, key);
  }

  void BlitFront(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
                 const Rect& dr) override {
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    D3DVIEWPORT9 vp;
    dev_->GetRenderTarget(0, &rt);
    dev_->GetDepthStencilSurface(&ds);
    dev_->GetViewport(&vp);
    dev_->SetRenderTarget(0, front_);
    dev_->SetDepthStencilSurface(nullptr);
    DrawImage(src, pitch, fmt, sr, dr, false, 0);
    dev_->SetRenderTarget(0, rt);
    dev_->SetDepthStencilSurface(ds);
    dev_->SetViewport(&vp);
    if (rt) rt->Release();
    if (ds) ds->Release();
  }

  void ReadBackFront(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    ReadSurface(front_, r, dst, pitch, fmt);
  }

  void PresentFront() override {
    if (in_scene_) EndScene();
    dev_->StretchRect(front_, nullptr, swap_, nullptr, D3DTEXF_POINT);
    dev_->Present(nullptr, nullptr, nullptr, nullptr);
  }

  void ReadBack(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    IDirect3DSurface9* rt = nullptr;
    dev_->GetRenderTarget(0, &rt);
    ReadSurface(rt, r, dst, pitch, fmt);
    rt->Release();
  }

  void SetRenderState(uint32_t s, uint32_t v) override {
    state_.rs[s] = v;
    switch (s) {
      case 47: {  // D3DRENDERSTATE_ZBIAS (0..16) -> DEPTHBIAS
        float bias = -float(v) * 0.000005f;
        uint32_t bits;
        std::memcpy(&bits, &bias, 4);
        dev_->SetRenderState(D3DRS_DEPTHBIAS, bits);
        return;
      }
      // D3D7 states that D3D9 dropped or that have no effect here.
      case 1: case 2: case 3: case 4: case 5: case 6: case 10: case 11:
      case 12: case 13: case 17: case 18: case 21: case 30: case 31:
      case 32: case 33: case 39: case 40: case 41: case 42: case 43:
      case 44: case 45: case 46: case 49: case 50: case 138: case 144:
        if (ignored_rs_.insert(s).second)
          RECOMP_TRACE("gfx: render state %u = %u ignored\n", s, v);
        return;
      default:
        dev_->SetRenderState(D3DRENDERSTATETYPE(s), v);
    }
  }

  void SetTextureStageState(uint32_t stage, uint32_t type, uint32_t v) override {
    state_.tss[{stage, type}] = v;
    switch (type) {
      case 12:  // ADDRESS (both)
        dev_->SetSamplerState(stage, D3DSAMP_ADDRESSU, v);
        dev_->SetSamplerState(stage, D3DSAMP_ADDRESSV, v);
        return;
      case 13: dev_->SetSamplerState(stage, D3DSAMP_ADDRESSU, v); return;
      case 14: dev_->SetSamplerState(stage, D3DSAMP_ADDRESSV, v); return;
      case 15: dev_->SetSamplerState(stage, D3DSAMP_BORDERCOLOR, v); return;
      case 16: {  // MAGFILTER: D3DTFG_POINT 1, LINEAR 2, cubic 3/4, ANISO 5
        DWORD f = v == 1 ? D3DTEXF_POINT : v == 5 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
        dev_->SetSamplerState(stage, D3DSAMP_MAGFILTER, f);
        return;
      }
      case 17: {  // MINFILTER: D3DTFN_POINT 1, LINEAR 2, ANISOTROPIC 3
        DWORD f = v == 1 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
        dev_->SetSamplerState(stage, D3DSAMP_MINFILTER, f);
        return;
      }
      case 18: {  // MIPFILTER: D3DTFP_NONE 1, POINT 2, LINEAR 3
        DWORD f = v == 2 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_LINEAR : D3DTEXF_NONE;
        dev_->SetSamplerState(stage, D3DSAMP_MIPFILTER, f);
        return;
      }
      case 19: dev_->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, v); return;
      case 20: dev_->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, v); return;
      case 21: dev_->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, v); return;
      default:
        dev_->SetTextureStageState(stage, D3DTEXTURESTAGESTATETYPE(type), v);
    }
  }

  void SetTexture(uint32_t stage, TextureId id) override {
    if (stage >= 8) return;
    state_.tex[stage] = id;
    auto it = textures_.find(id);
    dev_->SetTexture(stage, it == textures_.end() ? nullptr : it->second);
  }

  void SetTransform(uint32_t type, const float m[16]) override {
    std::memcpy(state_.xf[type].data(), m, sizeof(float) * 16);
    D3DTRANSFORMSTATETYPE t;
    switch (type) {
      case 1: t = D3DTS_WORLD; break;
      case 4: t = D3DTS_WORLD1; break;
      case 5: t = D3DTS_WORLD2; break;
      case 6: t = D3DTS_WORLD3; break;
      default: t = D3DTRANSFORMSTATETYPE(type); break;
    }
    dev_->SetTransform(t, reinterpret_cast<const D3DMATRIX*>(m));
  }

  void SetViewport(const Viewport& v) override {
    state_.vp = v;
    D3DVIEWPORT9 vp{v.x, v.y, v.width, v.height, v.min_z, v.max_z};
    dev_->SetViewport(&vp);
  }

  void SetScissor(const Rect& r) override {
    state_.scissor = r;
    RECT rc{r.left, r.top, r.right, r.bottom};
    dev_->SetScissorRect(&rc);
  }

  void SetMaterial(const Material& m) override {
    state_.mat = m;
    static_assert(sizeof(Material) == sizeof(D3DMATERIAL9));
    dev_->SetMaterial(reinterpret_cast<const D3DMATERIAL9*>(&m));
  }

  void SetLight(uint32_t i, const Light& l) override {
    state_.lights[i] = l;
    static_assert(sizeof(Light) == sizeof(D3DLIGHT9));
    dev_->SetLight(i, reinterpret_cast<const D3DLIGHT9*>(&l));
  }

  void LightEnable(uint32_t i, bool e) override {
    state_.light_on[i] = e;
    dev_->LightEnable(i, e);
  }

  void SetClipPlane(uint32_t i, const float p[4]) override {
    std::memcpy(state_.clip[i].data(), p, sizeof(float) * 4);
    dev_->SetClipPlane(i, p);
  }

  void Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t color,
             float z, uint32_t stencil) override {
    static_assert(sizeof(Rect) == sizeof(D3DRECT));
    if (!current_depth_) flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    dev_->Clear(count, reinterpret_cast<const D3DRECT*>(rects), flags, color, z,
                stencil);
  }

  void BeginScene() override {
    if (!in_scene_) dev_->BeginScene();
    in_scene_ = true;
  }
  void EndScene() override {
    if (in_scene_) dev_->EndScene();
    in_scene_ = false;
  }

  void Draw(uint32_t prim, uint32_t fvf, const void* v, uint32_t nv,
            uint32_t stride, const uint16_t* idx, uint32_t ni) override {
    dev_->SetFVF(fvf);
    if (idx) {
      UINT prims = PrimitiveCountFromVertices(prim, ni);
      if (prims)
        dev_->DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE(prim), 0, nv, prims, idx,
                                     D3DFMT_INDEX16, v, stride);
    } else {
      UINT prims = PrimitiveCountFromVertices(prim, nv);
      if (prims) dev_->DrawPrimitiveUP(D3DPRIMITIVETYPE(prim), prims, v, stride);
    }
  }

  void Present() override {
    if (in_scene_) EndScene();
    dev_->StretchRect(backbuffer_, nullptr, front_, nullptr, D3DTEXF_POINT);
    PresentFront();
  }

 private:
  void DrawImage(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
                 const Rect& dr, bool colorkey, uint32_t key) {
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0) return;
    EnsureStaging(w, h);
    D3DLOCKED_RECT lr;
    RECT rc{0, 0, w, h};
    if (FAILED(staging_->LockRect(0, &lr, &rc, 0))) return;
    uint32_t bpp = BitsPerPixel(fmt) / 8;
    for (int y = 0; y < h; y++) {
      const uint8_t* s = static_cast<const uint8_t*>(src) +
                         size_t(sr.top + y) * pitch + size_t(sr.left) * bpp;
      uint32_t* d = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) +
                                                size_t(y) * uint32_t(lr.Pitch));
      if (!colorkey && (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8)) {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(s);
        for (int x = 0; x < w; x++) d[x] = p[x] | 0xFF000000u;
        continue;
      }
      for (int x = 0; x < w; x++, s += bpp) {
        if (colorkey && RawPixel(s, fmt) == key)
          d[x] = 0;
        else
          d[x] = DecodePixel(s, fmt) | 0xFF000000u;
      }
    }
    staging_->UnlockRect(0);

    // Draw it as a screen-space quad with private state.
    IDirect3DStateBlock9* sb = nullptr;
    dev_->CreateStateBlock(D3DSBT_ALL, &sb);
    dev_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev_->SetTexture(0, staging_);
    for (DWORD i = 1; i < 8; i++) dev_->SetTexture(i, nullptr);
    dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev_->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev_->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev_->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ALPHATESTENABLE, colorkey ? TRUE : FALSE);
    dev_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    dev_->SetRenderState(D3DRS_ALPHAREF, 0);
    dev_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    D3DSURFACE_DESC td;
    staging_->GetLevelDesc(0, &td);
    float u1 = float(w) / float(td.Width), v1 = float(h) / float(td.Height);
    struct V {
      float x, y, z, rhw, u, v;
    } q[4] = {
        {float(dr.left) - 0.5f, float(dr.top) - 0.5f, 0, 1, 0, 0},
        {float(dr.right) - 0.5f, float(dr.top) - 0.5f, 0, 1, u1, 0},
        {float(dr.left) - 0.5f, float(dr.bottom) - 0.5f, 0, 1, 0, v1},
        {float(dr.right) - 0.5f, float(dr.bottom) - 0.5f, 0, 1, u1, v1},
    };
    bool in_scene = in_scene_;
    if (!in_scene) dev_->BeginScene();
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    if (!in_scene) dev_->EndScene();
    if (sb) {
      sb->Apply();
      sb->Release();
    }
  }

  void ReadSurface(IDirect3DSurface9* rt, const Rect& r, void* dst, uint32_t pitch,
                   Format fmt) {
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    if (readback_) {
      D3DSURFACE_DESC rd;
      readback_->GetDesc(&rd);
      if (rd.Width != d.Width || rd.Height != d.Height || rd.Format != d.Format) {
        readback_->Release();
        readback_ = nullptr;
      }
    }
    if (!readback_ &&
        FAILED(dev_->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format,
                                                 D3DPOOL_SYSTEMMEM, &readback_,
                                                 nullptr))) {
      readback_ = nullptr;
      return;
    }
    dev_->GetRenderTargetData(rt, readback_);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(readback_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
      uint32_t bpp = BitsPerPixel(fmt) / 8;
      int w = r.right - r.left;
      for (int y = r.top; y < r.bottom; y++) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(lr.pBits) + size_t(y) * uint32_t(lr.Pitch)) + r.left;
        uint8_t* out = static_cast<uint8_t*>(dst) + size_t(y - r.top) * pitch;
        if (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8) {
          // The render target is X8R8G8B8 too: copy, opaque alpha.
          uint32_t* o = reinterpret_cast<uint32_t*>(out);
          for (int x = 0; x < w; x++) o[x] = src[x] | 0xFF000000u;
        } else if (fmt == Format::R5G6B5) {
          uint16_t* o = reinterpret_cast<uint16_t*>(out);
          for (int x = 0; x < w; x++) {
            uint32_t c = src[x];
            o[x] = uint16_t((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F));
          }
        } else {
          for (int x = 0; x < w; x++)
            EncodePixel(out + size_t(x) * bpp, fmt, src[x] | 0xFF000000u);
        }
      }
      readback_->UnlockRect();
    }
  }

  struct Depth {
    IDirect3DSurface9* surf;
    int w, h;
    Format fmt;
  };
  struct Target {
    int w, h, levels;
    Format fmt;
  };

  static Format FromD3D(D3DFORMAT f) {
    for (uint32_t i = 0; i <= uint32_t(Format::DXT5); i++)
      if (ToD3D(Format(i)) == f) return Format(i);
    return Format::Unknown;
  }

  void ReleaseTargets() {
    for (IDirect3DSurface9** t : {&swap_, &backbuffer_, &front_})
      if (*t) {
        (*t)->Release();
        *t = nullptr;
      }
  }

  void EnsureStaging(int w, int h) {
    if (staging_) {
      D3DSURFACE_DESC d;
      staging_->GetLevelDesc(0, &d);
      if (int(d.Width) >= w && int(d.Height) >= h) return;
      staging_->Release();
      staging_ = nullptr;
    }
    int sw = 256, sh = 256;
    while (sw < w) sw *= 2;
    while (sh < h) sh *= 2;
    Check(dev_->CreateTexture(UINT(sw), UINT(sh), 1, D3DUSAGE_DYNAMIC,
                              D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &staging_,
                              nullptr),
          "CreateTexture(staging)");
  }

  SDL_Window* window_;
  IDirect3D9* d3d_ = nullptr;
  IDirect3DDevice9* dev_ = nullptr;
  IDirect3DSurface9* swap_ = nullptr;        // the swap chain's back buffer
  IDirect3DSurface9* backbuffer_ = nullptr;  // the game's back buffer
  IDirect3DSurface9* front_ = nullptr;       // the DirectDraw primary
  IDirect3DTexture9* staging_ = nullptr;
  IDirect3DSurface9* readback_ = nullptr;  // system-memory copy of the RT
  std::map<uint32_t, IDirect3DTexture9*> textures_;
  std::map<uint32_t, Target> targets_;  // render-target textures (default pool)
  std::map<uint32_t, Depth> depths_;
  uint32_t next_id_ = 1;
  TextureId current_color_ = 0;  // 0: the game's back buffer
  DepthId current_depth_ = 0;
  bool in_scene_ = false;
  std::set<uint32_t> ignored_rs_;

  // Everything set through the Device interface: D3D9's Reset discards
  // device state, and front ends only send state when it changes.
  struct State {
    std::map<uint32_t, uint32_t> rs;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> tss;
    std::map<uint32_t, TextureId> tex;
    std::map<uint32_t, std::array<float, 16>> xf;
    std::optional<Viewport> vp;
    std::optional<Rect> scissor;
    std::optional<Material> mat;
    std::map<uint32_t, Light> lights;
    std::map<uint32_t, bool> light_on;
    std::map<uint32_t, std::array<float, 4>> clip;
  } state_;

  void RestoreState() {
    const State s = state_;
    for (const auto& [k, v] : s.rs) SetRenderState(k, v);
    for (const auto& [k, v] : s.tss) SetTextureStageState(k.first, k.second, v);
    for (const auto& [k, v] : s.tex) SetTexture(k, v);
    for (const auto& [k, v] : s.xf) SetTransform(k, v.data());
    if (s.vp) SetViewport(*s.vp);
    if (s.scissor) SetScissor(*s.scissor);
    if (s.mat) SetMaterial(*s.mat);
    for (const auto& [k, v] : s.lights) SetLight(k, v);
    for (const auto& [k, v] : s.light_on) LightEnable(k, v);
    for (const auto& [k, v] : s.clip) SetClipPlane(k, v.data());
  }
};

}  // namespace

std::unique_ptr<Device> CreateD3D9Device(SDL_Window* window) {
  return std::make_unique<D3D9Device>(window);
}

}  // namespace recomp::gfx
