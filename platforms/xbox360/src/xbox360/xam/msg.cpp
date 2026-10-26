// xam: messages to its apps (msg.h).

#include "xbox360/xam/msg.h"

#include <map>
#include <utility>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorIoPending = 0x3E5, kErrorFunctionFailed = 0x65B;

struct Message {
  const char* name;
  XMsgFn fn;
};
std::map<std::pair<uint32_t, uint32_t>, Message>& Messages() {
  static std::map<std::pair<uint32_t, uint32_t>, Message> m;
  return m;
}

uint32_t Send(ppc::Ctx& c, uint32_t app, uint32_t message, uint32_t p1, uint32_t p2) {
  auto it = Messages().find({app, message});
  if (it == Messages().end())
    ppc::Fatal(&c, "XMsg: app %x message %x has no handler (called from %llx)", app, message,
               static_cast<unsigned long long>(c.lr));
  return it->second.fn(c, p1, p2);
}

// An HRESULT as the Win32 code an XOVERLAPPED's result carries: 0 for
// success, FACILITY_WIN32's own code, ERROR_FUNCTION_FAILED otherwise
// (chosen: each app completes its requests itself).
uint32_t Win32Result(uint32_t hr) {
  if (int32_t(hr) >= 0) return 0;
  return (hr >> 16 & 0x1FFF) == 7 ? hr & 0xFFFF : kErrorFunctionFailed;
}

}  // namespace

XMsgRegistrar::XMsgRegistrar(uint32_t app, uint32_t message, const char* name, XMsgFn fn) {
  if (!Messages().emplace(std::make_pair(app, message), Message{name, fn}).second)
    ppc::Fatal(nullptr, "two handlers for XMsg app %x message %x", app, message);
}

// XMsgInProcessCall(app, message, p1, p2): the app's handler, its result.
HLE(kXam, 500, XMsgInProcessCall) { return Send(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3)); }

// XMsgStartIORequestEx(app, message, *overlapped, buffer, size, *xparams):
// the request runs at once here. With an XOVERLAPPED it is complete when
// this returns, as XMsgCompleteIORequest leaves it: the result (a Win32
// code) in word 0, the length 0 in word 1, the HRESULT as the extended
// error in word 6, its event set. Without one xam waits for the request
// and leaves the HRESULT as the thread's last error, returning S_OK.
uint32_t StartXMsgRequest(ppc::Ctx& c, uint32_t app, uint32_t message, uint32_t ov, uint32_t buffer,
                          uint32_t size) {
  uint32_t hr = Send(c, app, message, buffer, size);
  if (!ov) {
    SetLastError(hr);
    return 0;
  }
  if (Rd32(ov + 0x10)) ppc::Fatal(&c, "XMsg: completion routines (APCs) are not supported yet");
  Wr32(ov + 4, 0);
  Wr32(ov + 0x18, hr);
  Wr32(ov, Win32Result(hr));
  if (uint32_t event = Rd32(ov + 0xC)) {
    uint32_t obj = HandleObject(event);
    if (!obj) ppc::Fatal(&c, "XMsg: the XOVERLAPPED's event %x is not a handle", event);
    SetSignal(obj, 1);
  }
  return 0;
}
HLE(kXam, 508, XMsgStartIORequestEx) {
  return StartXMsgRequest(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4));
}
HLE(kXam, 503, XMsgStartIORequest) {
  return StartXMsgRequest(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4));
}

}  // namespace recomp::xbox360
