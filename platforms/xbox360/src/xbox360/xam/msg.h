#pragma once
// xam's messages (XMsg*): a title calls into one of xam's apps by app and
// message id, in-process (XMsgInProcessCall, two arguments) or as an I/O
// request (XMsgStartIORequest(Ex), a buffer and its size) that the app
// completes through an XOVERLAPPED. Each message has a handler here,
// registered with XMSG(app, message, Name); one without is fatal when it
// is sent and names the app and message. A handler returns an HRESULT.

#include <cstdint>

#include "ppc/ctx.h"
#include "xbox360/hle.h"

namespace recomp::xbox360 {

// The apps titles send to, as Xenia names them.
constexpr uint32_t kXmpApp = 0xFA, kXgiApp = 0xFB, kXLiveBaseApp = 0xFC;

using XMsgFn = uint32_t (*)(ppc::Ctx& c, uint32_t p1, uint32_t p2);
struct XMsgRegistrar {
  XMsgRegistrar(uint32_t app, uint32_t message, const char* name, XMsgFn fn);
};

}  // namespace recomp::xbox360

// XMSG(kXmpApp, 0x7001B, XMPGetPlaybackController) { ... return hr; }
#define XMSG(app, message, name)                                                       \
  static uint32_t RECOMP_XHLE_CAT(xmsg_, name)(::recomp::ppc::Ctx & c, uint32_t p1, uint32_t p2); \
  static const ::recomp::xbox360::XMsgRegistrar RECOMP_XHLE_CAT(xmsg_reg_, name){              \
      app, message, #name, &RECOMP_XHLE_CAT(xmsg_, name)};                                     \
  static uint32_t RECOMP_XHLE_CAT(xmsg_, name)([[maybe_unused]] ::recomp::ppc::Ctx & c,          \
                                               [[maybe_unused]] uint32_t p1,                     \
                                               [[maybe_unused]] uint32_t p2)
