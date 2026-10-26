#pragma once
// xam's messages (XMsg*): a title calls into one of xam's apps by app and
// message id, in-process (XMsgInProcessCall, two arguments) or as an I/O
// request (XMsgStartIORequest(Ex), a buffer and its size) that the app
// completes through an XOVERLAPPED. Each message has a handler here,
// registered with XMSG(app, message, Name); one without is fatal when it
// is sent and names the app and message. A handler returns an HRESULT and
// may set the length its XOVERLAPPED reports (what the app hands
// XMsgCompleteIORequest: a count of items, a disposition).

#include <cstdint>

#include "ppc/ctx.h"
#include "xbox360/hle.h"

namespace recomp::xbox360 {

// The apps titles send to, as Xenia names them, and xam's own (0xFE), which
// its content exports send to.
constexpr uint32_t kXmpApp = 0xFA, kXgiApp = 0xFB, kXLiveBaseApp = 0xFC, kXamApp = 0xFE;

using XMsgFn = uint32_t (*)(ppc::Ctx& c, uint32_t p1, uint32_t p2, uint32_t& length);
// XMsgInProcessCall for xam's own callers: the handler's HRESULT.
uint32_t CallXMsg(ppc::Ctx& c, uint32_t app, uint32_t message, uint32_t p1, uint32_t p2);
// XMsgStartIORequest for xam's own callers (its exports that send a
// request): S_OK, the request completed.
uint32_t StartXMsgRequest(ppc::Ctx& c, uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer,
                          uint32_t size);
// XMsgProcessRequest: the same, returning the request's HRESULT, or with an
// XOVERLAPPED HRESULT_FROM_WIN32(ERROR_IO_PENDING).
uint32_t ProcessXMsgRequest(ppc::Ctx& c, uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer,
                            uint32_t size);
// The Win32 code xam's exports return for an HRESULT: 0 for success,
// FACILITY_WIN32's own code, ERROR_FUNCTION_FAILED otherwise.
uint32_t Win32FromHResult(uint32_t hr);
struct XMsgRegistrar {
  XMsgRegistrar(uint32_t app, uint32_t message, const char* name, XMsgFn fn);
};

}  // namespace recomp::xbox360

// XMSG(kXmpApp, 0x7001B, XMPGetPlaybackController) { ... return hr; }
#define XMSG(app, message, name)                                                       \
  static uint32_t RECOMP_XHLE_CAT(xmsg_, name)(::recomp::ppc::Ctx & c, uint32_t p1, uint32_t p2,  \
                                               uint32_t & length);                               \
  static const ::recomp::xbox360::XMsgRegistrar RECOMP_XHLE_CAT(xmsg_reg_, name){              \
      app, message, #name, &RECOMP_XHLE_CAT(xmsg_, name)};                                     \
  static uint32_t RECOMP_XHLE_CAT(xmsg_, name)([[maybe_unused]] ::recomp::ppc::Ctx & c,          \
                                               [[maybe_unused]] uint32_t p1,                     \
                                               [[maybe_unused]] uint32_t p2,                     \
                                               [[maybe_unused]] uint32_t & length)
