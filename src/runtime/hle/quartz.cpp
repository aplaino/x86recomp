// DirectShow's filter graph manager (quartz.dll, CLSID_FilterGraph) for
// games that play their movies through it, as the common case does: a
// source filter for a file (AddSourceFilter) rendered into a video
// renderer filter of the game's own (AddFilter; a DirectShow base-classes
// renderer that takes RGB24 frames and copies them into a texture).
//
// With ffmpeg (RECOMP_QUARTZ_VIDEO) the graph plays the file: Render
// connects the source's output pin to the game's renderer as RGB24
// VIDEOINFO, with the renderer's own allocator; Run starts decoding on a
// host thread and the sound on the audio mixer; and each time the game
// polls the graph (IMediaEvent::GetEvent, IMediaControl::GetState,
// IMediaSeeking::GetCurrentPosition) the frame due by then goes to the
// renderer's IMemInputPin::Receive on the game's own thread, which renders
// it at once (no reference clock is set). EC_COMPLETE follows the last
// frame. Where any of that fails (no ffmpeg, a file it cannot read, a
// renderer that refuses the format) the graph plays nothing: Run queues
// EC_COMPLETE at once and the game goes on as after the movie.
//
// The graph's interfaces (IGraphBuilder, IMediaControl, IMediaEvent,
// IMediaSeeking, IBasicAudio) are each a guest object over one shared
// Graph, as com.h has one guest pointer per interface; QueryInterface on
// any of them returns the graph's object for the requested one.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef RECOMP_QUARTZ_VIDEO
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include "runtime/audio/mixer.h"
#include "runtime/audio/source.h"
#endif

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

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
  Iface iface = kBuilder;
  ~Facade() override;
};

struct Movie;

struct Graph : std::enable_shared_from_this<Graph> {
  std::mutex mu;
  std::array<uint32_t, kIfaces> objs{};  // guest pointers, 0 until asked for
  uint32_t state = State_Stopped;
  bool complete_pending = false;
  uint32_t event = 0;  // IMediaEvent::GetEventHandle: set while an event waits
  long volume = 0;     // IBasicAudio, in 1/100 dB (0 = full)
  long balance = 0;
  // Playback (see the top of the file). The guest objects hold references
  // taken here and dropped by Stop.
  std::string file;          // the source filter's file, as a host path
  uint32_t renderer = 0;     // the game's renderer filter (AddFilter)
  uint32_t in_pin = 0;       // its input pin, connected to our source's
  uint32_t mem_pin = 0;      // that pin's IMemInputPin
  uint32_t allocator = 0;    // its IMemAllocator
  uint32_t out_pin = 0;      // our source's output pin (the guest pointer)
  std::unique_ptr<Movie> movie;  // open once connected
  bool playing = false;
};

// The graph's cached pointer goes with the object (Release destroys host
// objects outside the COM table's lock, so this may take the graph's).
Facade::~Facade() {
  std::lock_guard<std::mutex> l(graph->mu);
  if (graph->objs[iface] == guest) graph->objs[iface] = 0;
}

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
  // Only while it is still this graph's live object: once released, its
  // guest block may hold another object.
  if (g->objs[i]) {
    auto* f = com::As<Facade>(g->objs[i]);
    if (f && f->graph == g && f->iface == i) {
      com::AddRef(g->objs[i]);
      return g->objs[i];
    }
    g->objs[i] = 0;
  }
  auto f = std::make_shared<Facade>();
  f->graph = g;
  f->iface = i;
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
// The pin and enumerators may outlive the filter (it owns the pin, not
// the other way round): they reach it through a weak reference.
struct Pin : com::Object {
  std::weak_ptr<Source> filter;
};
struct PinEnum : com::Object {
  std::weak_ptr<Source> filter;
  bool done = false;
};

// The live source behind a weak reference, or null once it is released.
std::shared_ptr<Source> Live(const std::weak_ptr<Source>& w) {
  auto s = w.lock();
  return s && com::Find(s->guest) == s.get() ? s : nullptr;
}

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

// ---- the memory allocator (CLSID_MemoryAllocator) ---------------------------
// What a DirectShow base-classes input pin creates in GetAllocator: a pool
// of samples over guest buffers. A sample whose last reference goes comes
// back to the pool (it is not destroyed): the pool holds a reference of
// its own on each.

constexpr GUID kCLSID_MemoryAllocator = {0x1E651CC0, 0xB199, 0x11D0, {0x82, 0x12, 0x00, 0xC0, 0x4F, 0xC3, 0x2C, 0x45}};
constexpr GUID kIID_IMemAllocator = {0x56A8689C, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IMediaSample = {0x56A8689A, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr uint32_t VFW_E_BUFFER_OVERFLOW = 0x8004020D, VFW_E_ALREADY_COMMITTED = 0x8004020F,
                   VFW_E_NOT_COMMITTED = 0x80040211, VFW_E_SIZENOTSET = 0x80040212,
                   VFW_E_TIMEOUT = 0x8004022E, E_INVALIDARG = 0x80070057;

struct Allocator;
struct Sample : com::Object {
  std::weak_ptr<Allocator> owner;
  uint32_t buffer = 0, size = 0, actual = 0;
  int64_t start = 0, end = 0;
  bool times = false, sync = false, preroll = false, discontinuity = false;
  bool lent = false;  // handed out by GetBuffer, not back yet
};
struct Allocator : com::Object {
  std::mutex mu;
  uint32_t count = 0, size = 0, align = 1, prefix = 0;
  bool committed = false;
  std::vector<uint32_t> samples;  // guest pointers; the pool's references
};

uint32_t AllocatorVtable() {
  return com::Vtable(kDll, "IMemAllocator",
                     {"QueryInterface", "AddRef", "Release", "SetProperties", "GetProperties",
                      "Commit", "Decommit", "GetBuffer", "ReleaseBuffer"});
}
uint32_t SampleVtable() {
  return com::Vtable(kDll, "IMediaSample",
                     {"QueryInterface", "AddRef", "Release", "GetPointer", "GetSize", "GetTime",
                      "SetTime", "IsSyncPoint", "SetSyncPoint", "IsPreroll", "SetPreroll",
                      "GetActualDataLength", "SetActualDataLength", "GetMediaType",
                      "SetMediaType", "IsDiscontinuity", "SetDiscontinuity", "GetMediaTime",
                      "SetMediaTime"});
}

uint32_t CreateMemoryAllocator(uint32_t iid, uint32_t* out) {
  if (!com::GuidEq(iid, kIID_IMemAllocator) && !com::GuidEq(iid, kIID_IUnknown)) {
    *out = 0;
    return E_NOINTERFACE;
  }
  *out = com::Publish(std::make_shared<Allocator>(), AllocatorVtable());
  return S_OK;
}
com::CoClassRegistrar reg_memory_allocator{kCLSID_MemoryAllocator, &CreateMemoryAllocator};

// A free sample of `a`, lent out with a reference for the caller; 0 if
// all are out.
uint32_t Lend(Allocator& a) {
  std::lock_guard<std::mutex> l(a.mu);
  if (!a.committed) return 0;
  for (uint32_t g : a.samples) {
    auto* s = com::As<Sample>(g);
    if (s && !s->lent) {
      s->lent = true;
      s->times = s->sync = s->preroll = s->discontinuity = false;
      s->actual = s->size;
      com::AddRef(g);
      return g;
    }
  }
  return 0;
}

// ---- playback ---------------------------------------------------------------

// A method of a guest COM object (its vtable's slot), from an HLE handler
// on the calling guest thread.
uint32_t Call(Ctx& c, uint32_t obj, int slot, std::initializer_list<uint64_t> rest = {}) {
  std::vector<uint64_t> args{obj};
  args.insert(args.end(), rest.begin(), rest.end());
  const uint32_t fn = Rd32(Rd32(obj) + 4 * uint32_t(slot));
  switch (args.size()) {
    case 1: return uint32_t(CallGuest(c, fn, {args[0]}));
    case 2: return uint32_t(CallGuest(c, fn, {args[0], args[1]}));
    case 3: return uint32_t(CallGuest(c, fn, {args[0], args[1], args[2]}));
    case 4: return uint32_t(CallGuest(c, fn, {args[0], args[1], args[2], args[3]}));
    default: return uint32_t(CallGuest(c, fn, {args[0], args[1], args[2], args[3], args[4]}));
  }
}
// Vtable slots of the interfaces the graph calls.
enum : int {
  kQueryInterface = 0, kAddRef = 1, kRelease = 2,
  kFilterStop = 4, kFilterPause = 5, kFilterRun = 6, kFilterSetSyncSource = 8,
  kFilterEnumPins = 10,
  kEnumNext = 3,
  kPinReceiveConnection = 4, kPinDisconnect = 5, kPinQueryDirection = 9,
  kPinEndOfStream = 14,
  kMemGetAllocator = 3, kMemNotifyAllocator = 4, kMemReceive = 6,
  kAllocSetProperties = 3, kAllocCommit = 5, kAllocDecommit = 6, kAllocGetBuffer = 7,
  kSampleGetPointer = 3, kSampleGetSize = 4, kSampleSetTime = 6, kSampleSetSyncPoint = 8,
  kSampleSetActualDataLength = 12,
};

constexpr GUID kIID_IMemInputPin = {0x56A8689D, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kMEDIATYPE_Video = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMEDIASUBTYPE_RGB24 = {0xE436EB7D, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kFORMAT_VideoInfo = {0x05589F80, 0xC356, 0x11CE, {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};
constexpr uint32_t PINDIR_INPUT = 0;

void WrGuid(uint32_t va, const GUID& g) { std::memcpy(GPtr(va), &g, sizeof g); }

#ifdef RECOMP_QUARTZ_VIDEO
// The file, decoded on a host thread a few frames ahead into RGB24 rows
// bottom-up (a DIB, as VIDEOINFO says), and its sound into a mixer voice.
struct Movie {
  struct Frame {
    int64_t pts_us;
    std::vector<uint8_t> rgb;
  };
  AVFormatContext* fmt = nullptr;
  AVCodecContext* vdec = nullptr;
  AVCodecContext* adec = nullptr;
  SwsContext* sws = nullptr;
  int vstream = -1, astream = -1;
  int width = 0, height = 0, stride = 0;  // stride: rows padded to 4 bytes
  int64_t frame_us = 33333;               // nominal frame duration
  int64_t duration_us = 0;

  std::mutex mu;
  std::condition_variable cv;
  std::deque<Frame> frames;
  bool eof = false;
  std::atomic<bool> quit{false};
  std::thread thread;

  audio::QueueSource* queue = nullptr;  // owned by voice
  std::unique_ptr<audio::Voice> voice;

  // The playback clock: wall time while running.
  std::chrono::steady_clock::time_point started;
  int64_t paused_at_us = 0;  // the position while not running
  bool running = false;
  int64_t last_pts_us = 0;   // the last frame taken

  bool Open(const std::string& path) {
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
    vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vstream < 0) return false;
    vdec = OpenDecoder(fmt->streams[vstream]);
    if (!vdec) return false;
    if (astream >= 0) adec = OpenDecoder(fmt->streams[astream]);
    width = vdec->width;
    height = vdec->height;
    stride = (width * 3 + 3) & ~3;
    const AVRational fr = fmt->streams[vstream]->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0) frame_us = int64_t(1e6 * fr.den / fr.num);
    if (fmt->duration > 0) duration_us = fmt->duration;  // AV_TIME_BASE is 1e6
    return width > 0 && height > 0;
  }

  static AVCodecContext* OpenDecoder(AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c || avcodec_parameters_to_context(c, st->codecpar) < 0 ||
        avcodec_open2(c, codec, nullptr) < 0) {
      avcodec_free_context(&c);
      return nullptr;
    }
    return c;
  }

  void Start(float volume) {
    if (adec) {
      const int ch = std::min(adec->ch_layout.nb_channels, 2);
      auto q = std::make_unique<audio::QueueSource>(ch, adec->sample_rate);
      queue = q.get();
      voice = std::make_unique<audio::Voice>();
      voice->src = std::move(q);
      voice->dsound = true;  // linear volume, centred (pan 0)
      voice->pan = 0;
      std::lock_guard<std::mutex> l(audio::GetMixer().mu());
      voice->volume = volume;
      voice->state = audio::Voice::State::Playing;
      audio::GetMixer().Add(voice.get());
    }
    thread = std::thread([this] { Decode(); });
    started = std::chrono::steady_clock::now();
    running = true;
  }

  int64_t PositionUs() const {
    if (!running) return paused_at_us;
    return paused_at_us + std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - started).count();
  }

  void SetRunning(bool run) {
    if (run == running) return;
    if (!run) paused_at_us = PositionUs();
    else started = std::chrono::steady_clock::now();
    running = run;
    if (voice) {
      std::lock_guard<std::mutex> l(audio::GetMixer().mu());
      voice->state = run ? audio::Voice::State::Playing : audio::Voice::State::Paused;
    }
  }

  void SetVolume(float v) {
    if (!voice) return;
    std::lock_guard<std::mutex> l(audio::GetMixer().mu());
    voice->volume = v;
  }

  // The newest frame due by now (older due ones are dropped), if any.
  bool TakeDue(Frame* out) {
    const int64_t now = PositionUs();
    std::lock_guard<std::mutex> l(mu);
    bool got = false;
    while (!frames.empty() && frames.front().pts_us <= now) {
      *out = std::move(frames.front());
      frames.pop_front();
      last_pts_us = out->pts_us;
      got = true;
    }
    if (got) cv.notify_all();
    return got;
  }

  // Every frame decoded and shown for its duration.
  bool Finished() {
    std::lock_guard<std::mutex> l(mu);
    return eof && frames.empty() && PositionUs() >= last_pts_us + frame_us;
  }

  void Decode() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    auto push_video = [&](AVFrame* fr) {
      sws = sws_getCachedContext(sws, fr->width, fr->height, AVPixelFormat(fr->format),
                                 width, height, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr,
                                 nullptr, nullptr);
      if (!sws) return;
      Frame out;
      out.rgb.resize(size_t(stride) * height);
      // Bottom-up: the first row out is the last row of the buffer.
      uint8_t* dst[1] = {out.rgb.data() + size_t(stride) * (height - 1)};
      int dst_stride[1] = {-stride};
      sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, dst_stride);
      const AVRational tb = fmt->streams[vstream]->time_base;
      const int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : 0;
      out.pts_us = av_rescale_q(ts, tb, AVRational{1, 1000000}) -
                   (fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0);
      std::unique_lock<std::mutex> l(mu);
      cv.wait(l, [&] { return frames.size() < 8 || quit; });
      if (!quit) frames.push_back(std::move(out));
    };
    auto drain = [&](AVCodecContext* dec, bool video) {
      while (avcodec_receive_frame(dec, f) == 0) {
        if (video) {
          push_video(f);
        } else if (queue) {
          const int ch = queue->channels();
          std::vector<float> buf(size_t(f->nb_samples) * ch);
          audio::ConvertFrame(f, ch, buf.data());
          queue->Push(buf.data(), f->nb_samples);
        }
        av_frame_unref(f);
      }
    };
    while (!quit && av_read_frame(fmt, pkt) >= 0) {
      if (pkt->stream_index == vstream) {
        if (avcodec_send_packet(vdec, pkt) >= 0) drain(vdec, true);
      } else if (pkt->stream_index == astream && adec) {
        if (avcodec_send_packet(adec, pkt) >= 0) drain(adec, false);
      }
      av_packet_unref(pkt);
    }
    if (!quit) {
      avcodec_send_packet(vdec, nullptr);
      drain(vdec, true);
      if (adec) {
        avcodec_send_packet(adec, nullptr);
        drain(adec, false);
      }
    }
    av_frame_free(&f);
    av_packet_free(&pkt);
    std::lock_guard<std::mutex> l(mu);
    eof = true;
  }

  ~Movie() {
    quit = true;
    cv.notify_all();
    if (thread.joinable()) thread.join();
    if (voice) {
      std::lock_guard<std::mutex> l(audio::GetMixer().mu());
      audio::GetMixer().Remove(voice.get());
    }
    sws_freeContext(sws);
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    avformat_close_input(&fmt);
  }
};
#else
struct Movie {};
#endif

// IBasicAudio's volume (1/100 dB, 0 full, -10000 silent) as a gain.
float Gain(long volume) { return volume <= -10000 ? 0.0f : std::pow(10.0f, float(volume) / 2000.0f); }

// Releases what the graph holds of the game's objects (on the guest thread
// that stops or tears down the graph).
void ReleaseGuest(Ctx& c, Graph& g) {
  if (g.allocator) Call(c, g.allocator, kRelease);
  if (g.mem_pin) Call(c, g.mem_pin, kRelease);
  if (g.in_pin) Call(c, g.in_pin, kRelease);
  if (g.renderer) Call(c, g.renderer, kRelease);
  g.allocator = g.mem_pin = g.in_pin = g.renderer = 0;
}

#ifdef RECOMP_QUARTZ_VIDEO
// Render: our output pin to the game's renderer, RGB24 VIDEOINFO with the
// renderer's allocator. False (and nothing kept) where it does not work.
bool Connect(Ctx& c, Graph& g) {
  if (!g.renderer || g.file.empty() || !g.out_pin) return false;
  auto m = std::make_unique<Movie>();
  if (!m->Open(g.file)) {
    WarnF("quartz: cannot play %s\n", g.file.c_str());
    return false;
  }
  const uint32_t scratch = HeapAllocProcess(256, true);
  auto done = [&](bool ok) {
    HeapFreeProcess(scratch);
    return ok;
  };
  // The renderer's input pin.
  uint32_t in = 0;
  if (const uint32_t hr = Call(c, g.renderer, kFilterEnumPins, {scratch}); hr != S_OK) {
    WarnF("quartz: the renderer's EnumPins failed (%08X)\n", hr);
  } else {
    const uint32_t e = Rd32(scratch);
    while (!in && Call(c, e, kEnumNext, {1, scratch + 4, scratch + 8}) == S_OK) {
      const uint32_t pin = Rd32(scratch + 4);
      if (Call(c, pin, kPinQueryDirection, {scratch + 12}) == S_OK &&
          Rd32(scratch + 12) == PINDIR_INPUT)
        in = pin;
      else
        Call(c, pin, kRelease);
    }
    Call(c, e, kRelease);
  }
  if (!in) {
    WarnF("quartz: the renderer has no input pin\n");
    return done(false);
  }
  // AM_MEDIA_TYPE (72 bytes) and its VIDEOINFOHEADER (88) after it.
  const uint32_t mt = scratch + 32, vih = mt + 72;
  const uint32_t size = uint32_t(m->stride) * uint32_t(m->height);
  std::memset(GPtr(mt), 0, 72 + 88);
  WrGuid(mt, kMEDIATYPE_Video);
  WrGuid(mt + 16, kMEDIASUBTYPE_RGB24);
  Wr32(mt + 32, 1);  // bFixedSizeSamples
  Wr32(mt + 36, 0);  // bTemporalCompression
  Wr32(mt + 40, size);
  WrGuid(mt + 44, kFORMAT_VideoInfo);
  Wr32(mt + 64, 88);  // cbFormat
  Wr32(mt + 68, vih);
  Wr32(vih + 8, uint32_t(m->width));  // rcSource
  Wr32(vih + 12, uint32_t(m->height));
  Wr32(vih + 24, uint32_t(m->width));  // rcTarget
  Wr32(vih + 28, uint32_t(m->height));
  Wr32(vih + 40, uint32_t(m->frame_us * 10));  // AvgTimePerFrame (100 ns)
  Wr32(vih + 44, uint32_t((m->frame_us * 10) >> 32));
  Wr32(vih + 48, 40);  // BITMAPINFOHEADER: biSize, width, height, planes, bits
  Wr32(vih + 52, uint32_t(m->width));
  Wr32(vih + 56, uint32_t(m->height));
  Wr16(vih + 60, 1);
  Wr16(vih + 62, 24);
  Wr32(vih + 68, size);  // biSizeImage
  if (const uint32_t hr = Call(c, in, kPinReceiveConnection, {g.out_pin, mt}); hr != S_OK) {
    WarnF("quartz: the renderer refused RGB24 %dx%d (%08X)\n", m->width, m->height, hr);
    Call(c, in, kRelease);
    return done(false);
  }
  // Its allocator, two frames.
  std::memcpy(GPtr(scratch + 16), &kIID_IMemInputPin, 16);
  if (Call(c, in, kQueryInterface, {scratch + 16, scratch}) != S_OK) {
    WarnF("quartz: the renderer's input pin has no IMemInputPin\n");
    Call(c, in, kPinDisconnect);
    Call(c, in, kRelease);
    return done(false);
  }
  const uint32_t mem = Rd32(scratch);
  if (const uint32_t hr = Call(c, mem, kMemGetAllocator, {scratch}); hr != S_OK) {
    WarnF("quartz: the renderer offers no allocator (%08X)\n", hr);
    Call(c, mem, kRelease);
    Call(c, in, kPinDisconnect);
    Call(c, in, kRelease);
    return done(false);
  }
  const uint32_t alloc = Rd32(scratch);
  if (!com::As<Allocator>(alloc)) {
    WarnF("quartz: the renderer's allocator is not the system one\n");
    Call(c, alloc, kRelease);
    Call(c, mem, kRelease);
    Call(c, in, kPinDisconnect);
    Call(c, in, kRelease);
    return done(false);
  }
  const uint32_t props = scratch + 200;  // ALLOCATOR_PROPERTIES, requested then actual
  Wr32(props, 2);
  Wr32(props + 4, size);
  Wr32(props + 8, 1);
  Wr32(props + 12, 0);
  Call(c, alloc, kAllocSetProperties, {props, props + 16});
  Call(c, mem, kMemNotifyAllocator, {alloc, 0});
  g.in_pin = in;
  g.mem_pin = mem;
  g.allocator = alloc;
  g.movie = std::move(m);
  RECOMP_TRACE("  quartz: playing %s, %dx%d\n", g.file.c_str(), g.movie->width, g.movie->height);
  return done(true);
}

// The frame due now, if any, to the renderer; EC_COMPLETE after the last.
void Pump(Ctx& c, Graph& g) {
  if (!g.playing || !g.movie || g.complete_pending) return;
  Movie& m = *g.movie;
  Movie::Frame fr;
  auto* alloc = com::As<Allocator>(g.allocator);
  if (alloc && m.TakeDue(&fr)) {
    if (const uint32_t sample = Lend(*alloc)) {
      Sample* s = com::As<Sample>(sample);
      const uint32_t n = std::min<uint32_t>(s->size, uint32_t(fr.rgb.size()));
      std::memcpy(GPtr(s->buffer), fr.rgb.data(), n);
      s->actual = n;
      s->start = fr.pts_us * 10;
      s->end = s->start + m.frame_us * 10;
      s->times = s->sync = true;
      Call(c, g.mem_pin, kMemReceive, {sample});
      Call(c, sample, kRelease);
    }
  }
  if (m.Finished()) {
    Call(c, g.in_pin, kPinEndOfStream);
    SetComplete(g, true);
  }
}
#else
bool Connect(Ctx&, Graph&) { return false; }
void Pump(Ctx&, Graph&) {}
#endif

// Stops the renderer and the decoding. The pins stay connected, as in
// DirectShow: the game's renderer keeps what it made for the connection
// (its textures) until the graph goes.
void StopPlayback(Ctx& c, Graph& g) {
  if (g.in_pin && g.playing) {
    Call(c, g.renderer, kFilterStop);
    Call(c, g.allocator, kAllocDecommit);
  }
  g.movie.reset();
  g.playing = false;
}

// The graph going (the game released its last interface): the pins are
// disconnected and the game's objects let go, as the graph manager does.
void Teardown(Ctx& c, Graph& g) {
  StopPlayback(c, g);
  if (g.in_pin) Call(c, g.in_pin, kPinDisconnect);
  ReleaseGuest(c, g);
}

}  // namespace

// The last of a graph's interfaces released: the graph goes.
uint32_t ReleaseFacade(Ctx& c, const char* iface) {
  auto g = Self(c, iface);
  const uint32_t left = com::Release(Arg(c, 0));
  if (!left) {
    bool any;
    {
      std::lock_guard<std::mutex> l(g->mu);
      any = std::any_of(g->objs.begin(), g->objs.end(), [](uint32_t o) { return o != 0; });
    }
    if (!any) Teardown(c, *g);
  }
  return left;
}

#define RECOMP_QUARTZ_UNKNOWN(iface, prefix)                                                  \
  HLE_NAMED(kDll, #iface "::QueryInterface", prefix##_QueryInterface, 3) {                    \
    return QueryInterface(c, #iface);                                                         \
  }                                                                                           \
  HLE_NAMED(kDll, #iface "::AddRef", prefix##_AddRef, 1) { return com::AddRef(Arg(c, 0)); }   \
  HLE_NAMED(kDll, #iface "::Release", prefix##_Release, 1) { return ReleaseFacade(c, #iface); }
RECOMP_QUARTZ_UNKNOWN(IGraphBuilder, Gb)
RECOMP_QUARTZ_UNKNOWN(IMediaControl, Mc)
RECOMP_QUARTZ_UNKNOWN(IMediaEventEx, Me)
RECOMP_QUARTZ_UNKNOWN(IMediaSeeking, Ms)
RECOMP_QUARTZ_UNKNOWN(IBasicAudio, Ba)
#undef RECOMP_QUARTZ_UNKNOWN

// ---- IGraphBuilder ------------------------------------------------------------

// (IBaseFilter*, LPCWSTR name): the game's renderer, kept (with a
// reference) to render the source into.
HLE_NAMED(kDll, "IGraphBuilder::AddFilter", Gb_AddFilter, 3) {
  auto g = Self(c, "IGraphBuilder");
  RECOMP_TRACE("  quartz: AddFilter(%08X, %s)\n", uint32_t(Arg(c, 1)),
               Arg(c, 2) ? win32::Narrow(GWStr(Arg(c, 2))).c_str() : "");
  const uint32_t f = Arg(c, 1);
  if (f && !com::Find(f) && !g->renderer) {
    Call(c, f, kAddRef);
    g->renderer = f;
  }
  return S_OK;
}
HLE_NAMED(kDll, "IGraphBuilder::RemoveFilter", Gb_RemoveFilter, 2) {
  Self(c, "IGraphBuilder");
  return S_OK;
}

// (LPCWSTR file, LPCWSTR filter_name, IBaseFilter** out): the file is not
// read; the filter's output pin renders nothing.
HLE_NAMED(kDll, "IGraphBuilder::AddSourceFilter", Gb_AddSourceFilter, 4) {
  auto g = Self(c, "IGraphBuilder");
  const uint32_t out = Arg(c, 3);
  if (!out) return E_POINTER;
  RECOMP_TRACE("  quartz: AddSourceFilter(%s)\n",
               Arg(c, 1) ? win32::Narrow(GWStr(Arg(c, 1))).c_str() : "");
  if (Arg(c, 1)) g->file = HostPath(NarrowPath(GWStr(Arg(c, 1))));
  auto s = std::make_shared<Source>();
  if (Arg(c, 2)) s->name = GWStr(Arg(c, 2));
  const uint32_t filter = com::Publish(s, SourceVtable());
  auto p = std::make_shared<Pin>();
  p->filter = s;
  s->pin = com::Publish(p, PinVtable());
  g->out_pin = s->pin;
  Wr32(out, filter);
  return S_OK;
}

// (IPin* output): to the game's renderer, where it can be; otherwise
// nothing plays (S_OK all the same: the movie then ends as it starts).
HLE_NAMED(kDll, "IGraphBuilder::Render", Gb_Render, 2) {
  auto g = Self(c, "IGraphBuilder");
  if (!g->movie) Connect(c, *g);
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
  e->filter = std::static_pointer_cast<Source>(com::Lookup(s->guest));
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
  auto s = Live(e->filter);
  if (n && !e->done && s) {
    com::AddRef(s->pin);
    Wr32(pins, s->pin);
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
  // A pin whose filter is gone has none (PIN_INFO allows a null filter).
  auto s = Live(p->filter);
  if (s) com::AddRef(s->guest);
  Wr32(info, s ? s->guest : 0);
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

// The renderer's state follows the graph's. Calls into the game's
// renderer run without the graph's lock: it may call back into the graph.
HLE_NAMED(kDll, "IMediaControl::Run", Mc_Run, 1) {
  auto g = Self(c, "IMediaControl");
  if (g->state == State_Running) return S_OK;
  if (g->movie) {
    if (!g->playing) {
      Call(c, g->renderer, kFilterSetSyncSource, {0});
      Call(c, g->allocator, kAllocCommit);
      Call(c, g->renderer, kFilterPause);
      Call(c, g->renderer, kFilterRun, {0, 0});
#ifdef RECOMP_QUARTZ_VIDEO
      g->movie->Start(Gain(g->volume));
#endif
      g->playing = true;
    } else {
      Call(c, g->renderer, kFilterRun, {0, 0});
#ifdef RECOMP_QUARTZ_VIDEO
      g->movie->SetRunning(true);
#endif
    }
  } else {
    std::lock_guard<std::mutex> l(g->mu);
    SetComplete(*g, true);
  }
  g->state = State_Running;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::Pause", Mc_Pause, 1) {
  auto g = Self(c, "IMediaControl");
  if (g->playing) {
    Call(c, g->renderer, kFilterPause);
#ifdef RECOMP_QUARTZ_VIDEO
    g->movie->SetRunning(false);
#endif
  }
  g->state = State_Paused;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::Stop", Mc_Stop, 1) {
  auto g = Self(c, "IMediaControl");
  StopPlayback(c, *g);
  g->state = State_Stopped;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::StopWhenReady", Mc_StopWhenReady, 1) {
  auto g = Self(c, "IMediaControl");
  StopPlayback(c, *g);
  g->state = State_Stopped;
  return S_OK;
}
// (msTimeout, OAFilterState*): no state change is ever in progress.
HLE_NAMED(kDll, "IMediaControl::GetState", Mc_GetState, 3) {
  auto g = Self(c, "IMediaControl");
  if (!Arg(c, 2)) return E_POINTER;
  Pump(c, *g);
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
// once the movie has played (at once if nothing plays), else E_ABORT (the
// timeout passing without an event). Delivers the frame due first.
HLE_NAMED(kDll, "IMediaEventEx::GetEvent", Me_GetEvent, 5) {
  auto g = Self(c, "IMediaEventEx");
  const uint32_t code = Arg(c, 1), p1 = Arg(c, 2), p2 = Arg(c, 3);
  if (!code || !p1 || !p2) return E_POINTER;
  Pump(c, *g);
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
// In 100 ns units; a graph that plays nothing has length 0.

void WrTime(uint32_t va, int64_t t) { std::memcpy(GPtr(va), &t, 8); }

HLE_NAMED(kDll, "IMediaSeeking::GetDuration", Ms_GetDuration, 2) {
  auto g = Self(c, "IMediaSeeking");
  if (!Arg(c, 1)) return E_POINTER;
#ifdef RECOMP_QUARTZ_VIDEO
  WrTime(Arg(c, 1), g->movie ? g->movie->duration_us * 10 : 0);
#else
  WrTime(Arg(c, 1), 0);
#endif
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSeeking::GetCurrentPosition", Ms_GetCurrentPosition, 2) {
  auto g = Self(c, "IMediaSeeking");
  if (!Arg(c, 1)) return E_POINTER;
  Pump(c, *g);
#ifdef RECOMP_QUARTZ_VIDEO
  WrTime(Arg(c, 1), g->movie && g->playing ? g->movie->PositionUs() * 10 : 0);
#else
  WrTime(Arg(c, 1), 0);
#endif
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
#ifdef RECOMP_QUARTZ_VIDEO
  if (g->movie) g->movie->SetVolume(Gain(g->volume));
#endif
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

// ---- IMemAllocator ------------------------------------------------------------

HLE_NAMED(kDll, "IMemAllocator::QueryInterface", Ma_QueryInterface, 3) {
  return QueryOne(c, kIID_IMemAllocator);
}
HLE_NAMED(kDll, "IMemAllocator::AddRef", Ma_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
// The last reference frees the pool (the samples and their buffers).
HLE_NAMED(kDll, "IMemAllocator::Release", Ma_Release, 1) {
  Allocator* a = SelfAs<Allocator>(c, "IMemAllocator");
  std::vector<uint32_t> samples;
  if (a->refs.load() == 1) {
    std::lock_guard<std::mutex> l(a->mu);
    samples.swap(a->samples);
  }
  const uint32_t left = com::Release(Arg(c, 0));
  for (uint32_t g : samples) {
    if (auto* s = com::As<Sample>(g)) HeapFreeProcess(s->buffer);
    com::Release(g);
  }
  return left;
}
// (ALLOCATOR_PROPERTIES* request, ALLOCATOR_PROPERTIES* actual):
// {cBuffers, cbBuffer, cbAlign, cbPrefix}, granted as asked.
HLE_NAMED(kDll, "IMemAllocator::SetProperties", Ma_SetProperties, 3) {
  Allocator* a = SelfAs<Allocator>(c, "IMemAllocator");
  const uint32_t req = Arg(c, 1), act = Arg(c, 2);
  if (!req || !act) return E_POINTER;
  std::lock_guard<std::mutex> l(a->mu);
  if (a->committed) return VFW_E_ALREADY_COMMITTED;
  const int32_t n = int32_t(Rd32(req)), size = int32_t(Rd32(req + 4));
  if (n <= 0 || size <= 0) return E_INVALIDARG;
  a->count = uint32_t(n);
  a->size = uint32_t(size);
  a->align = std::max<uint32_t>(Rd32(req + 8), 1);
  a->prefix = Rd32(req + 12);
  Wr32(act, a->count);
  Wr32(act + 4, a->size);
  Wr32(act + 8, a->align);
  Wr32(act + 12, a->prefix);
  return S_OK;
}
HLE_NAMED(kDll, "IMemAllocator::GetProperties", Ma_GetProperties, 2) {
  Allocator* a = SelfAs<Allocator>(c, "IMemAllocator");
  const uint32_t p = Arg(c, 1);
  if (!p) return E_POINTER;
  std::lock_guard<std::mutex> l(a->mu);
  Wr32(p, a->count);
  Wr32(p + 4, a->size);
  Wr32(p + 8, a->align);
  Wr32(p + 12, a->prefix);
  return S_OK;
}
// The buffers are made on the first commit and kept until the pool goes.
HLE_NAMED(kDll, "IMemAllocator::Commit", Ma_Commit, 1) {
  Allocator* a = SelfAs<Allocator>(c, "IMemAllocator");
  std::lock_guard<std::mutex> l(a->mu);
  if (!a->size) return VFW_E_SIZENOTSET;
  if (a->samples.empty()) {
    auto self = std::static_pointer_cast<Allocator>(com::Lookup(a->guest));
    for (uint32_t i = 0; i < a->count; i++) {
      auto smp = std::make_shared<Sample>();
      smp->owner = self;
      smp->size = a->size;
      smp->buffer = HeapAllocProcess(a->size + a->prefix + a->align, true) + a->prefix;
      a->samples.push_back(com::Publish(smp, SampleVtable()));
    }
  }
  a->committed = true;
  return S_OK;
}
HLE_NAMED(kDll, "IMemAllocator::Decommit", Ma_Decommit, 1) {
  Allocator* a = SelfAs<Allocator>(c, "IMemAllocator");
  std::lock_guard<std::mutex> l(a->mu);
  a->committed = false;
  return S_OK;
}
// (IMediaSample** out, REFERENCE_TIME* start, REFERENCE_TIME* end, DWORD
// flags): never waits (the graph renders each sample before asking for
// the next).
HLE_NAMED(kDll, "IMemAllocator::GetBuffer", Ma_GetBuffer, 5) {
  Allocator* a = SelfAs<Allocator>(c, "IMemAllocator");
  const uint32_t out = Arg(c, 1);
  if (!out) return E_POINTER;
  const uint32_t g = Lend(*a);
  Wr32(out, g);
  if (g) return S_OK;
  std::lock_guard<std::mutex> l(a->mu);
  return a->committed ? VFW_E_TIMEOUT : VFW_E_NOT_COMMITTED;
}
HLE_NAMED(kDll, "IMemAllocator::ReleaseBuffer", Ma_ReleaseBuffer, 2) {
  SelfAs<Allocator>(c, "IMemAllocator");
  return S_OK;
}

// ---- IMediaSample -------------------------------------------------------------

HLE_NAMED(kDll, "IMediaSample::QueryInterface", Smp_QueryInterface, 3) {
  return QueryOne(c, kIID_IMediaSample);
}
HLE_NAMED(kDll, "IMediaSample::AddRef", Smp_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
// Down to the pool's own reference: back in the pool.
HLE_NAMED(kDll, "IMediaSample::Release", Smp_Release, 1) {
  Sample* s = SelfAs<Sample>(c, "IMediaSample");
  auto a = s->owner.lock();
  if (!a) return com::Release(Arg(c, 0));
  std::lock_guard<std::mutex> l(a->mu);
  const uint32_t left = com::Release(Arg(c, 0));
  if (left == 1) s->lent = false;
  return left;
}
HLE_NAMED(kDll, "IMediaSample::GetPointer", Smp_GetPointer, 2) {
  Sample* s = SelfAs<Sample>(c, "IMediaSample");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), s->buffer);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSample::GetSize", Smp_GetSize, 1) {
  return SelfAs<Sample>(c, "IMediaSample")->size;
}
// (REFERENCE_TIME* start, REFERENCE_TIME* end): VFW_E_SAMPLE_TIME_NOT_SET
// without times.
HLE_NAMED(kDll, "IMediaSample::GetTime", Smp_GetTime, 3) {
  Sample* s = SelfAs<Sample>(c, "IMediaSample");
  if (!s->times) return 0x80040249;
  if (Arg(c, 1)) std::memcpy(GPtr(Arg(c, 1)), &s->start, 8);
  if (Arg(c, 2)) std::memcpy(GPtr(Arg(c, 2)), &s->end, 8);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSample::SetTime", Smp_SetTime, 3) {
  Sample* s = SelfAs<Sample>(c, "IMediaSample");
  s->times = Arg(c, 1) != 0;
  if (Arg(c, 1)) std::memcpy(&s->start, GPtr(Arg(c, 1)), 8);
  s->end = s->start;
  if (Arg(c, 2)) std::memcpy(&s->end, GPtr(Arg(c, 2)), 8);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSample::IsSyncPoint", Smp_IsSyncPoint, 1) {
  return SelfAs<Sample>(c, "IMediaSample")->sync ? S_OK : S_FALSE;
}
HLE_NAMED(kDll, "IMediaSample::SetSyncPoint", Smp_SetSyncPoint, 2) {
  SelfAs<Sample>(c, "IMediaSample")->sync = Arg(c, 1) != 0;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSample::IsPreroll", Smp_IsPreroll, 1) {
  return SelfAs<Sample>(c, "IMediaSample")->preroll ? S_OK : S_FALSE;
}
HLE_NAMED(kDll, "IMediaSample::SetPreroll", Smp_SetPreroll, 2) {
  SelfAs<Sample>(c, "IMediaSample")->preroll = Arg(c, 1) != 0;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSample::GetActualDataLength", Smp_GetActualDataLength, 1) {
  return SelfAs<Sample>(c, "IMediaSample")->actual;
}
HLE_NAMED(kDll, "IMediaSample::SetActualDataLength", Smp_SetActualDataLength, 2) {
  Sample* s = SelfAs<Sample>(c, "IMediaSample");
  if (Arg(c, 1) > s->size) return VFW_E_BUFFER_OVERFLOW;
  s->actual = Arg(c, 1);
  return S_OK;
}
// (AM_MEDIA_TYPE**): the type has not changed since the connection.
HLE_NAMED(kDll, "IMediaSample::GetMediaType", Smp_GetMediaType, 2) {
  SelfAs<Sample>(c, "IMediaSample");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), 0);
  return S_FALSE;
}
HLE_NAMED(kDll, "IMediaSample::IsDiscontinuity", Smp_IsDiscontinuity, 1) {
  return SelfAs<Sample>(c, "IMediaSample")->discontinuity ? S_OK : S_FALSE;
}
HLE_NAMED(kDll, "IMediaSample::SetDiscontinuity", Smp_SetDiscontinuity, 2) {
  SelfAs<Sample>(c, "IMediaSample")->discontinuity = Arg(c, 1) != 0;
  return S_OK;
}
// (LONGLONG* start, LONGLONG* end): no media times.
HLE_NAMED(kDll, "IMediaSample::GetMediaTime", Smp_GetMediaTime, 3) {
  SelfAs<Sample>(c, "IMediaSample");
  return 0x80040251;  // VFW_E_MEDIA_TIME_NOT_SET
}

}  // namespace recomp::hle
