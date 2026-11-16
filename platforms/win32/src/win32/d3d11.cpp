// d3d11.dll (Direct3D 11) for x64 guests, over gfx::Device's programs
// (ARCHITECTURE.md, "Direct3D 11"). Interfaces are com.h objects whose
// methods are handlers named "<Interface>::<Method>"; a method a game is
// not yet seen to call has no handler and is fatal by name.
//
// The device exists before any window does (the swap chain names one), so
// resources and states are host records here: descriptions and the data
// the guest gave, made into gfx resources once the swap chain has created
// the gfx::Device.

#include <cstring>
#include <memory>
#include <mutex>

#include "runtime/log.h"
#include "win32/com.h"
#include "win32/d3d11_impl.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "x86/runtime.h"

namespace recomp::hle::d3d11 {

namespace {

constexpr const char* kDll = "d3d11.dll";
constexpr uint32_t kFeatureLevel11_0 = 0xB000;

// {db6f6ddb-ac77-4e88-8253-819df9bbf140}
constexpr guest::GUID kIID_ID3D11Device = {
    0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
constexpr guest::GUID kIID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

gaddr DeviceVtable() {
  return com::Vtable(kDll, "ID3D11Device",
                     {"QueryInterface", "AddRef", "Release", "CreateBuffer", "CreateTexture1D",
                      "CreateTexture2D", "CreateTexture3D", "CreateShaderResourceView",
                      "CreateUnorderedAccessView", "CreateRenderTargetView",
                      "CreateDepthStencilView", "CreateInputLayout", "CreateVertexShader",
                      "CreateGeometryShader", "CreateGeometryShaderWithStreamOutput",
                      "CreatePixelShader", "CreateHullShader", "CreateDomainShader",
                      "CreateComputeShader", "CreateClassLinkage", "CreateBlendState",
                      "CreateDepthStencilState", "CreateRasterizerState", "CreateSamplerState",
                      "CreateQuery", "CreatePredicate", "CreateCounter", "CreateDeferredContext",
                      "OpenSharedResource", "CheckFormatSupport", "CheckMultisampleQualityLevels",
                      "CheckCounterInfo", "CheckCounter", "CheckFeatureSupport", "GetPrivateData",
                      "SetPrivateData", "SetPrivateDataInterface", "GetFeatureLevel",
                      "GetCreationFlags", "GetDeviceRemovedReason", "GetImmediateContext",
                      "SetExceptionMode", "GetExceptionMode"});
}

gaddr ContextVtable() {
  return com::Vtable(
      kDll, "ID3D11DeviceContext",
      {"QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData",
       "SetPrivateDataInterface", "VSSetConstantBuffers", "PSSetShaderResources", "PSSetShader",
       "PSSetSamplers", "VSSetShader", "DrawIndexed", "Draw", "Map", "Unmap",
       "PSSetConstantBuffers", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer",
       "DrawIndexedInstanced", "DrawInstanced", "GSSetConstantBuffers", "GSSetShader",
       "IASetPrimitiveTopology", "VSSetShaderResources", "VSSetSamplers", "Begin", "End",
       "GetData", "SetPredication", "GSSetShaderResources", "GSSetSamplers",
       "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState",
       "OMSetDepthStencilState", "SOSetTargets", "DrawAuto", "DrawIndexedInstancedIndirect",
       "DrawInstancedIndirect", "Dispatch", "DispatchIndirect", "RSSetState", "RSSetViewports",
       "RSSetScissorRects", "CopySubresourceRegion", "CopyResource", "UpdateSubresource",
       "CopyStructureCount", "ClearRenderTargetView", "ClearUnorderedAccessViewUint",
       "ClearUnorderedAccessViewFloat", "ClearDepthStencilView", "GenerateMips",
       "SetResourceMinLOD", "GetResourceMinLOD", "ResolveSubresource", "ExecuteCommandList",
       "HSSetShaderResources", "HSSetShader", "HSSetSamplers", "HSSetConstantBuffers",
       "DSSetShaderResources", "DSSetShader", "DSSetSamplers", "DSSetConstantBuffers",
       "CSSetShaderResources", "CSSetUnorderedAccessViews", "CSSetShader", "CSSetSamplers",
       "CSSetConstantBuffers", "VSGetConstantBuffers", "PSGetShaderResources", "PSGetShader",
       "PSGetSamplers", "VSGetShader", "PSGetConstantBuffers", "IAGetInputLayout",
       "IAGetVertexBuffers", "IAGetIndexBuffer", "GSGetConstantBuffers", "GSGetShader",
       "IAGetPrimitiveTopology", "VSGetShaderResources", "VSGetSamplers", "GetPredication",
       "GSGetShaderResources", "GSGetSamplers", "OMGetRenderTargets",
       "OMGetRenderTargetsAndUnorderedAccessViews", "OMGetBlendState", "OMGetDepthStencilState",
       "SOGetTargets", "RSGetState", "RSGetViewports", "RSGetScissorRects",
       "HSGetShaderResources", "HSGetShader", "HSGetSamplers", "HSGetConstantBuffers",
       "DSGetShaderResources", "DSGetShader", "DSGetSamplers", "DSGetConstantBuffers",
       "CSGetShaderResources", "CSGetUnorderedAccessViews", "CSGetShader", "CSGetSamplers",
       "CSGetConstantBuffers", "ClearState", "Flush", "GetType", "GetContextFlags",
       "FinishCommandList"});
}

std::mutex g_mu;
std::shared_ptr<Device> g_device;  // one device per process

}  // namespace

Device* TheDevice() { return g_device.get(); }

// D3D11CreateDevice(adapter, driver type, software, flags, feature levels,
// count, SDK version, &device, &feature level, &immediate context): one
// hardware device at feature level 11_0, whatever adapter and levels are
// asked for (a game that asks only for higher levels gets an error).
HLE64(kDll, D3D11CreateDevice, kCdecl) {
  const gaddr levels = Arg(c, 4), out_dev = Arg(c, 7), out_level = Arg(c, 8),
              out_ctx = Arg(c, 9);
  const uint32_t nlevels = uint32_t(Arg(c, 5));
  bool ok = !levels || !nlevels;
  for (uint32_t i = 0; i < nlevels && !ok; i++) ok = Rd32(levels + 4 * i) <= kFeatureLevel11_0;
  if (!ok) return kE_INVALIDARG;
  std::lock_guard<std::mutex> l(g_mu);
  // A device the game has released (probing for the feature level first)
  // is gone; the next one is new.
  if (g_device && !com::Lookup(g_device->guest)) g_device.reset();
  if (!g_device) {
    g_device = std::make_shared<Device>();
    com::Publish(g_device, DeviceVtable());
    g_device->context = std::make_shared<Context>();
    com::Publish(g_device->context, ContextVtable());
    RECOMP_TRACE("  d3d11: device %08llX, immediate context %08llX\n",
                 (unsigned long long)g_device->guest,
                 (unsigned long long)g_device->context->guest);
  } else {
    com::AddRef(g_device->guest);
    com::AddRef(g_device->context->guest);
  }
  if (out_dev) Wr64(out_dev, g_device->guest);
  else com::Release(g_device->guest);
  if (out_ctx) Wr64(out_ctx, g_device->context->guest);
  else com::Release(g_device->context->guest);
  if (out_level) Wr32(out_level, kFeatureLevel11_0);
  return kS_OK;
}

// ---- ID3D11Device ----------------------------------------------------------------

#define D3D11_METHOD(iface, name) \
  HLE_NAMED_X(kDll, #iface "::" #name, iface##_##name, kCdecl, true)

D3D11_METHOD(ID3D11Device, QueryInterface) {
  const gaddr iid = Arg(c, 1), out = Arg(c, 2);
  if (com::GuidEq(iid, kIID_ID3D11Device) || com::GuidEq(iid, kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr64(out, Arg(c, 0));
    return kS_OK;
  }
  // {54ec77fa-1377-44e6-8c32-88fd5f44c84c} IDXGIDevice and
  // {77db970f-6276-48ba-ba28-070143b4392c} IDXGIDevice1
  static constexpr guest::GUID kIID_IDXGIDevice = {
      0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
  static constexpr guest::GUID kIID_IDXGIDevice1 = {
      0x77db970f, 0x6276, 0x48ba, {0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c}};
  if (com::GuidEq(iid, kIID_IDXGIDevice) || com::GuidEq(iid, kIID_IDXGIDevice1)) {
    Wr64(out, DxgiDeviceOf(*com::As<Device>(Arg(c, 0))));
    return kS_OK;
  }
  guest::GUID g = com::ReadGuid(iid);
  WarnF("ID3D11Device::QueryInterface({%08X-%04X-%04X-...}): not supported\n", g.Data1, g.Data2,
        g.Data3);
  Wr64(out, 0);
  return com::kE_NOINTERFACE;
}
D3D11_METHOD(ID3D11Device, AddRef) { return com::AddRef(Arg(c, 0)); }
D3D11_METHOD(ID3D11Device, Release) { return com::Release(Arg(c, 0)); }
D3D11_METHOD(ID3D11Device, GetFeatureLevel) { return kFeatureLevel11_0; }
D3D11_METHOD(ID3D11Device, GetCreationFlags) { return 0; }
D3D11_METHOD(ID3D11Device, GetDeviceRemovedReason) { return kS_OK; }
D3D11_METHOD(ID3D11Device, GetImmediateContext) {
  gaddr ctx = com::As<Device>(Arg(c, 0))->context->guest;
  com::AddRef(ctx);
  Wr64(Arg(c, 1), ctx);
  return 0;
}

// CreateDeferredContext(flags, &context): a context that records
// (d3d11_context.cpp) with the immediate context's methods.
D3D11_METHOD(ID3D11Device, CreateDeferredContext) {
  auto x = std::make_shared<Context>();
  x->deferred = true;
  const gaddr out = Arg(c, 2);
  if (!out) return kS_FALSE;
  Wr64(out, com::Publish(x, ContextVtable()));
  return kS_OK;
}

// ---- ID3D11DeviceContext -------------------------------------------------------------

D3D11_METHOD(ID3D11DeviceContext, AddRef) { return com::AddRef(Arg(c, 0)); }
D3D11_METHOD(ID3D11DeviceContext, Release) { return com::Release(Arg(c, 0)); }
D3D11_METHOD(ID3D11DeviceContext, GetDevice) {
  com::AddRef(g_device->guest);
  Wr64(Arg(c, 1), g_device->guest);
  return 0;
}

}  // namespace recomp::hle::d3d11
