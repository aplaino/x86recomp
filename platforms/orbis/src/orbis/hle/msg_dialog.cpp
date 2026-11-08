// libSceMsgDialog HLE: the system's message dialog, drawn by the runtime
// over the game's frames (videoout::SetOverlay) while it has the
// controller (pad::SetIntercepted). The library's side is libSceMsgDialog
// 11.00's: the checks of SceMsgDialogParam and what it points to (in its
// order; some depend on the game's SDK version), the states (FINISHED
// seen only through UpdateStatus), the codes, the result's layout and its
// fix-up of a backed-out dialog for SDK 3.00 on. The dialog the player
// sees and answers is this runtime's own design. Implemented: user
// messages (but button type 4) and progress bars; system messages are
// fatal. docs/hle.md has what is not measured.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/dialog.h"
#include "orbis/hle/dialog_ui.h"
#include "orbis/hle/pad.h"
#include "orbis/hle/videoout.h"
#include "orbis/process.h"
#include "runtime/ui/canvas.h"
#include "x86/runtime.h"

namespace {

using recomp::gaddr;
using recomp::Rd32;
using recomp::Rd64;
using recomp::orbis::SdkVersion;
namespace pad = recomp::orbis::pad;

constexpr uint32_t kErrNotSystemInitialized = 0x80B80001, kErrNotInitialized = 0x80B80003,
                   kErrAlreadyInitialized = 0x80B80004, kErrNotFinished = 0x80B80005,
                   kErrInvalidState = 0x80B80006, kErrParam = 0x80B8000A,
                   kErrNotRunning = 0x80B8000B, kErrAlreadyClose = 0x80B8000C,
                   kErrArgNull = 0x80B8000D, kErrNotSupported = 0x80B8000F;

enum : uint32_t { kNone, kInitialized, kRunning, kFinished };
enum : uint32_t { kModeUser = 1, kModeProgress = 2, kModeSystem = 3 };
// Button types.
enum : uint32_t {
  kOk, kYesNo, kNoButtons, kOkCancel, kWait = 5, kWaitCancel, kYesNoFocusNo,
  kOkCancelFocusCancel, kTwoButtons,
};
constexpr int32_t kResultOk = 0, kResultCanceled = 1;
constexpr uint32_t kButtonNone = 0, kButton1 = 1, kButton2 = 2;

bool Zero(gaddr p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (recomp::Rd8(p + i)) return false;
  return true;
}

// The longest message (from SDK 1.50; 0x200 before).
size_t MsgMax() { return SdkVersion() >= 0x01500000 ? 0x2000 : 0x200; }

// A string of fewer than max bytes (strnlen_s(s, max) < max).
bool Shorter(gaddr s, size_t max) {
  for (size_t i = 0; i < max; i++)
    if (!recomp::Rd8(s + i)) return true;
  return false;
}

// SceCommonDialogBaseParam: its size (48), 36 zero bytes and the magic,
// 0xC0D1A109 plus the low 32 bits of its address.
bool ValidBase(gaddr p) {
  return Rd32(p + 44) == uint32_t(0xC0D1A109 + uint32_t(p)) && Rd64(p) == 48 && Zero(p + 8, 36);
}

// A button label ({msg1*, msg2*, 32 bytes unchecked}): 63 bytes at most,
// no line breaks.
bool ValidLabel(gaddr s) {
  if (!s || !Shorter(s, 0x40)) return false;
  for (gaddr p = s; recomp::Rd8(p); p++)
    if (recomp::Rd8(p) == '\n' || recomp::Rd8(p) == '\r') return false;
  return true;
}

// SceMsgDialogUserMessageParam: {button type (< 10), msg*, buttons*
// (only and always for type 9), 24 zero bytes}.
bool ValidUser(gaddr u) {
  const uint32_t type = Rd32(u);
  const gaddr msg = Rd64(u + 8), buttons = Rd64(u + 16);
  if (type >= 10 || !msg || !Shorter(msg, MsgMax())) return false;
  if (type == kTwoButtons ? !buttons || !ValidLabel(Rd64(buttons)) || !ValidLabel(Rd64(buttons + 8))
                          : buttons != 0)
    return false;
  return Zero(u + 24, 24);
}

// SceMsgDialogProgressBarParam: {bar type (< 2), msg*, 64 zero bytes}.
bool ValidProgress(gaddr p) {
  const gaddr msg = Rd64(p + 8);
  if (Rd32(p) >= 2 || !msg) return false;
  if (SdkVersion() >= 0x01500000 && !Shorter(msg, 0x2000)) return false;
  return Zero(p + 16, 64);
}

// SceMsgDialogSystemMessageParam: {type (<= 6), 32 zero bytes}.
bool ValidSystem(gaddr s) { return Rd32(s) <= 6 && Zero(s + 4, 32); }

// SceMsgDialogParam (0x88 after the base): mode, the one parameter it
// takes, the user (system messages), 40 zero bytes.
uint32_t CheckParam(gaddr p) {
  if (!ValidBase(p) || Rd64(p + 0x30) != 0x88) return kErrParam;
  const uint32_t mode = Rd32(p + 0x38);
  const gaddr user = Rd64(p + 0x40), bar = Rd64(p + 0x48), sys = Rd64(p + 0x50);
  switch (mode) {
    case kModeSystem:
      if (user || bar || !sys || !ValidSystem(sys)) return kErrParam;
      // Types 1, 2, 5 and 6 are about the signed-in user (the one user).
      if ((0x66 >> Rd32(sys) & 1) && Rd32(p + 0x58) != 0x10000000) return kErrParam;
      break;
    case kModeProgress:
      if (user || sys || !bar || !ValidProgress(bar)) return kErrParam;
      break;
    case kModeUser:
      if (bar || sys || !user || !ValidUser(user)) return kErrParam;
      break;
    default: return kErrParam;
  }
  return Zero(p + 0x5C, 40) ? 0 : kErrParam;
}

// ---- the dialog the player sees (this runtime's own)

constexpr int kW = 1920, kH = 1080, kBoxW = 1100, kTextSize = 36;
constexpr uint32_t kText = 0xFFFFFFFF, kDim = 0xFFB0B8C8;

struct Dialog {
  uint32_t mode, type = 0, bar_type = 0;
  std::string text;
  std::vector<std::string> buttons;  // left to right
  size_t focus = 0;
  uint32_t progress = 0;
};

std::mutex g_mu;
uint32_t g_status = kNone;     // as UpdateStatus last saw it
bool g_done = false;           // the dialog has ended (UpdateStatus moves to FINISHED)
bool g_close_requested = false;
std::shared_ptr<Dialog> g_dialog;  // the one open or last finished
uint32_t g_progress = 0;           // kept across opens, as the library's
int32_t g_result = 0;
uint32_t g_button = kButtonNone;
uint32_t g_mode = 0, g_type = 0;

std::shared_ptr<recomp::ui::Canvas> Draw(const Dialog& d) {
  auto c = std::make_shared<recomp::ui::Canvas>(kW, kH);
  const auto lines = recomp::orbis::dialog::Wrap(d.text, kTextSize, kBoxW - 120);
  const int bar_h = d.mode == kModeProgress ? 90 : 0;
  const int buttons_h = d.buttons.empty() ? 0 : 110;
  const int h = 120 + int(lines.size()) * 50 + bar_h + buttons_h;
  const int x = (kW - kBoxW) / 2, y = (kH - h) / 2;
  c->Fill(0, 0, kW, kH, 0x80000000);
  c->Fill(x, y, kBoxW, h, 0xF0181C24);
  c->Fill(x, y, kBoxW, 2, 0x60FFFFFF);
  int ty = y + 60;
  for (const std::string& l : lines) {
    c->Text(x + 60, ty + 36, kTextSize, l, kText, kBoxW - 120);
    ty += 50;
  }
  if (d.mode == kModeProgress) {
    const int bw = kBoxW - 120, filled = bw * int(std::min<uint32_t>(d.progress, 100)) / 100;
    c->Fill(x + 60, ty + 20, bw, 16, 0x40FFFFFF);
    c->Fill(x + 60, ty + 20, filled, 16, 0xFF3C8CFF);
    c->Text(x + 60, ty + 80, 30, std::to_string(std::min<uint32_t>(d.progress, 100)) + "%", kDim);
    ty += bar_h;
  }
  if (!d.buttons.empty()) {
    const int bw = 300, gap = 40;
    const int total = int(d.buttons.size()) * bw + (int(d.buttons.size()) - 1) * gap;
    int bx = x + (kBoxW - total) / 2;
    for (size_t i = 0; i < d.buttons.size(); i++, bx += bw + gap) {
      c->Fill(bx, ty + 20, bw, 64, i == d.focus ? 0xFF3C8CFF : 0x40FFFFFF);
      const int tw = recomp::ui::TextWidth(32, d.buttons[i]);
      c->Text(bx + (bw - tw) / 2, ty + 64, 32, d.buttons[i], kText, bw);
    }
  }
  return c;
}

// Ends the dialog with the answer, unless it has ended. Under g_mu.
void Finish(Dialog* d, int32_t result, uint32_t button) {
  if (g_done || g_dialog.get() != d) return;
  recomp::orbis::videoout::SetOverlay(nullptr);
  pad::SetIntercepted(false);
  g_result = result, g_button = button, g_done = true;
}

bool Open(const Dialog* d) {
  std::lock_guard<std::mutex> l(g_mu);
  return !g_done && g_dialog.get() == d;
}

// What choosing a button answers: {result, button id}.
std::pair<int32_t, uint32_t> Answer(const Dialog& d, size_t i) {
  switch (d.type) {
    case kYesNo: case kYesNoFocusNo: case kTwoButtons:
      return {kResultOk, i == 0 ? kButton1 : kButton2};
    case kOkCancel: case kOkCancelFocusCancel:
      return i == 0 ? std::pair{kResultOk, kButton1} : std::pair{kResultCanceled, kButtonNone};
    case kWaitCancel: return {kResultCanceled, kButtonNone};
    default: return {kResultOk, kButton1};
  }
}

// Left and right move the focus, cross chooses, circle backs out (where
// the dialog can be backed out of). Buttons held when the dialog opens
// count only once released. A progress bar redraws as the game moves it.
void Run(std::shared_ptr<Dialog> d) {
  uint32_t prev = pad::Buttons(), drawn = d->progress;
  const bool can_back = d->mode == kModeProgress ? d->bar_type == 1 : !d->buttons.empty();
  while (Open(d.get())) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    const uint32_t b = pad::Buttons(), down = b & ~prev;
    prev = b;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_done || g_dialog != d) return;
    if (down & pad::kCross && !d->buttons.empty()) {
      const auto [result, button] = Answer(*d, d->focus);
      return Finish(d.get(), result, button);
    }
    if (down & pad::kCircle && can_back) return Finish(d.get(), kResultCanceled, kButtonNone);
    const int move = down & pad::kLeft ? -1 : down & pad::kRight ? 1 : 0;
    const size_t f = move && !d->buttons.empty()
                         ? size_t(std::clamp<ptrdiff_t>(ptrdiff_t(d->focus) + move, 0,
                                                        ptrdiff_t(d->buttons.size()) - 1))
                         : d->focus;
    d->progress = g_progress;
    if (f != d->focus || d->progress != drawn) {
      d->focus = f, drawn = d->progress;
      recomp::orbis::videoout::SetOverlay(Draw(*d));
    }
  }
}

// The progress bar calls' checks; 0 if they pass. Under g_mu.
uint32_t CheckProgressCall(int32_t target) {
  if (g_status != kRunning) return kErrNotRunning;  // as UpdateStatus last saw it
  if (g_mode != kModeProgress) return kErrNotSupported;
  return target != 0 ? kErrParam : 0;
}

}  // namespace

using recomp::hle::Arg;

ORBIS_HLE("libSceMsgDialog", sceMsgDialogInitialize, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status != kNone) return kErrAlreadyInitialized;
  if (!recomp::orbis::dialog::CommonInitialized()) return kErrNotSystemInitialized;
  g_status = kInitialized;
  return 0;
}

// int sceMsgDialogOpen(const SceMsgDialogParam* param): every parameter
// check before the library's state; a finished dialog opens again.
ORBIS_HLE("libSceMsgDialog", sceMsgDialogOpen, 1) {
  const gaddr p = Arg(c, 0);
  if (!p) return kErrArgNull;
  if (const uint32_t r = CheckParam(p)) return r;
  auto d = std::make_shared<Dialog>();
  d->mode = Rd32(p + 0x38);
  if (d->mode == kModeSystem)
    recomp::FatalF("sceMsgDialogOpen: system message %u not implemented", Rd32(Rd64(p + 0x50)));
  if (d->mode == kModeUser) {
    const gaddr u = Rd64(p + 0x40);
    d->type = Rd32(u);
    d->text = recomp::GStr(Rd64(u + 8)).substr(0, MsgMax() - 1);
    switch (d->type) {
      case kOk: d->buttons = {"OK"}; break;
      case kYesNo: d->buttons = {"Yes", "No"}; break;
      case kYesNoFocusNo: d->buttons = {"Yes", "No"}, d->focus = 1; break;
      case kOkCancel: d->buttons = {"OK", "Cancel"}; break;
      case kOkCancelFocusCancel: d->buttons = {"OK", "Cancel"}, d->focus = 1; break;
      case kWaitCancel: d->buttons = {"Cancel"}; break;
      case kTwoButtons: {
        const gaddr b = Rd64(u + 16);
        d->buttons = {recomp::GStr(Rd64(b)), recomp::GStr(Rd64(b + 8))};
        break;
      }
      case kNoButtons: case kWait: break;
      default: recomp::FatalF("sceMsgDialogOpen: button type %u not implemented", d->type);
    }
  } else {
    const gaddr bar = Rd64(p + 0x48);
    d->bar_type = Rd32(bar);
    d->text = recomp::GStr(Rd64(bar + 8)).substr(0, MsgMax() - 1);
  }
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (g_status == kRunning) return kErrInvalidState;
  d->progress = g_progress;
  g_dialog = d;
  g_status = kRunning;
  g_done = g_close_requested = false;
  g_mode = d->mode, g_type = d->type;
  g_result = 0, g_button = kButtonNone;
  pad::SetIntercepted(true);
  recomp::orbis::videoout::SetOverlay(Draw(*d));
  std::thread([d] { Run(d); }).detach();
  return 0;
}

// SceCommonDialogStatus: GetStatus as last seen, UpdateStatus also sees
// the dialog end.
ORBIS_HLE("libSceMsgDialog", sceMsgDialogGetStatus, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status;
}
ORBIS_HLE("libSceMsgDialog", sceMsgDialogUpdateStatus, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kRunning && g_done) g_status = kFinished;
  return g_status;
}

// int sceMsgDialogGetResult(SceMsgDialogResult* r): {mode, result, button
// id, 32 bytes zero on input}. From SDK 3.00, backing out of an OK dialog
// reads as OK, of a yes/no one as no.
ORBIS_HLE("libSceMsgDialog", sceMsgDialogGetResult, 1) {
  const gaddr r = Arg(c, 0);
  if (!r) return kErrArgNull;
  if (!Zero(r + 12, 32)) return kErrParam;
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (g_status != kFinished) return kErrNotFinished;
  int32_t result = g_result;
  uint32_t button = g_button;
  if (SdkVersion() >= 0x03000000 && button == kButtonNone && result == kResultCanceled &&
      g_mode == kModeUser) {
    if (g_type == kOk) result = kResultOk, button = kButton1;
    else if (g_type == kYesNo || g_type == kYesNoFocusNo) result = kResultOk, button = kButton2;
  }
  recomp::Wr32(r, g_mode);
  recomp::Wr32(r + 4, uint32_t(result));
  recomp::Wr32(r + 8, button);
  return 0;
}

// int sceMsgDialogClose(void): the game ends the dialog (unless the player
// has answered); it answers as backed out of.
ORBIS_HLE("libSceMsgDialog", sceMsgDialogClose, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (g_status != kRunning) return kErrNotRunning;  // as UpdateStatus last saw it
  if (g_close_requested) return kErrAlreadyClose;
  g_close_requested = true;
  Finish(g_dialog.get(), kResultCanceled, kButtonNone);
  return 0;
}

// int sceMsgDialogTerminate(void): also while the dialog is open.
ORBIS_HLE("libSceMsgDialog", sceMsgDialogTerminate, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (g_dialog && !g_done) Finish(g_dialog.get(), kResultCanceled, kButtonNone);
  g_dialog.reset();
  g_status = kNone;
  return 0;
}

// The progress bar (target 0, the only bar): SetValue stores the rate as
// it is, Inc adds with 32-bit wrap and stops at 100, SetMsg replaces the
// text.
ORBIS_HLE("libSceMsgDialog", sceMsgDialogProgressBarSetValue, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (const uint32_t r = CheckProgressCall(int32_t(Arg(c, 0)))) return r;
  g_progress = uint32_t(Arg(c, 1));
  return 0;
}
ORBIS_HLE("libSceMsgDialog", sceMsgDialogProgressBarInc, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (const uint32_t r = CheckProgressCall(int32_t(Arg(c, 0)))) return r;
  const uint32_t v = g_progress + uint32_t(Arg(c, 1));
  g_progress = v >= 100 ? 100 : v;
  return 0;
}
ORBIS_HLE("libSceMsgDialog", sceMsgDialogProgressBarSetMsg, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (const uint32_t r = CheckProgressCall(int32_t(Arg(c, 0)))) return r;
  if (SdkVersion() >= 0x01700000 && !Shorter(Arg(c, 1), 0x2000)) return kErrParam;
  g_dialog->text = recomp::GStr(Arg(c, 1)).substr(0, MsgMax() - 1);
  recomp::orbis::videoout::SetOverlay(Draw(*g_dialog));
  return 0;
}
