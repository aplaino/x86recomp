// libSceSaveDataDialog HLE: the system's save data dialog, drawn by the
// runtime over the game's frames (videoout::SetOverlay) while it has the
// controller (pad::SetIntercepted). The library's side is libSceSaveData-
// Dialog 11.00's: the states, the checks of SceSaveDataDialogParam and
// what it points to (some depend on the game's SDK version), the codes
// (its own, and libSceSaveData's that it turns into its own) and the
// result's layout. The system's side, the dialog the player sees and
// answers, is this runtime's own design: the console's look and wording
// are not copied. docs/hle.md has what is not measured. Implemented: the
// list for loading and the progress message; the other lists, messages
// and modes are fatal.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/dialog.h"
#include "orbis/hle/pad.h"
#include "orbis/hle/savedata.h"
#include "orbis/hle/videoout.h"
#include "orbis/process.h"
#include "runtime/ui/canvas.h"
#include "x86/runtime.h"

namespace {

using recomp::gaddr;
using recomp::Rd32;
using recomp::Rd64;
namespace pad = recomp::orbis::pad;
namespace savedata = recomp::orbis::savedata;

constexpr uint32_t kErrNotInitialized = 0x80B80003, kErrAlreadyInitialized = 0x80B80004,
                   kErrNotFinished = 0x80B80005, kErrFinished = 0x80B80006,
                   kErrParam = 0x80B8000A, kErrNotRunning = 0x80B8000B,
                   kErrArgNull = 0x80B8000D;
constexpr uint32_t kUser = 0x10000000;  // the one user (libSceUserService)
constexpr uint32_t kParamSize = 152;    // SceSaveDataDialogParam after its base

// SceSaveDataDialogStatus.
enum : uint32_t { kNone, kInitialized, kRunning, kFinished };
// Modes, list types, system messages, focus positions and item styles.
enum : uint32_t { kModeList = 1, kModeSystemMsg = 3 };
enum : uint32_t { kTypeSave = 1, kTypeLoad = 2, kTypeDelete = 3 };
enum : uint32_t { kMsgNoData = 1, kMsgNoSpace = 4, kMsgProgress = 5, kMsgNoSpaceContinuable = 8 };
enum : uint32_t {
  kFocusListHead, kFocusListTail, kFocusDataHead, kFocusDataTail, kFocusLatest, kFocusOldest,
  kFocusDirName,
};
enum : uint32_t { kStyleDateSizeSubtitle, kStyleSubtitleDateSize, kStyleDateSize };
// Results.
constexpr int32_t kResultOk = 0, kResultCanceled = 1;

// The game's compiled SDK version (its process parameters').
uint32_t SdkVersion() {
  static const uint32_t v = [] {
    const gaddr p = recomp::orbis::ProcParam();
    return p ? Rd32(p + 0x10) : 0u;
  }();
  return v;
}

struct Item {
  std::string dir;
  savedata::Entry e;
  std::vector<uint32_t> icon;  // A8R8G8B8; empty: none
  int icon_w = 0, icon_h = 0;
};

struct Dialog {
  uint32_t mode, type, style = 0, msg = 0;
  gaddr user_data;
  std::vector<Item> items;
  size_t focus = 0, top = 0;
};

std::mutex g_mu;
uint32_t g_status = kNone;
std::shared_ptr<Dialog> g_dialog;  // the one open or last finished
// The answer, once finished.
int32_t g_result = 0;
std::string g_chosen;  // the directory picked
uint8_t g_chosen_param[savedata::kParamSize];

bool Zero(gaddr p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (recomp::GPtr(p)[i]) return false;
  return true;
}

// A directory name the library accepts: 1-31 of A-Z, a-z, 0-9, '-' and
// '_' in its 32 bytes.
bool ValidDirName(gaddr p) {
  const uint8_t* s = recomp::GPtr(p);
  if (!s[0]) return false;
  for (int i = 0; i < 32; i++) {
    if (!s[i]) return true;
    if (!std::isalnum(s[i]) && s[i] != '-' && s[i] != '_') return false;
  }
  return false;
}

// SceCommonDialogBaseParam: its size (48), 36 zero bytes and the magic,
// 0xC0D1A109 plus the low 32 bits of its address.
bool ValidBase(gaddr p) {
  return Rd64(p) == 48 && Zero(p + 8, 36) && Rd32(p + 44) == uint32_t(p) + 0xC0D1A109u;
}

// The items (SceSaveDataDialogItems): {user, title id*, directory names*
// (32 bytes each), their number (to 1024), new item*, focus position, its
// directory name*, item style, 36 zero bytes}. The user is signed in and
// registered; save data of other titles and new items are not
// implemented yet.
uint32_t CheckItemsCommon(gaddr it, uint32_t type) {
  if (Rd32(it) != kUser) return kErrParam;
  if (const gaddr title = Rd64(it + 8); title && !savedata::OwnTitle(title))
    recomp::FatalF("sceSaveDataDialogOpen: save data of title %s not implemented yet",
                   recomp::GStr(title).c_str());
  if (Rd32(it + 24) > 1024) return kErrParam;
  if (type == kTypeSave && Rd64(it + 32))
    recomp::FatalF("sceSaveDataDialogOpen: a new item not implemented yet");
  return 0;
}

// A list's items: loading and deleting list at least one directory, all
// valid and different; a focus position (0-6), its directory only with
// position 6; a style (0-2).
uint32_t CheckListItems(gaddr it, uint32_t type) {
  if (uint32_t e = CheckItemsCommon(it, type)) return e;
  const gaddr names = Rd64(it + 16);
  const uint32_t n = Rd32(it + 24);
  if ((type == kTypeLoad || type == kTypeDelete) && (!n || !names)) return kErrParam;
  if (names) {
    for (uint32_t i = 0; i < n; i++)
      if (!ValidDirName(names + 32 * uint64_t(i))) return kErrParam;
    for (uint32_t i = 0; i < n; i++)
      for (uint32_t j = 0; j < n; j++)
        if (i != j && recomp::GStr(names + 32 * uint64_t(i)) == recomp::GStr(names + 32 * uint64_t(j)))
          return kErrParam;
  }
  const uint32_t focus = Rd32(it + 40);
  if (focus > kFocusDirName) return kErrParam;
  if (const gaddr fd = Rd64(it + 48))
    if (focus != kFocusDirName || !ValidDirName(fd)) return kErrParam;
  if (Rd32(it + 56) > kStyleDateSize || !Zero(it + 60, 36)) return kErrParam;
  return 0;
}

// A system message's items: the first directory (if named) valid, no
// focus, a style (0-2, checked from SDK 6.00).
uint32_t CheckMessageItems(gaddr it, uint32_t type) {
  if (uint32_t e = CheckItemsCommon(it, type)) return e;
  if (const gaddr names = Rd64(it + 16))
    if (recomp::GPtr(names)[0] && !ValidDirName(names)) return kErrParam;
  if (Rd32(it + 40) || Rd64(it + 48)) return kErrParam;
  if (SdkVersion() >= 0x6000000 && Rd32(it + 56) > kStyleDateSize) return kErrParam;
  return Zero(it + 60, 36) ? 0 : kErrParam;
}

// A system message ({type, value, 32 zero bytes}): types 1-15; 3, 4, 8
// and 14 only for saving; 9 and 15 and 10, 11 and 13 only for saving or
// loading. Which need directories depends on the SDK version.
uint32_t CheckSystemMessage(gaddr m, uint32_t type, gaddr it) {
  const uint32_t t = Rd32(m);
  if (t - 1 >= 15) return kErrParam;
  if (type != kTypeSave && (t == 3 || t == kMsgNoSpace || t == kMsgNoSpaceContinuable || t == 14))
    return kErrParam;
  if ((t == 9 || t == 15 || t == 10 || t == 11 || t == 13) && type != kTypeSave && type != kTypeLoad)
    return kErrParam;
  if (!Zero(m + 16, 32)) return kErrParam;
  const gaddr names = Rd64(it + 16);
  const uint32_t n = Rd32(it + 24);
  const uint32_t sdk = SdkVersion();
  const bool no_dir = t == kMsgNoData || t == kMsgNoSpace || t == kMsgNoSpaceContinuable;
  if (sdk >= 0x2000000) {
    if (!no_dir && t != 14) {
      if (t == 9 || t == 13) {
        if (!names || !recomp::GPtr(names)[0] || Rd64(it + 32)) return kErrParam;
      } else if (t == 15) {
        if (!names || !n || Rd64(it + 32)) return kErrParam;
      } else if (!!names != !!n) {
        return kErrParam;
      }
    }
  } else if (sdk >= 0x1700000) {
    if (!no_dir && !n) return kErrParam;
  } else if ((t == kMsgNoData) != !n) {
    return kErrParam;
  }
  if (t == 12) recomp::FatalF("sceSaveDataDialogOpen: system message 12 not implemented yet");
  return 0;
}

// What the library checks of any mode's parameter: the mode (1-7), the
// list type (1-3), the animations ({OK, cancel}, each 0 or 1, 32 zero
// bytes), the items, the one pointer the mode takes (user message 80,
// system message 88, error code 96, progress bar 104, or 128 for 6 and
// 7) and none of the others, the options ({back, 0, 28 zero bytes}) and
// 16 zero bytes.
uint32_t CheckParam(gaddr p) {
  const uint32_t mode = Rd32(p + 52), type = Rd32(p + 56);
  if (mode - 1 >= 7 || type - 1 >= 3) return kErrParam;
  if (const gaddr anim = Rd64(p + 64))
    if (Rd32(anim) > 1 || Rd32(anim + 4) > 1 || !Zero(anim + 8, 32)) return kErrParam;
  if (!Rd64(p + 72)) return kErrParam;
  static const uint32_t kOwn[8] = {0, 0, 80, 88, 96, 104, 128, 128};
  for (uint32_t at : {80u, 88u, 96u, 104u, 128u})
    if (!!Rd64(p + at) != (at == kOwn[mode])) return kErrParam;
  if (const gaddr opt = Rd64(p + 120))
    if (Rd32(opt) > 1 || Rd32(opt + 4) || !Zero(opt + 8, 28)) return kErrParam;
  if (!Zero(p + 136, 16)) return kErrParam;
  return 0;
}

std::string Field(const uint8_t* p, size_t n) {
  return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), n));
}

// ---- the dialog the player sees (this runtime's own)

constexpr int kW = 1920, kH = 1080;
constexpr int kRowH = 168, kListX = 120, kListY = 210, kListW = 1120, kRows = 4;
constexpr int kIconW = 228, kIconH = 128;  // a save data icon's size
constexpr uint32_t kText = 0xFFFFFFFF, kDim = 0xFFB0B8C8;

void LoadIcon(Item& it) {
  if (it.e.icon.empty()) return;
  SDL_Surface* s = SDL_LoadPNG(it.e.icon.c_str());
  if (!s) return;
  if (SDL_Surface* a = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888)) {
    it.icon_w = a->w;
    it.icon_h = a->h;
    it.icon.resize(size_t(a->w) * a->h);
    for (int y = 0; y < a->h; y++)
      std::memcpy(&it.icon[size_t(y) * a->w], static_cast<uint8_t*>(a->pixels) + y * a->pitch,
                  size_t(a->w) * 4);
    SDL_DestroySurface(a);
  }
  SDL_DestroySurface(s);
}

// The listed directories that exist, in the game's order.
std::vector<Item> FindItems(gaddr it) {
  std::vector<Item> out;
  const gaddr names = Rd64(it + 16);
  for (uint32_t i = 0; names && i < Rd32(it + 24); i++) {
    Item item;
    item.dir = recomp::GStr(names + 32 * uint64_t(i));
    if (!savedata::Find(item.dir, &item.e)) continue;
    LoadIcon(item);
    out.push_back(std::move(item));
  }
  return out;
}

std::string When(int64_t t) {
  const time_t tt = time_t(t);
  std::tm tm{};
  gmtime_r(&tt, &tm);  // the system's time zone is UTC (libSceSystemService)
  char b[32];
  std::strftime(b, sizeof b, "%Y/%m/%d %H:%M", &tm);
  return b;
}

std::string Size(uint64_t blocks) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f MB", double(blocks) * 32 / 1024);
  return b;
}

// Lines of text no wider than w, at the newlines and spaces.
std::vector<std::string> Wrap(const std::string& s, int size, int w) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    if (nl == std::string::npos) nl = s.size();
    const std::string para = s.substr(start, nl - start);
    std::string line;
    for (size_t i = 0; i <= para.size();) {
      size_t sp = para.find(' ', i);
      if (sp == std::string::npos) sp = para.size();
      const std::string word = para.substr(i, sp - i);
      const std::string next = line.empty() ? word : line + " " + word;
      if (!line.empty() && recomp::ui::TextWidth(size, next) > w) {
        out.push_back(line);
        line = word;
      } else {
        line = next;
      }
      i = sp + 1;
    }
    out.push_back(line);
    start = nl + 1;
  }
  return out;
}

// An item's icon and lines (title, then the date and size and the
// subtitle in the order of the style) from (x, y).
void DrawItem(recomp::ui::Canvas& c, const Item& it, uint32_t style, int x, int y, int right) {
  if (!it.icon.empty())
    c.Draw(x, y, kIconW, kIconH, it.icon.data(), it.icon_w, it.icon_h);
  else
    c.Fill(x, y, kIconW, kIconH, 0x30FFFFFF);
  const int tx = x + kIconW + 28, tw = right - tx;
  const std::string date = When(it.e.mtime) + "    " + Size(it.e.blocks);
  const std::string sub = Field(it.e.param + 0x80, 0x80);
  std::vector<std::pair<std::string, uint32_t>> lines = {{Field(it.e.param, 0x80), kText}};
  if (style == kStyleSubtitleDateSize) lines.push_back({sub, kDim});
  lines.push_back({date, kDim});
  if (style == kStyleDateSizeSubtitle) lines.push_back({sub, kDim});
  int ly = y + 40;
  for (size_t k = 0; k < lines.size(); k++) {
    c.Text(tx, ly, k ? 28 : 36, lines[k].first, lines[k].second, tw);
    ly += 44;
  }
}

std::shared_ptr<recomp::ui::Canvas> DrawList(const Dialog& d) {
  auto c = std::make_shared<recomp::ui::Canvas>(kW, kH);
  c->Fill(0, 0, kW, kH, 0xD0101418);
  c->Text(kListX, 140, 52, "Load", kText);
  c->Fill(kListX, 162, kW - 2 * kListX, 2, 0x60FFFFFF);
  for (size_t i = d.top; i < d.items.size() && i < d.top + kRows; i++) {
    const int y = kListY + int(i - d.top) * kRowH;
    if (i == d.focus) {
      c->Fill(kListX - 12, y, kListW + 24, kRowH - 12, 0x40FFFFFF);
      c->Fill(kListX - 12, y, 6, kRowH - 12, 0xFF3C8CFF);
    }
    DrawItem(*c, d.items[i], d.style, kListX + 8, y + 14, kListX + kListW);
  }
  // The focused item's details.
  const Item& f = d.items[d.focus];
  const int dx = kListX + kListW + 60, dw = kW - kListX - dx;
  int y = kListY + 40;
  for (const std::string& l : Wrap(Field(f.e.param + 0x100, 0x400), 30, dw)) {
    c->Text(dx, y, 30, l, kDim, dw);
    y += 42;
  }
  char n[32];
  std::snprintf(n, sizeof n, "%zu / %zu", d.focus + 1, d.items.size());
  c->Text(kListX, kH - 80, 30, n, kDim);
  const std::string hint = "✕ Load      ○ Back";
  c->Text(kW - kListX - recomp::ui::TextWidth(32, hint), kH - 80, 32, hint, kText);
  return c;
}

// A message over the bottom of the frame: the item it is about (if any)
// and the text, with `dots` of an ellipsis that moves while it works.
std::shared_ptr<recomp::ui::Canvas> DrawMessage(const Dialog& d, const std::string& text, int dots) {
  auto c = std::make_shared<recomp::ui::Canvas>(kW, kH);
  const int h = 300, y = kH - h;
  c->Fill(0, y, kW, h, 0xE0101418);
  c->Fill(0, y, kW, 2, 0x60FFFFFF);
  int x = kListX;
  if (!d.items.empty()) {
    DrawItem(*c, d.items[0], d.style, x, y + 50, kW / 2);
    x = kW / 2 + 60;
  }
  const int w = c->Text(x, y + 160, 40, text, kText);
  c->Text(x + w, y + 160, 40, std::string(size_t(dots), '.'), kText);
  return c;
}

// Ends the dialog with the answer (`picked`: the player chose the focused
// item), unless it has ended already. Under g_mu.
void Finish(Dialog* d, int32_t result, bool picked = false) {
  if (g_status != kRunning || g_dialog.get() != d) return;
  recomp::orbis::videoout::SetOverlay(nullptr);
  pad::SetIntercepted(false);
  g_result = result;
  std::memset(g_chosen_param, 0, sizeof g_chosen_param);
  g_chosen.clear();
  if (picked) {
    const Item& it = d->items[d->focus];
    g_chosen = it.dir;
    std::memcpy(g_chosen_param, it.e.param, sizeof g_chosen_param);
  }
  g_status = kFinished;
}

bool Open(const Dialog* d) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status == kRunning && g_dialog.get() == d;
}

// The list: up and down (held: repeated) move the focus, cross picks,
// circle backs out. Buttons held when the dialog opens count only once
// released.
void RunList(std::shared_ptr<Dialog> d) {
  using Clock = std::chrono::steady_clock;
  uint32_t prev = pad::Buttons();
  Clock::time_point held_since{}, last_repeat{};
  while (Open(d.get())) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    const uint32_t b = pad::Buttons(), down = b & ~prev;
    prev = b;
    if (down & (pad::kCross | pad::kCircle)) {
      std::lock_guard<std::mutex> l(g_mu);
      const bool cross = down & pad::kCross;
      return Finish(d.get(), cross ? kResultOk : kResultCanceled, cross);
    }
    const auto now = Clock::now();
    int move = 0;
    if (down & (pad::kUp | pad::kDown)) {
      move = down & pad::kUp ? -1 : 1;
      held_since = last_repeat = now;
    } else if (b & (pad::kUp | pad::kDown) && now - held_since > std::chrono::milliseconds(400) &&
               now - last_repeat > std::chrono::milliseconds(100)) {
      move = b & pad::kUp ? -1 : 1;
      last_repeat = now;
    }
    if (!move) continue;
    const size_t f = size_t(std::clamp<ptrdiff_t>(ptrdiff_t(d->focus) + move, 0,
                                                  ptrdiff_t(d->items.size()) - 1));
    if (f == d->focus) continue;
    d->focus = f;
    if (f < d->top) d->top = f;
    if (f >= d->top + kRows) d->top = f - kRows + 1;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_status == kRunning && g_dialog == d) recomp::orbis::videoout::SetOverlay(DrawList(*d));
  }
}

// The progress message, until the game closes it.
void RunProgress(std::shared_ptr<Dialog> d, std::string text) {
  for (int tick = 1; Open(d.get()); tick++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::lock_guard<std::mutex> l(g_mu);
    if (g_status == kRunning && g_dialog == d)
      recomp::orbis::videoout::SetOverlay(DrawMessage(*d, text, tick % 4));
  }
}

}  // namespace

using recomp::hle::Arg;

ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogInitialize, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status != kNone) return kErrAlreadyInitialized;
  if (!recomp::orbis::dialog::CommonInitialized())
    recomp::FatalF("sceSaveDataDialogInitialize before sceCommonDialogInitialize not implemented yet");
  g_status = kInitialized;
  return 0;
}

ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogTerminate, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (g_status == kRunning)
    recomp::FatalF("sceSaveDataDialogTerminate with the dialog open not implemented yet");
  g_status = kNone;
  return 0;
}

ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogGetStatus, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status;
}

ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogUpdateStatus, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status;
}

// int sceSaveDataDialogIsReadyToDisplay(): 1 once the dialog shows.
ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogIsReadyToDisplay, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (g_status == kFinished) return kErrFinished;
  if (g_status != kRunning)
    recomp::FatalF("sceSaveDataDialogIsReadyToDisplay before Open not implemented yet");
  return 1;
}

// int sceSaveDataDialogOpen(const SceSaveDataDialogParam* p): p is
// {SceCommonDialogBaseParam, size (152), mode, list type, animations*,
// items*, user message*, system message*, error code*, progress bar*,
// user data, options*, mode 6 and 7's parameter*, 16 zero bytes}.
ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogOpen, 1) {
  const gaddr p = Arg(c, 0);
  if (!p) return kErrArgNull;
  if (!ValidBase(p) || Rd32(p + 48) != kParamSize) return kErrParam;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_status == kNone) return kErrNotInitialized;
    if (g_status == kRunning) recomp::FatalF("sceSaveDataDialogOpen: a dialog is open already");
  }
  if (uint32_t e = CheckParam(p)) return e;
  auto d = std::make_shared<Dialog>();
  d->mode = Rd32(p + 52);
  d->type = Rd32(p + 56);
  d->user_data = Rd64(p + 112);
  const gaddr items = Rd64(p + 72);
  d->style = Rd32(items + 56);
  std::shared_ptr<recomp::ui::Canvas> first;
  std::function<void()> run;  // started once the dialog is open
  if (d->mode == kModeList) {
    if (uint32_t e = CheckListItems(items, d->type)) return e;
    if (d->type != kTypeLoad)
      recomp::FatalF("sceSaveDataDialogOpen: list type %u not implemented yet", d->type);
    d->items = FindItems(items);
    if (d->items.empty())
      recomp::FatalF("sceSaveDataDialogOpen: a list with no save data not implemented yet");
    const size_t last = d->items.size() - 1;
    const uint32_t focus = Rd32(items + 40);
    if (focus == kFocusListTail || focus == kFocusDataTail) d->focus = last;
    for (size_t i = 0; i <= last; i++) {
      const int64_t t = d->items[i].e.mtime, best = d->items[d->focus].e.mtime;
      if ((focus == kFocusLatest && t > best) || (focus == kFocusOldest && t < best) ||
          (focus == kFocusDirName && d->items[i].dir == recomp::GStr(Rd64(items + 48))))
        d->focus = i;
    }
    d->top = d->focus >= size_t(kRows) ? d->focus - kRows + 1 : 0;
    first = DrawList(*d);
    run = [d] { RunList(d); };
  } else if (d->mode == kModeSystemMsg) {
    const gaddr m = Rd64(p + 88);
    if (uint32_t e = CheckMessageItems(items, d->type)) return e;
    if (uint32_t e = CheckSystemMessage(m, d->type, items)) return e;
    d->msg = Rd32(m);
    if (d->msg != kMsgProgress)
      recomp::FatalF("sceSaveDataDialogOpen: system message %u not implemented yet", d->msg);
    d->items = FindItems(items);
    d->items.resize(std::min<size_t>(d->items.size(), 1));
    static const char* const kVerb[4] = {"", "Saving", "Loading", "Deleting"};
    first = DrawMessage(*d, kVerb[d->type], 0);
    run = [d, verb = std::string(kVerb[d->type])] { RunProgress(d, verb); };
  } else {
    recomp::FatalF("sceSaveDataDialogOpen: mode %u not implemented yet", d->mode);
  }
  std::lock_guard<std::mutex> l(g_mu);
  pad::SetIntercepted(true);
  recomp::orbis::videoout::SetOverlay(first);
  g_dialog = d;
  g_status = kRunning;
  std::thread(run).detach();
  return 0;
}

// int sceSaveDataDialogClose(const SceSaveDataDialogCloseParam* p): p is
// {animation (0 or 1), 32 zero bytes}; the game ends the open dialog.
ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogClose, 1) {
  const gaddr p = Arg(c, 0);
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (!p) return kErrArgNull;
  if (g_status != kRunning) return kErrNotRunning;
  if (Rd32(p) > 1 || !Zero(p + 4, 32)) return kErrParam;
  Finish(g_dialog.get(), kResultOk);
  return 0;
}

// int sceSaveDataDialogGetResult(SceSaveDataDialogResult* r): r is {mode,
// result, button, directory name* (32 bytes), param* (SceSaveDataParam),
// user data, 32 zero bytes}; the button is 0 (none) for a list.
ORBIS_HLE("libSceSaveDataDialog", sceSaveDataDialogGetResult, 1) {
  const gaddr r = Arg(c, 0);
  if (!r) return kErrArgNull;
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status == kNone) return kErrNotInitialized;
  if (g_status != kFinished) return kErrNotFinished;
  if (!Zero(r + 40, 32)) return kErrParam;
  recomp::Wr32(r, g_dialog->mode);
  recomp::Wr32(r + 4, uint32_t(g_result));
  recomp::Wr32(r + 8, 0);
  if (const gaddr dir = Rd64(r + 16)) {
    std::memset(recomp::GPtr(dir), 0, 32);
    recomp::PutStr(dir, g_chosen);
  }
  if (const gaddr param = Rd64(r + 24))
    std::memcpy(recomp::GPtr(param), g_chosen_param, savedata::kParamSize);
  recomp::Wr64(r + 32, g_dialog->user_data);
  return 0;
}
