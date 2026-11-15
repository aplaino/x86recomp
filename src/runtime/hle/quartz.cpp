// DirectShow's filter graph manager (quartz.dll, CLSID_FilterGraph) for
// games that play their movies through it: a graph that takes the filters
// a game adds and plays no media, so each movie ends as it starts. Run
// queues EC_COMPLETE, which IMediaEvent hands out (and its event handle
// signals), as a graph does at the end of the stream. A game sees a movie
// that is already over and goes on.
//
// The graph's interfaces (IGraphBuilder, IMediaControl, IMediaEvent,
// IMediaSeeking, IBasicAudio) are each a guest object over one shared
// Graph, as com.h has one guest pointer per interface; QueryInterface on
// any of them returns the graph's object for the requested one.

#include <array>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr const char* kDll = "quartz.dll";

constexpr GUID kCLSID_FilterGraph = {0xE436EBB3, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IFilterGraph = {0x56A8689F, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IGraphBuilder = {0x56A868A9, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IMediaControl = {0x56A868B1, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IMediaEvent = {0x56A868B6, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IMediaEventEx = {0x56A868C0, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IMediaSeeking = {0x36B73880, 0xC2C8, 0x11CF, {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60}};
constexpr GUID kIID_IBasicAudio = {0x56A868B3, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};

constexpr uint32_t S_OK = 0, S_FALSE = 1, E_POINTER = 0x80004003, E_ABORT = 0x80004004,
                   E_NOINTERFACE = 0x80004002;
constexpr uint32_t EC_COMPLETE = 0x01;
// FILTER_STATE
constexpr uint32_t State_Stopped = 0, State_Paused = 1, State_Running = 2;

enum Iface { kBuilder, kControl, kEvent, kSeeking, kAudio, kIfaces };

struct Graph;
struct Facade : com::Object {
  std::shared_ptr<Graph> graph;
};

struct Graph : std::enable_shared_from_this<Graph> {
  std::mutex mu;
  std::array<uint32_t, kIfaces> objs{};  // guest pointers, 0 until asked for
  uint32_t state = State_Stopped;
  bool complete_pending = false;
  uint32_t event = 0;  // IMediaEvent::GetEventHandle: set while an event waits
  long volume = 0;     // IBasicAudio, in 1/100 dB (0 = full)
  long balance = 0;
};

uint32_t VtableFor(Iface i) {
  static const char* const kDispatch[] = {"GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames",
                                          "Invoke"};
  switch (i) {
    case kBuilder:
      return com::Vtable(kDll, "IGraphBuilder",
                         {"QueryInterface", "AddRef", "Release", "AddFilter", "RemoveFilter",
                          "EnumFilters", "FindFilterByName", "ConnectDirect", "Reconnect",
                          "Disconnect", "SetDefaultSyncSource", "Connect", "Render",
                          "RenderFile", "AddSourceFilter", "SetLogFile", "Abort",
                          "ShouldOperationContinue"});
    case kControl:
      return com::Vtable(kDll, "IMediaControl",
                         {"QueryInterface", "AddRef", "Release", kDispatch[0], kDispatch[1],
                          kDispatch[2], kDispatch[3], "Run", "Pause", "Stop", "GetState",
                          "RenderFile", "AddSourceFilter", "get_FilterCollection",
                          "get_RegFilterCollection", "StopWhenReady"});
    case kEvent:
      return com::Vtable(kDll, "IMediaEventEx",
                         {"QueryInterface", "AddRef", "Release", kDispatch[0], kDispatch[1],
                          kDispatch[2], kDispatch[3], "GetEventHandle", "GetEvent",
                          "WaitForCompletion", "CancelDefaultHandling",
                          "RestoreDefaultHandling", "FreeEventParams", "SetNotifyWindow",
                          "SetNotifyFlags", "GetNotifyFlags"});
    case kSeeking:
      return com::Vtable(kDll, "IMediaSeeking",
                         {"QueryInterface", "AddRef", "Release", "GetCapabilities",
                          "CheckCapabilities", "IsFormatSupported", "QueryPreferredFormat",
                          "GetTimeFormat", "IsUsingTimeFormat", "SetTimeFormat", "GetDuration",
                          "GetStopPosition", "GetCurrentPosition", "ConvertTimeFormat",
                          "SetPositions", "GetPositions", "GetAvailable", "SetRate", "GetRate",
                          "GetPreroll"});
    case kAudio:
      return com::Vtable(kDll, "IBasicAudio",
                         {"QueryInterface", "AddRef", "Release", kDispatch[0], kDispatch[1],
                          kDispatch[2], kDispatch[3], "put_Volume", "get_Volume",
                          "put_Balance", "get_Balance"});
    case kIfaces: break;
  }
  return 0;
}

// The graph's guest object for interface `i`, with a reference for the
// caller.
uint32_t Get(const std::shared_ptr<Graph>& g, Iface i) {
  std::lock_guard<std::mutex> l(g->mu);
  if (g->objs[i]) {
    com::AddRef(g->objs[i]);
    return g->objs[i];
  }
  auto f = std::make_shared<Facade>();
  f->graph = g;
  return g->objs[i] = com::Publish(f, VtableFor(i));
}

// The interface a guest IID names, or kIfaces.
Iface IfaceFor(uint32_t iid) {
  if (com::GuidEq(iid, kIID_IUnknown) || com::GuidEq(iid, kIID_IGraphBuilder) ||
      com::GuidEq(iid, kIID_IFilterGraph))
    return kBuilder;
  if (com::GuidEq(iid, kIID_IMediaControl)) return kControl;
  if (com::GuidEq(iid, kIID_IMediaEvent) || com::GuidEq(iid, kIID_IMediaEventEx)) return kEvent;
  if (com::GuidEq(iid, kIID_IMediaSeeking)) return kSeeking;
  if (com::GuidEq(iid, kIID_IBasicAudio)) return kAudio;
  return kIfaces;
}

std::shared_ptr<Graph> Self(Ctx& c, const char* iface) {
  auto* f = com::As<Facade>(Arg(c, 0));
  if (!f) FatalF("%s call on unknown object %08X", iface, uint32_t(Arg(c, 0)));
  return f->graph;
}

uint32_t QueryInterface(Ctx& c, const char* iface) {
  auto g = Self(c, iface);
  const uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  const Iface i = IfaceFor(Arg(c, 1));
  if (i == kIfaces) {
    const GUID iid = com::ReadGuid(Arg(c, 1));
    WarnF("quartz: %s::QueryInterface({%08X-...}): not available\n", iface, iid.Data1);
    Wr32(out, 0);
    return E_NOINTERFACE;
  }
  Wr32(out, Get(g, i));
  return S_OK;
}

uint32_t CreateFilterGraph(uint32_t iid, uint32_t* out) {
  const Iface i = IfaceFor(iid);
  if (i == kIfaces) {
    *out = 0;
    return E_NOINTERFACE;
  }
  *out = Get(std::make_shared<Graph>(), i);
  return S_OK;
}

com::CoClassRegistrar reg_filter_graph{kCLSID_FilterGraph, &CreateFilterGraph};

// AddSourceFilter's filter: one output pin, which Render "renders" (there
// is no stream to connect). Games walk its pins to find the output one.
constexpr GUID kIID_IBaseFilter = {0x56A86895, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IPin = {0x56A86891, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IEnumPins = {0x56A86892, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr uint32_t PINDIR_OUTPUT = 1;
constexpr uint32_t VFW_E_NOT_FOUND = 0x80040216;
const char16_t kOutputPin[] = u"Output";

struct Source : com::Object {
  std::u16string name;  // as AddSourceFilter named it
  uint32_t pin = 0;     // the output pin's guest pointer (the filter holds it)
};
struct Pin : com::Object {
  uint32_t filter = 0;  // the source's guest pointer (no reference: the filter owns the pin)
};
struct PinEnum : com::Object {
  uint32_t pin = 0;
  bool done = false;
};

uint32_t SourceVtable() {
  return com::Vtable(kDll, "IBaseFilter",
                     {"QueryInterface", "AddRef", "Release", "GetClassID", "Stop", "Pause",
                      "Run", "GetState", "SetSyncSource", "GetSyncSource", "EnumPins", "FindPin",
                      "QueryFilterInfo", "JoinFilterGraph", "QueryVendorInfo"});
}
uint32_t PinVtable() {
  return com::Vtable(kDll, "IPin",
                     {"QueryInterface", "AddRef", "Release", "Connect", "ReceiveConnection",
                      "Disconnect", "ConnectedTo", "ConnectionMediaType", "QueryPinInfo",
                      "QueryDirection", "QueryId", "QueryAccept", "EnumMediaTypes",
                      "QueryInternalConnections", "EndOfStream", "BeginFlush", "EndFlush",
                      "NewSegment"});
}
uint32_t PinEnumVtable() {
  return com::Vtable(kDll, "IEnumPins",
                     {"QueryInterface", "AddRef", "Release", "Next", "Skip", "Reset", "Clone"});
}

template <typename T>
T* SelfAs(Ctx& c, const char* iface) {
  T* o = com::As<T>(Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(Arg(c, 0)));
  return o;
}

uint32_t QueryOne(Ctx& c, const GUID& iid) {
  const uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  if (com::GuidEq(Arg(c, 1), iid) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr32(out, Arg(c, 0));
    return S_OK;
  }
  Wr32(out, 0);
  return E_NOINTERFACE;
}

// The graph's event handle, created on first use: set while an event waits.
uint32_t EventHandle(Graph& g) {
  if (!g.event) g.event = NewEventHandle(true, false);
  return g.event;
}

void SetComplete(Graph& g, bool pending) {
  g.complete_pending = pending;
  if (pending)
    SignalEventHandle(EventHandle(g), false);
  else if (g.event)
    ResetEventHandle(g.event);
}

}  // namespace

#define RECOMP_QUARTZ_UNKNOWN(iface, prefix)                                                  \
  HLE_NAMED(kDll, #iface "::QueryInterface", prefix##_QueryInterface, 3) {                    \
    return QueryInterface(c, #iface);                                                         \
  }                                                                                           \
  HLE_NAMED(kDll, #iface "::AddRef", prefix##_AddRef, 1) { return com::AddRef(Arg(c, 0)); }   \
  HLE_NAMED(kDll, #iface "::Release", prefix##_Release, 1) { return com::Release(Arg(c, 0)); }
RECOMP_QUARTZ_UNKNOWN(IGraphBuilder, Gb)
RECOMP_QUARTZ_UNKNOWN(IMediaControl, Mc)
RECOMP_QUARTZ_UNKNOWN(IMediaEventEx, Me)
RECOMP_QUARTZ_UNKNOWN(IMediaSeeking, Ms)
RECOMP_QUARTZ_UNKNOWN(IBasicAudio, Ba)
#undef RECOMP_QUARTZ_UNKNOWN

// ---- IGraphBuilder ------------------------------------------------------------

// (IBaseFilter*, LPCWSTR name): the filter is not used (nothing plays), so
// the graph keeps no reference.
HLE_NAMED(kDll, "IGraphBuilder::AddFilter", Gb_AddFilter, 3) {
  Self(c, "IGraphBuilder");
  RECOMP_TRACE("  quartz: AddFilter(%08X, %s)\n", uint32_t(Arg(c, 1)),
               Arg(c, 2) ? win32::Narrow(GWStr(Arg(c, 2))).c_str() : "");
  return S_OK;
}
HLE_NAMED(kDll, "IGraphBuilder::RemoveFilter", Gb_RemoveFilter, 2) {
  Self(c, "IGraphBuilder");
  return S_OK;
}

// (LPCWSTR file, LPCWSTR filter_name, IBaseFilter** out): the file is not
// read; the filter's output pin renders nothing.
HLE_NAMED(kDll, "IGraphBuilder::AddSourceFilter", Gb_AddSourceFilter, 4) {
  Self(c, "IGraphBuilder");
  const uint32_t out = Arg(c, 3);
  if (!out) return E_POINTER;
  RECOMP_TRACE("  quartz: AddSourceFilter(%s)\n",
               Arg(c, 1) ? win32::Narrow(GWStr(Arg(c, 1))).c_str() : "");
  auto s = std::make_shared<Source>();
  if (Arg(c, 2)) s->name = GWStr(Arg(c, 2));
  const uint32_t filter = com::Publish(s, SourceVtable());
  auto p = std::make_shared<Pin>();
  p->filter = filter;
  s->pin = com::Publish(p, PinVtable());
  Wr32(out, filter);
  return S_OK;
}

// (IPin* output): nothing to connect it to.
HLE_NAMED(kDll, "IGraphBuilder::Render", Gb_Render, 2) {
  Self(c, "IGraphBuilder");
  return S_OK;
}

// (LPCWSTR file, LPCWSTR playlist)
HLE_NAMED(kDll, "IGraphBuilder::RenderFile", Gb_RenderFile, 3) {
  Self(c, "IGraphBuilder");
  RECOMP_TRACE("  quartz: RenderFile(%s)\n",
               Arg(c, 1) ? win32::Narrow(GWStr(Arg(c, 1))).c_str() : "");
  return S_OK;
}

// ---- the source filter ----------------------------------------------------------

HLE_NAMED(kDll, "IBaseFilter::QueryInterface", Sf_QueryInterface, 3) {
  return QueryOne(c, kIID_IBaseFilter);
}
HLE_NAMED(kDll, "IBaseFilter::AddRef", Sf_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED(kDll, "IBaseFilter::Release", Sf_Release, 1) {
  const uint32_t self = Arg(c, 0);
  const uint32_t pin = SelfAs<Source>(c, "IBaseFilter")->pin;
  const uint32_t left = com::Release(self);
  if (!left) com::Release(pin);
  return left;
}
// (IEnumPins**)
HLE_NAMED(kDll, "IBaseFilter::EnumPins", Sf_EnumPins, 2) {
  Source* s = SelfAs<Source>(c, "IBaseFilter");
  if (!Arg(c, 1)) return E_POINTER;
  auto e = std::make_shared<PinEnum>();
  e->pin = s->pin;
  Wr32(Arg(c, 1), com::Publish(e, PinEnumVtable()));
  return S_OK;
}
// (LPCWSTR id, IPin**)
HLE_NAMED(kDll, "IBaseFilter::FindPin", Sf_FindPin, 3) {
  Source* s = SelfAs<Source>(c, "IBaseFilter");
  const uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  if (!Arg(c, 1) || GWStr(Arg(c, 1)) != kOutputPin) {
    Wr32(out, 0);
    return VFW_E_NOT_FOUND;
  }
  com::AddRef(s->pin);
  Wr32(out, s->pin);
  return S_OK;
}

HLE_NAMED(kDll, "IEnumPins::QueryInterface", Ep_QueryInterface, 3) {
  return QueryOne(c, kIID_IEnumPins);
}
HLE_NAMED(kDll, "IEnumPins::AddRef", Ep_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED(kDll, "IEnumPins::Release", Ep_Release, 1) { return com::Release(Arg(c, 0)); }
// (ULONG count, IPin** pins, ULONG* fetched)
HLE_NAMED(kDll, "IEnumPins::Next", Ep_Next, 4) {
  PinEnum* e = SelfAs<PinEnum>(c, "IEnumPins");
  const uint32_t n = Arg(c, 1), pins = Arg(c, 2), fetched = Arg(c, 3);
  if (!pins || (n > 1 && !fetched)) return E_POINTER;
  uint32_t got = 0;
  if (n && !e->done) {
    com::AddRef(e->pin);
    Wr32(pins, e->pin);
    e->done = true;
    got = 1;
  }
  if (fetched) Wr32(fetched, got);
  return got == n ? S_OK : S_FALSE;
}
HLE_NAMED(kDll, "IEnumPins::Reset", Ep_Reset, 1) {
  SelfAs<PinEnum>(c, "IEnumPins")->done = false;
  return S_OK;
}

HLE_NAMED(kDll, "IPin::QueryInterface", Pn_QueryInterface, 3) { return QueryOne(c, kIID_IPin); }
HLE_NAMED(kDll, "IPin::AddRef", Pn_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED(kDll, "IPin::Release", Pn_Release, 1) { return com::Release(Arg(c, 0)); }
// (PIN_INFO*): {IBaseFilter* (with a reference), PIN_DIRECTION, WCHAR[128]}.
HLE_NAMED(kDll, "IPin::QueryPinInfo", Pn_QueryPinInfo, 2) {
  Pin* p = SelfAs<Pin>(c, "IPin");
  const uint32_t info = Arg(c, 1);
  if (!info) return E_POINTER;
  com::AddRef(p->filter);
  Wr32(info, p->filter);
  Wr32(info + 4, PINDIR_OUTPUT);
  for (size_t i = 0; i < 128; i++)
    Wr16(info + 8 + 2 * uint32_t(i), i < std::size(kOutputPin) ? uint16_t(kOutputPin[i]) : 0);
  return S_OK;
}
HLE_NAMED(kDll, "IPin::QueryDirection", Pn_QueryDirection, 2) {
  SelfAs<Pin>(c, "IPin");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), PINDIR_OUTPUT);
  return S_OK;
}

// ---- IMediaControl ------------------------------------------------------------

HLE_NAMED(kDll, "IMediaControl::Run", Mc_Run, 1) {
  auto g = Self(c, "IMediaControl");
  std::lock_guard<std::mutex> l(g->mu);
  if (g->state != State_Running) SetComplete(*g, true);
  g->state = State_Running;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::Pause", Mc_Pause, 1) {
  auto g = Self(c, "IMediaControl");
  std::lock_guard<std::mutex> l(g->mu);
  g->state = State_Paused;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::Stop", Mc_Stop, 1) {
  auto g = Self(c, "IMediaControl");
  std::lock_guard<std::mutex> l(g->mu);
  g->state = State_Stopped;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::StopWhenReady", Mc_StopWhenReady, 1) {
  auto g = Self(c, "IMediaControl");
  std::lock_guard<std::mutex> l(g->mu);
  g->state = State_Stopped;
  return S_OK;
}
// (msTimeout, OAFilterState*): no state change is ever in progress.
HLE_NAMED(kDll, "IMediaControl::GetState", Mc_GetState, 3) {
  auto g = Self(c, "IMediaControl");
  if (!Arg(c, 2)) return E_POINTER;
  std::lock_guard<std::mutex> l(g->mu);
  Wr32(Arg(c, 2), g->state);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::RenderFile", Mc_RenderFile, 2) {
  Self(c, "IMediaControl");
  return S_OK;
}

// ---- IMediaEventEx ------------------------------------------------------------

// (OAEVENT*): a manual-reset event, set while an event waits.
HLE_NAMED(kDll, "IMediaEventEx::GetEventHandle", Me_GetEventHandle, 2) {
  auto g = Self(c, "IMediaEventEx");
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g->mu);
  Wr32(Arg(c, 1), EventHandle(*g));
  return S_OK;
}
// (long* code, LONG_PTR* p1, LONG_PTR* p2, long msTimeout): EC_COMPLETE
// once after Run, else E_ABORT (the timeout passing without an event: no
// other event comes, so there is nothing to wait for).
HLE_NAMED(kDll, "IMediaEventEx::GetEvent", Me_GetEvent, 5) {
  auto g = Self(c, "IMediaEventEx");
  const uint32_t code = Arg(c, 1), p1 = Arg(c, 2), p2 = Arg(c, 3);
  if (!code || !p1 || !p2) return E_POINTER;
  std::lock_guard<std::mutex> l(g->mu);
  if (!g->complete_pending) return E_ABORT;
  SetComplete(*g, false);
  Wr32(code, EC_COMPLETE);
  Wr32(p1, S_OK);
  Wr32(p2, 0);
  return S_OK;
}
// (long msTimeout, long* code): complete once running.
HLE_NAMED(kDll, "IMediaEventEx::WaitForCompletion", Me_WaitForCompletion, 3) {
  auto g = Self(c, "IMediaEventEx");
  if (!Arg(c, 2)) return E_POINTER;
  std::lock_guard<std::mutex> l(g->mu);
  if (g->state != State_Running) {
    Wr32(Arg(c, 2), 0);
    return E_ABORT;
  }
  Wr32(Arg(c, 2), EC_COMPLETE);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaEventEx::FreeEventParams", Me_FreeEventParams, 4) {
  Self(c, "IMediaEventEx");
  return S_OK;
}
HLE_NAMED(kDll, "IMediaEventEx::CancelDefaultHandling", Me_CancelDefaultHandling, 2) {
  Self(c, "IMediaEventEx");
  return S_OK;
}
// (OAHWND, long msg, LONG_PTR param): events are read with GetEvent; no
// window message is ever posted.
HLE_NAMED(kDll, "IMediaEventEx::SetNotifyWindow", Me_SetNotifyWindow, 4) {
  Self(c, "IMediaEventEx");
  return S_OK;
}

// ---- IMediaSeeking ------------------------------------------------------------
// The media has no length: positions and durations are 0.

HLE_NAMED(kDll, "IMediaSeeking::GetDuration", Ms_GetDuration, 2) {
  Self(c, "IMediaSeeking");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), 0);
  Wr32(Arg(c, 1) + 4, 0);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSeeking::GetCurrentPosition", Ms_GetCurrentPosition, 2) {
  Self(c, "IMediaSeeking");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), 0);
  Wr32(Arg(c, 1) + 4, 0);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSeeking::GetStopPosition", Ms_GetStopPosition, 2) {
  Self(c, "IMediaSeeking");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), 0);
  Wr32(Arg(c, 1) + 4, 0);
  return S_OK;
}
// (LONGLONG* current, DWORD current_flags, LONGLONG* stop, DWORD stop_flags)
HLE_NAMED(kDll, "IMediaSeeking::SetPositions", Ms_SetPositions, 5) {
  Self(c, "IMediaSeeking");
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSeeking::SetRate", Ms_SetRate, 3) {
  Self(c, "IMediaSeeking");
  return S_OK;
}

// ---- IBasicAudio --------------------------------------------------------------

HLE_NAMED(kDll, "IBasicAudio::put_Volume", Ba_put_Volume, 2) {
  auto g = Self(c, "IBasicAudio");
  std::lock_guard<std::mutex> l(g->mu);
  g->volume = int32_t(Arg(c, 1));
  return S_OK;
}
HLE_NAMED(kDll, "IBasicAudio::get_Volume", Ba_get_Volume, 2) {
  auto g = Self(c, "IBasicAudio");
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g->mu);
  Wr32(Arg(c, 1), uint32_t(g->volume));
  return S_OK;
}
HLE_NAMED(kDll, "IBasicAudio::put_Balance", Ba_put_Balance, 2) {
  auto g = Self(c, "IBasicAudio");
  std::lock_guard<std::mutex> l(g->mu);
  g->balance = int32_t(Arg(c, 1));
  return S_OK;
}
HLE_NAMED(kDll, "IBasicAudio::get_Balance", Ba_get_Balance, 2) {
  auto g = Self(c, "IBasicAudio");
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g->mu);
  Wr32(Arg(c, 1), uint32_t(g->balance));
  return S_OK;
}

}  // namespace recomp::hle
