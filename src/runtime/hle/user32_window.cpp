// user32: window classes, windows, message queues and the SDL3 event pump.
//
// Every top-level window gets an SDL window sized to its client area; child
// windows are bookkeeping only. SDL events are translated into WM_* messages
// on the queue of the thread that owns the window, and DispatchMessage calls
// the guest WndProc. Messages Windows sends synchronously (creation,
// activation, size) are delivered by calling the WndProc directly.

#include <SDL3/SDL.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/watchdog.h"

namespace recomp {

// SDL_MouseID of <ID>_TEST_INPUT "pointer" events: they stand for the host
// pointer, which automated (background) runs otherwise ignore.
constexpr SDL_MouseID kTestPointer = 0xD51D51;

bool Background() {
  static const bool bg = [] {
    const char* e = Env("BACKGROUND");
    return e && *e && *e != '0';
  }();
  return bg;
}

namespace {


// ---- messages and styles -----------------------------------------------------
enum : uint32_t {
  WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003,
  WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007,
  WM_KILLFOCUS = 0x0008, WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D,
  WM_GETTEXTLENGTH = 0x000E, WM_CLOSE = 0x0010,
  WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014, WM_SHOWWINDOW = 0x0018,
  WM_SETFONT = 0x0030, WM_GETFONT = 0x0031, WM_TIMER = 0x0113,
  WM_ACTIVATEAPP = 0x001C, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082,
  WM_NCHITTEST = 0x0084, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102,
  WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106,
  WM_SYSCOMMAND = 0x0112, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
  WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204,
  WM_RBUTTONUP = 0x0205, WM_RBUTTONDBLCLK = 0x0206, WM_MBUTTONDOWN = 0x0207,
  WM_MBUTTONUP = 0x0208, WM_MBUTTONDBLCLK = 0x0209, WM_MOUSEWHEEL = 0x020A,
};
constexpr uint32_t WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000,
                   WS_CAPTION = 0x00C00000,
                   WS_THICKFRAME = 0x00040000;
constexpr uint32_t CS_DBLCLKS = 0x0008;
constexpr int32_t GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8,
                  GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21,
                  GWL_ID = -12;

struct WndClass {
  std::string name;  // lower case
  uint32_t style = 0, wndproc = 0, wnd_extra = 0;
  uint16_t atom = 0;
};

struct Window {
  uint32_t hwnd = 0;
  std::string cls;
  uint32_t wndproc = 0, style = 0, exstyle = 0, parent = 0, id = 0;
  uint32_t hinst = 0, userdata = 0, class_style = 0;
  int x = 0, y = 0, w = 0, h = 0;  // window rect (screen)
  std::string title;
  std::vector<uint8_t> extra;      // cbWndExtra bytes (GetWindowLong >= 0)
  uint32_t tid = 0;
  bool visible = false, enabled = true, destroyed = false;
  bool minimized = false;      // as the game asked (IsIconic)
  int mode_w = 0, mode_h = 0;  // DirectDraw exclusive mode, see header
  SDL_Window* sdl = nullptr;
  // Built-in controls (see "built-in control classes"): the text is
  // `title`; selection, font, image and check state are kept here.
  uint32_t sel_lo = 0, sel_hi = 0, font = 0, image = 0, check = 0;
  uint32_t limit = 30000;  // edit text limit (EM_SETLIMITTEXT)
};

struct Msg {
  uint32_t hwnd, msg, wparam, lparam, time;
  int32_t ptx, pty;
};

struct Queue {
  std::deque<Msg> msgs;
  bool quit = false;
  uint32_t quit_code = 0;
};

// SetTimer: WM_TIMER is not queued but synthesised when the owning
// thread's queue has nothing else for the caller, as on Windows.
struct Timer {
  uint32_t hwnd, id, elapse, proc, tid;
  int64_t due;  // TickCount() of the next WM_TIMER
};

std::recursive_mutex g_mu;
std::map<std::string, WndClass> g_classes;
std::map<uint32_t, Window> g_windows;
std::map<uint32_t, Queue> g_queues;  // by tid
std::vector<Timer> g_timers;

// ClipCursor: while the game confines the cursor to one of our windows
// (Quake 3-style mouse look), the cursor is virtual, as in exclusive mode.
struct Clip {
  bool on = false;
  int32_t l = 0, t = 0, r = 0, b = 0;  // screen coordinates
  uint32_t hwnd = 0;
};
Clip g_clip;
uint32_t g_capture = 0;  // SetCapture

// RegisterHotKey: a matching key press becomes WM_HOTKEY instead of a key
// message, as on Windows (here only while one of our windows has focus).
struct HotKey {
  uint32_t hwnd, id, mods, vk, tid;
};
std::vector<HotKey> g_hotkeys;
std::condition_variable_any g_queue_cv;
uint32_t g_next_hwnd = 0x00010010;
uint16_t g_next_atom = 0xC100;
uint32_t g_focus = 0, g_active = 0;
int g_cursor_count = 0;  // ShowCursor display counter
int32_t g_mouse_x = 0, g_mouse_y = 0;  // screen coordinates
// The host pointer over the game window, in client (mode) coordinates;
// valid while g_pointer_inside. Written on the main thread.
std::atomic<bool> g_pointer_inside{false};
std::atomic<int32_t> g_real_x{0}, g_real_y{0};
// When the game last re-centred the cursor (it does so every frame while
// it steers its own pointer by relative motion).
std::atomic<int64_t> g_last_recentre_ms{-100000};
std::atomic<int32_t> g_recentre_x{0}, g_recentre_y{0};  // screen coordinates
bool g_sdl_up = false;
std::thread::id g_main_thread_id = std::this_thread::get_id();

Window* Win(uint32_t hwnd) {
  auto it = g_windows.find(hwnd);
  return it == g_windows.end() || it->second.destroyed ? nullptr : &it->second;
}

// Frame sizes for a top-level style: border on each side, caption on top.
void FrameSize(uint32_t style, int* border, int* caption) {
  bool framed = style & (WS_CAPTION | WS_THICKFRAME);
  *border = !framed ? 0 : (style & WS_THICKFRAME) ? 4 : 3;
  *caption = framed && (style & WS_CAPTION) == WS_CAPTION ? 19 : 0;
}

// Client-area size for a window rect, mirroring what AdjustWindowRect added.
void ClientSize(const Window& w, int* cw, int* ch) {
  int border = 0, caption = 0;
  if (!(w.style & WS_CHILD)) FrameSize(w.style, &border, &caption);
  *cw = std::max(0, w.w - 2 * border);
  *ch = std::max(0, w.h - 2 * border - caption);
}

void PostTo(uint32_t tid, const Msg& m) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_queues[tid].msgs.push_back(m);
  }
  g_queue_cv.notify_all();
}

void Post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t tid;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    tid = w ? w->tid : CurrentThread()->tid;
  }
  PostTo(tid, Msg{hwnd, msg, wp, lp, TickCount(), g_mouse_x, g_mouse_y});
}

// Synchronous delivery to the guest WndProc (SendMessage semantics).
uint32_t Send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t proc;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (w->tid != CurrentThread()->tid)
      WarnF("SendMessage %X to hwnd %X owned by thread %X from thread %X: "
            "delivered on the calling thread\n",
            msg, hwnd, w->tid, CurrentThread()->tid);
    proc = w->wndproc;
  }
  Ctx& c = CurrentThread()->ctx;
  return CallGuest(c, proc, {hwnd, msg, wp, lp});
}

uint32_t MkLparam(int lo, int hi) {
  return (uint32_t(uint16_t(lo))) | (uint32_t(uint16_t(hi)) << 16);
}

// Top-level window under SDL window id.
uint32_t HwndForSdl(SDL_WindowID id) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows)
    if (w.sdl && SDL_GetWindowID(w.sdl) == id && !w.destroyed) return h;
  return 0;
}

// ---- SDL key -> Windows VK ----------------------------------------------------

uint8_t VkFromScancode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return uint8_t('A' + (sc - SDL_SCANCODE_A));
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return uint8_t('1' + (sc - SDL_SCANCODE_1));
  if (sc == SDL_SCANCODE_0) return '0';
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return uint8_t(0x70 + (sc - SDL_SCANCODE_F1));
  switch (sc) {
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_KP_1: return 0x61;
    case SDL_SCANCODE_KP_2: return 0x62;
    case SDL_SCANCODE_KP_3: return 0x63;
    case SDL_SCANCODE_KP_4: return 0x64;
    case SDL_SCANCODE_KP_5: return 0x65;
    case SDL_SCANCODE_KP_6: return 0x66;
    case SDL_SCANCODE_KP_7: return 0x67;
    case SDL_SCANCODE_KP_8: return 0x68;
    case SDL_SCANCODE_KP_9: return 0x69;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: return 0xA2;
    case SDL_SCANCODE_RCTRL: return 0xA3;
    case SDL_SCANCODE_LSHIFT: return 0xA0;
    case SDL_SCANCODE_RSHIFT: return 0xA1;
    case SDL_SCANCODE_LALT: return 0xA4;
    case SDL_SCANCODE_RALT: return 0xA5;
    case SDL_SCANCODE_LGUI: return 0x5B;
    case SDL_SCANCODE_RGUI: return 0x5C;
    case SDL_SCANCODE_APPLICATION: return 0x5D;
    default: return 0;
  }
}

// Messages use the generic VK for modifiers; key state tracks both.
uint8_t GenericVk(uint8_t vk) {
  switch (vk) {
    case 0xA0: case 0xA1: return 0x10;
    case 0xA2: case 0xA3: return 0x11;
    case 0xA4: case 0xA5: return 0x12;
    default: return vk;
  }
}

uint32_t MouseKeys() {
  uint32_t mk = 0;
  if (g_key_down[0x01]) mk |= 0x01;  // MK_LBUTTON
  if (g_key_down[0x02]) mk |= 0x02;  // MK_RBUTTON
  if (g_key_down[0x10]) mk |= 0x04;  // MK_SHIFT
  if (g_key_down[0x11]) mk |= 0x08;  // MK_CONTROL
  if (g_key_down[0x04]) mk |= 0x10;  // MK_MBUTTON
  return mk;
}

void SetKey(uint8_t vk, bool down) {
  if (down && !g_key_down[vk]) {
    g_key_pressed_since[vk] = 1;
    g_key_toggled[vk] = !g_key_toggled[vk];
  }
  g_key_down[vk] = down;
}

void WindowOrigin(uint32_t hwnd, int* x, int* y) {
  *x = *y = 0;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    int cw, ch;
    ClientSize(*w, &cw, &ch);
    *x = w->x + (w->w - cw) / 2;
    *y = w->y + (w->h - ch) - (w->w - cw) / 2;
  }
}

// True (with the mode size) while hwnd is in DirectDraw exclusive mode.
bool ExclusiveSize(uint32_t hwnd, int* w, int* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* win = Win(hwnd);
  if (!win || !win->mode_w) return false;
  *w = win->mode_w;
  *h = win->mode_h;
  return true;
}

// SDL window coordinates -> client coordinates. In exclusive mode the
// client area is the display mode, whatever size the host window really
// has (resized by the user, or by desktop scaling), so scale.
void ModeScale(uint32_t hwnd, float* sx, float* sy) {
  *sx = *sy = 1.0f;
  SDL_Window* sdl;
  int mw, mh;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->mode_w || !w->sdl) return;
    sdl = w->sdl;
    mw = w->mode_w;
    mh = w->mode_h;
  }
  int ww = 0, wh = 0;
  if (!SDL_GetWindowSize(sdl, &ww, &wh) || ww <= 0 || wh <= 0) return;
  *sx = float(mw) / ww;
  *sy = float(mh) / wh;
}

void ActivateApp(uint32_t hwnd, bool active) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (active) {
      if (g_active == hwnd) return;  // already active: nothing to report
      g_active = g_focus = hwnd;
    } else {
      if (g_active != hwnd) return;
      g_active = g_focus = 0;
    }
  }
  WatchdogAppActive(active);
  Post(hwnd, WM_ACTIVATEAPP, active ? 1 : 0, 0);
  Post(hwnd, WM_ACTIVATE, active ? 1 : 0, 0);
  Post(hwnd, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

void HandleSdlEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_QUIT: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      for (auto& [h, w] : g_windows)
        if (w.sdl && !w.destroyed) Post(h, WM_CLOSE, 0, 0);
      break;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (uint32_t h = HwndForSdl(e.window.windowID)) Post(h, WM_CLOSE, 0, 0);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      if (uint32_t h = HwndForSdl(e.window.windowID)) ActivateApp(h, true);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (uint32_t h = HwndForSdl(e.window.windowID)) {
        // Release keys so nothing stays stuck while we're away.
        for (int vk = 0; vk < 256; vk++) g_key_down[vk] = 0;
        ActivateApp(h, false);
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      uint32_t h = HwndForSdl(e.key.windowID);
      uint8_t vk = VkFromScancode(e.key.scancode);
      if (!h || !vk) break;
      bool down = e.type == SDL_EVENT_KEY_DOWN;
      bool was_down = g_key_down[GenericVk(vk)];
      SetKey(vk, down);
      SetKey(GenericVk(vk), down);
      uint8_t gvk = GenericVk(vk);
      bool ext;
      uint8_t sc = ScanCodeForVk(vk, &ext);
      if (e.key.scancode == SDL_SCANCODE_KP_ENTER) ext = true;
      uint32_t lp = 1u | (uint32_t(sc) << 16) | (ext ? 1u << 24 : 0);
      if (!down) lp |= 0xC0000000u;
      else if (was_down) lp |= 0x40000000u;
      bool alt = g_key_down[0x12];
      bool sys = alt || gvk == 0x79 /* F10 */ || gvk == 0x12;
      if (alt) lp |= 1u << 29;
      if (down) {
        uint32_t mods = (alt ? 1u : 0) | (g_key_down[0x11] ? 2u : 0) |
                        (g_key_down[0x10] ? 4u : 0);  // MOD_ALT, CONTROL, SHIFT
        const HotKey* hk = nullptr;
        {
          std::lock_guard<std::recursive_mutex> l(g_mu);
          for (const HotKey& k : g_hotkeys)
            if (k.vk == gvk && (k.mods & 7) == mods) hk = &k;
          if (hk) {
            HotKey k = *hk;
            Msg m{k.hwnd, 0x0312 /* WM_HOTKEY */, k.id, mods | gvk << 16, TickCount(),
                  g_mouse_x, g_mouse_y};
            PostTo(k.tid, m);
            break;
          }
        }
      }
      uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN)
                          : (sys ? WM_SYSKEYUP : WM_KEYUP);
      Post(h, msg, gvk, lp);
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      uint32_t h = HwndForSdl(e.motion.windowID);
      if (!h) break;
      int ox, oy;
      WindowOrigin(h, &ox, &oy);
      float kx, ky;
      ModeScale(h, &kx, &ky);
      int mw, mh;
      if (!Background() || e.motion.which == kTestPointer) {
        g_real_x = int(e.motion.x * kx);
        g_real_y = int(e.motion.y * ky);
        g_pointer_inside = true;
      }
      bool clipped = g_clip.on && g_clip.hwnd == h;
      if (clipped || ExclusiveSize(h, &mw, &mh)) {
        // Virtual cursor: the game recentres it with SetCursorPos every
        // frame and steers its own pointer by the difference, as on a
        // fullscreen Windows desktop. Real motion moves the virtual
        // position; the host pointer is never warped (while clipped it
        // is grabbed in SDL's relative mode instead).
        static float fx = 0, fy = 0;  // sub-pixel remainder
        fx += e.motion.xrel * kx;
        fy += e.motion.yrel * ky;
        int dx = int(fx), dy = int(fy);
        fx -= dx;
        fy -= dy;
        if (clipped) {
          g_mouse_x = std::clamp(g_mouse_x + dx, g_clip.l, std::max(g_clip.l, g_clip.r - 1));
          g_mouse_y = std::clamp(g_mouse_y + dy, g_clip.t, std::max(g_clip.t, g_clip.b - 1));
        } else {
          g_mouse_x = std::clamp(g_mouse_x + dx, ox, ox + mw - 1);
          g_mouse_y = std::clamp(g_mouse_y + dy, oy, oy + mh - 1);
        }
      } else {
        g_mouse_x = ox + int(e.motion.x * kx);
        g_mouse_y = oy + int(e.motion.y * ky);
      }
      Post(h, WM_MOUSEMOVE, MouseKeys(), MkLparam(g_mouse_x - ox, g_mouse_y - oy));
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      g_pointer_inside = false;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      uint32_t h = HwndForSdl(e.button.windowID);
      if (!h) break;
      bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      int ox, oy;
      WindowOrigin(h, &ox, &oy);
      int x = g_mouse_x - ox, y = g_mouse_y - oy;  // where motion left it
      uint32_t base;
      uint8_t vk;
      switch (e.button.button) {
        case SDL_BUTTON_LEFT: base = WM_LBUTTONDOWN; vk = 0x01; break;
        case SDL_BUTTON_RIGHT: base = WM_RBUTTONDOWN; vk = 0x02; break;
        case SDL_BUTTON_MIDDLE: base = WM_MBUTTONDOWN; vk = 0x04; break;
        default: return;
      }
      SetKey(vk, down);
      bool dbl = false;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(h);
        dbl = down && w && (w->class_style & CS_DBLCLKS) && e.button.clicks == 2;
      }
      uint32_t msg = !down ? base + 1 : dbl ? base + 2 : base;
      Post(h, msg, MouseKeys(), MkLparam(x, y));
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      uint32_t h = HwndForSdl(e.wheel.windowID);
      if (!h) break;
      int delta = int(e.wheel.y * 120.0f);
      if (!delta) break;
      Post(h, WM_MOUSEWHEEL, MouseKeys() | (uint32_t(uint16_t(int16_t(delta))) << 16),
           MkLparam(g_mouse_x, g_mouse_y));
      break;
    }
    default:
      break;
  }
}

// Writes a guest MSG.
void PutMsg(uint32_t va, const Msg& m) {
  Wr32(va + 0, m.hwnd);
  Wr32(va + 4, m.msg);
  Wr32(va + 8, m.wparam);
  Wr32(va + 12, m.lparam);
  Wr32(va + 16, m.time);
  Wr32(va + 20, uint32_t(m.ptx));
  Wr32(va + 24, uint32_t(m.pty));
}

bool Matches(const Msg& m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
  if (hwnd && m.hwnd != hwnd) return false;
  if (lo == 0 && hi == 0) return true;
  return m.msg >= lo && m.msg <= hi;
}

// Takes (or peeks) the first matching message for the current thread.
bool TakeMessage(uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove, Msg* out) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Queue& q = g_queues[CurrentThread()->tid];
  for (auto it = q.msgs.begin(); it != q.msgs.end(); ++it)
    if (Matches(*it, hwnd, lo, hi)) {
      *out = *it;
      if (remove) q.msgs.erase(it);
      return true;
    }
  if (q.quit) {
    *out = Msg{0, WM_QUIT, q.quit_code, 0, TickCount(), g_mouse_x, g_mouse_y};
    if (remove) q.quit = false;
    return true;
  }
  const uint32_t now = TickCount();
  for (Timer& t : g_timers) {
    Msg m{t.hwnd, WM_TIMER, t.id, t.proc, now, g_mouse_x, g_mouse_y};
    if (t.tid != CurrentThread()->tid || int64_t(now) < t.due || !Matches(m, hwnd, lo, hi))
      continue;
    *out = m;
    if (remove) t.due = int64_t(now) + t.elapse;
    return true;
  }
  return false;
}

bool OnMainThread() { return std::this_thread::get_id() == g_main_thread_id; }

}  // namespace

void EnsureSdlVideo() {
  if (g_sdl_up) return;
  if (!OnMainThread()) FatalF("SDL video requested off the main thread");
  if (Background()) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  // Leave SIGINT/SIGTERM alone: SDL would turn them into quit events the
  // game may ignore, making the process hard to stop.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_VIDEO)) FatalF("SDL_Init: %s", SDL_GetError());
  g_sdl_up = true;
  if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
    g_display_w = m->w;
    g_display_h = m->h;
  }
}

namespace {

// Test hook: <ID>_TEST_FOCUS="lost,gained" (seconds since the first pump)
// injects window focus loss and regain, to exercise the game's
// deactivation path without touching the desktop.
void InjectTestFocus() {
  static int stage = -1;
  static double lost = 0, gained = 0;
  static Uint64 t0 = 0;
  if (stage == -1) {
    stage = 2;
    const char* env = Env("TEST_FOCUS");
    if (env && std::sscanf(env, "%lf,%lf", &lost, &gained) == 2) stage = 0;
    t0 = SDL_GetTicks();
  }
  if (stage >= 2) return;
  double t = (SDL_GetTicks() - t0) / 1000.0;
  if (t < (stage == 0 ? lost : gained)) return;
  uint32_t hwnd = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) hwnd = h;
  }
  if (!hwnd) return;
  WarnF("TEST_FOCUS: focus %s at %.1f s\n", stage == 0 ? "lost" : "gained", t);
  if (stage == 0) {
    for (int vk = 0; vk < 256; vk++) g_key_down[vk] = 0;
    ActivateApp(hwnd, false);
  } else {
    ActivateApp(hwnd, true);
  }
  stage++;
}

// Test hook: <ID>_TEST_INPUT=<file> replays scripted input, one command
// per line, times in seconds since the first event pump:
//   <t> goto <x> <y>      move the game's cursor to client (x, y): it is
//                         driven by relative motion, so this homes it to
//                         the top-left corner first, over a few frames
//   <t> move <dx> <dy>    relative motion
//   <t> pointer <x> <y>   the host pointer is at client (x, y) (the
//                         game's cursor should follow it exactly)
//   <t> click [left|right]
//   <t> key <SDL scancode name>   e.g. Escape, Return, Space, A
// '#' starts a comment.
struct TestEvent {
  double t;
  SDL_Event e;
};

void InjectTestInput() {
  static std::vector<TestEvent> q;
  static size_t next = 0;
  static Uint64 t0 = 0;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    t0 = SDL_GetTicks();
    const char* path = Env("TEST_INPUT");
    if (!path) return;
    FILE* f = std::fopen(path, "r");
    if (!f) {
      WarnF("TEST_INPUT: cannot open %s\n", path);
      return;
    }
    char line[256];
    auto motion = [](double t, float dx, float dy) {
      SDL_Event e{};
      e.type = SDL_EVENT_MOUSE_MOTION;
      e.motion.xrel = dx;
      e.motion.yrel = dy;
      return TestEvent{t, e};
    };
    while (std::fgets(line, sizeof line, f)) {
      if (char* h = std::strchr(line, '#')) *h = 0;
      double t;
      char cmd[32] = {}, a[64] = {}, b[64] = {};
      int n = std::sscanf(line, "%lf %31s %63s %63s", &t, cmd, a, b);
      if (n < 2) continue;
      std::string c = cmd;
      if (c == "goto" && n == 4) {
        // Home: two big moves (the virtual cursor clamps at the edge and
        // the game re-centres it every frame), then steps of <= 200 px.
        q.push_back(motion(t, -4000, -4000));
        q.push_back(motion(t + 0.1, -4000, -4000));
        q.push_back(motion(t + 0.2, -4000, -4000));
        float x = std::atof(a), y = std::atof(b);
        double tt = t + 0.3;
        while (x > 0 || y > 0) {
          float sx = std::min(x, 200.0f), sy = std::min(y, 200.0f);
          q.push_back(motion(tt, sx, sy));
          x -= sx;
          y -= sy;
          tt += 0.1;
        }
      } else if (c == "pointer" && n == 4) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.which = kTestPointer;
        e.motion.x = std::atof(a);
        e.motion.y = std::atof(b);
        q.push_back({t, e});
      } else if (c == "move" && n == 4) {
        q.push_back(motion(t, std::atof(a), std::atof(b)));
      } else if (c == "click") {
        bool right = n >= 3 && std::string(a) == "right";
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        e.button.down = true;
        e.button.clicks = 1;
        q.push_back({t, e});
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.down = false;
        q.push_back({t + 0.08, e});
      } else if (c == "key" && n >= 3) {
        SDL_Scancode sc = SDL_GetScancodeFromName(a);
        if (sc == SDL_SCANCODE_UNKNOWN) {
          WarnF("TEST_INPUT: unknown key %s\n", a);
          continue;
        }
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = true;
        q.push_back({t, e});
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        q.push_back({t + 0.08, e});
      } else {
        WarnF("TEST_INPUT: bad line: %s", line);
      }
    }
    std::fclose(f);
    std::stable_sort(q.begin(), q.end(),
                     [](const TestEvent& x, const TestEvent& y) { return x.t < y.t; });
    WarnF("TEST_INPUT: %zu events from %s\n", q.size(), path);
  }
  if (next >= q.size()) return;
  double t = (SDL_GetTicks() - t0) / 1000.0;
  SDL_WindowID id = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) id = SDL_GetWindowID(w.sdl);
  }
  if (!id) return;
  // At most one motion per pump, so each lands in its own game frame.
  bool moved = false;
  while (next < q.size() && q[next].t <= t) {
    SDL_Event e = q[next].e;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
      if (moved) break;
      moved = true;
      e.motion.windowID = id;
    } else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
      e.key.windowID = id;
    } else {
      e.button.windowID = id;
    }
    HandleSdlEvent(e);
    next++;
  }
}

// Test hook: <ID>_TEST_STALL="at,seconds" blocks the main thread once, to
// exercise the hang watchdog.
void InjectTestStall() {
  static double at = -1, dur = 0;
  static bool done = false;
  static Uint64 t0 = SDL_GetTicks();
  if (done) return;
  if (at < 0) {
    const char* env = Env("TEST_STALL");
    if (!env || std::sscanf(env, "%lf,%lf", &at, &dur) != 2) {
      done = true;
      return;
    }
  }
  if ((SDL_GetTicks() - t0) / 1000.0 < at) return;
  done = true;
  WarnF("TEST_STALL: blocking the main thread for %.1f s\n", dur);
  std::this_thread::sleep_for(std::chrono::duration<double>(dur));
}

}  // namespace

void PumpHostEvents() {
  if (!g_sdl_up || !OnMainThread()) return;
  InjectTestStall();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    // Automated runs take input only from <ID>_TEST_INPUT: their windows
    // are on someone's desktop and stray clicks must not drive them.
    if (Background() && (e.type == SDL_EVENT_MOUSE_MOTION ||
                         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                         e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                         e.type == SDL_EVENT_MOUSE_WHEEL ||
                         e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
                         e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                         e.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                         e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE))
      continue;
    HandleSdlEvent(e);
  }
  InjectTestFocus();
  InjectTestInput();
  WatchdogHeartbeat();
}

uint32_t DcForWindow(uint32_t hwnd) { return hwnd ? 0x40000000u | hwnd : 0x00D00001u; }

uint32_t WindowForDc(uint32_t hdc) { return hdc & 0x40000000u ? hdc & ~0x40000000u : 0; }

SDL_Window* SdlWindowFor(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w ? w->sdl : nullptr;
}

void SetExclusiveMode(uint32_t hwnd, int w, int h) {
  SDL_Window* sdl;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win) return;
    win->mode_w = w;
    win->mode_h = h;
    sdl = win->sdl;
  }
  if (sdl && w) SDL_SetWindowSize(sdl, w, h);
}

namespace hle {
namespace {

using namespace win32;

// ---- classes ---------------------------------------------------------------

// g_classes key of a class name argument: an atom (MAKEINTATOM) or a name.
std::string ClassKey(uint32_t name) {
  return name < 0x10000 ? "#" + std::to_string(name) : Lower(GStr(name));
}

// The system control classes every process has (see "built-in control
// classes" below). Their window procedures are host handlers behind
// thunks, so games can subclass them and chain with CallWindowProc.
void EnsureSystemClasses() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_classes.count("edit")) return;
  for (const char* n : {"static", "edit", "button"}) {
    WndClass k;
    k.name = n;
    k.wndproc = ThunkFor("user32.dll", std::string("<") + n + ">");
    k.atom = g_next_atom++;
    g_classes[n] = k;
  }
}

HLE("user32.dll", RegisterClassA, 1) {
  uint32_t wc = Arg(c, 0);
  WndClass k;
  k.style = Rd32(wc + 0);
  k.wndproc = Rd32(wc + 4);
  k.wnd_extra = Rd32(wc + 12);
  k.name = ClassKey(Rd32(wc + 36));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_classes.count(k.name)) {
    SetLastError(kErrorClassAlreadyExists);
    return 0;
  }
  k.atom = g_next_atom++;
  g_classes[k.name] = k;
  return k.atom;
}

HLE("user32.dll", UnregisterClassA, 2) {
  std::string n = ClassKey(Arg(c, 0));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_classes.erase(n) ? 1 : 0;
}

HLE("user32.dll", SetClassLongA, 3) {
  // GCL_HCURSOR (-12), GCL_HICON (-14)... cosmetic; remember nothing.
  return 0;
}

// ---- windows ---------------------------------------------------------------

HLE("user32.dll", CreateWindowExA, 12) {
  uint32_t exstyle = Arg(c, 0), cls_va = Arg(c, 1), title_va = Arg(c, 2),
           style = Arg(c, 3), parent = Arg(c, 8), menu = Arg(c, 9),
           hinst = Arg(c, 10), param = Arg(c, 11);
  int32_t x = int32_t(Arg(c, 4)), y = int32_t(Arg(c, 5)), w = int32_t(Arg(c, 6)),
          h = int32_t(Arg(c, 7));
  std::string cls = ClassKey(cls_va);
  EnsureSystemClasses();
  Window win;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) {
      WarnF("CreateWindowExA: unknown class %s\n", cls.c_str());
      SetLastError(kErrorCannotFindWndClass);
      return 0;
    }
    win.hwnd = g_next_hwnd;
    g_next_hwnd += 0x10;
    win.cls = cls;
    win.wndproc = it->second.wndproc;
    win.class_style = it->second.style;
    win.extra.assign(it->second.wnd_extra, 0);
  }
  constexpr int32_t kUseDefault = int32_t(0x80000000);
  if (x == kUseDefault) x = y = 0;
  if (w == kUseDefault) {
    w = 640;
    h = 480;
  }
  win.style = style;
  win.exstyle = exstyle;
  win.parent = parent;
  win.id = (style & WS_CHILD) ? menu : 0;
  win.hinst = hinst;
  win.x = x;
  win.y = y;
  win.w = w;
  win.h = h;
  win.title = title_va ? GStr(title_va) : "";
  win.tid = CurrentThread()->tid;
  if (!(style & WS_CHILD)) {
    EnsureSdlVideo();
    int cw, ch;
    ClientSize(win, &cw, &ch);
    win.sdl = SDL_CreateWindow(win.title.c_str(), std::max(cw, 1), std::max(ch, 1),
                               SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (!win.sdl) FatalF("SDL_CreateWindow: %s", SDL_GetError());
  }
  uint32_t hwnd = win.hwnd;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_windows[hwnd] = std::move(win);
  }
  RECOMP_TRACE("  CreateWindowExA(%s, \"%s\", style %08X, %d,%d %dx%d) -> %X\n",
            cls.c_str(), GStr(title_va).c_str(), style, x, y, w, h, hwnd);

  // CREATESTRUCTA in a process-heap block.
  uint32_t cs = HeapAllocProcess(48, true);
  Wr32(cs + 0, param);
  Wr32(cs + 4, hinst);
  Wr32(cs + 8, menu);
  Wr32(cs + 12, parent);
  Wr32(cs + 16, uint32_t(h));
  Wr32(cs + 20, uint32_t(w));
  Wr32(cs + 24, uint32_t(y));
  Wr32(cs + 28, uint32_t(x));
  Wr32(cs + 32, style);
  Wr32(cs + 36, title_va);
  Wr32(cs + 40, cls_va);
  Wr32(cs + 44, exstyle);
  bool ok = Send(hwnd, WM_NCCREATE, 0, cs) != 0;
  if (ok) ok = int32_t(Send(hwnd, WM_CREATE, 0, cs)) != -1;
  HeapFreeProcess(cs);
  if (!ok) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* wp = Win(hwnd)) {
      if (wp->sdl) SDL_DestroyWindow(wp->sdl);
      wp->destroyed = true;
    }
    return 0;
  }
  if (style & WS_VISIBLE) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* wp = Win(hwnd)) {
      wp->visible = true;
      if (wp->sdl) SDL_ShowWindow(wp->sdl);
    }
  }
  return hwnd;
}

HLE("user32.dll", DestroyWindow, 1) {
  uint32_t hwnd = Arg(c, 0);
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return 0;
  }
  Send(hwnd, WM_DESTROY, 0, 0);
  Send(hwnd, WM_NCDESTROY, 0, 0);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    w->sdl = nullptr;
    w->destroyed = true;
  }
  std::erase_if(g_timers, [&](const Timer& t) { return t.hwnd == hwnd; });
  if (g_focus == hwnd) g_focus = 0;
  if (g_active == hwnd) g_active = 0;
  return 1;
}

HLE("user32.dll", IsWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return Win(Arg(c, 0)) ? 1 : 0;
}

HLE("user32.dll", ShowWindow, 2) {
  uint32_t hwnd = Arg(c, 0), cmd = Arg(c, 1);
  bool was, show = cmd != 0;  // SW_HIDE = 0
  // SW_SHOWMINIMIZED, SW_MINIMIZE, SW_SHOWMINNOACTIVE
  bool minimize = cmd == 2 || cmd == 6 || cmd == 7;
  // SW_SHOWNORMAL, SW_SHOWMAXIMIZED, SW_RESTORE, SW_SHOWDEFAULT
  bool restore = cmd == 1 || cmd == 3 || cmd == 9 || cmd == 10;
  // Only these make the window the active one; minimizing hands
  // activation to some other window (and the game minimizes itself from
  // its WM_ACTIVATEAPP(0) handler, so activating here would loop).
  bool activates = restore || cmd == 5;
  SDL_Window* sdl;
  bool exclusive;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    was = w->visible;
    w->visible = show;
    if (minimize) w->minimized = true;
    if (restore) w->minimized = false;
    sdl = w->sdl;
    exclusive = w->mode_w != 0;
  }
  if (sdl) {
    if (!show) {
      SDL_HideWindow(sdl);
    } else {
      SDL_ShowWindow(sdl);
      // Behind an exclusive-mode "screen" the game's minimize on
      // deactivation is not carried out: the window simply stays put
      // while the game idles, and focusing it again resumes the game.
      if (minimize && !exclusive) SDL_MinimizeWindow(sdl);
      if (restore && !exclusive) SDL_RestoreWindow(sdl);
    }
  }
  if (show != was) Send(hwnd, WM_SHOWWINDOW, show ? 1 : 0, 0);
  bool activate = false;
  {
    // A shown top-level window becomes the active app window. Record that
    // before sending: the game's activation handler calls ShowWindow
    // again, which must then see it as already active.
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (activates && sdl && g_active != hwnd) {
      g_active = g_focus = hwnd;
      activate = true;
    }
  }
  if (activate) {
    Send(hwnd, WM_ACTIVATEAPP, 1, 0);
    Send(hwnd, WM_ACTIVATE, 1, 0);
    Send(hwnd, WM_SETFOCUS, 0, 0);
  }
  return was ? 1 : 0;
}

// CloseWindow minimizes (it does not destroy).
HLE("user32.dll", CloseWindow, 1) {
  uint32_t show = ProcAddress(ModuleHandle("user32.dll"), "ShowWindow");
  CallGuest(c, show, {Arg(c, 0), 6 /* SW_MINIMIZE */});
  return 1;
}

HLE("user32.dll", UpdateWindow, 1) { return 1; }
HLE("user32.dll", InvalidateRect, 3) { return 1; }

HLE("user32.dll", IsWindowVisible, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->visible ? 1 : 0;
}

HLE("user32.dll", IsWindowEnabled, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->enabled ? 1 : 0;
}

HLE("user32.dll", EnableWindow, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  if (!w) return 0;
  bool was_disabled = !w->enabled;
  w->enabled = Arg(c, 1) != 0;
  return was_disabled ? 1 : 0;
}

HLE("user32.dll", IsIconic, 1) {
  SDL_Window* s;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    if (w->minimized) return 1;
    s = w->sdl;
  }
  return s && (SDL_GetWindowFlags(s) & SDL_WINDOW_MINIMIZED) ? 1 : 0;
}


// Like Windows, WM_SIZE/WM_MOVE are only sent when the size/position
// actually changes (the game re-applies its rect from its WM_SIZE handler;
// sending unconditionally recurses forever).
void ApplyRect(uint32_t hwnd, int x, int y, int w, int h, bool move, bool size) {
  SDL_Window* sdl = nullptr;
  int cw = 0, ch = 0;
  bool moved = false, sized = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win) return;
    if (move && (win->x != x || win->y != y)) {
      win->x = x;
      win->y = y;
      moved = true;
    }
    if (size && (win->w != w || win->h != h)) {
      win->w = w;
      win->h = h;
      sized = true;
    }
    ClientSize(*win, &cw, &ch);
    // In exclusive mode the host window keeps the display-mode size.
    sdl = win->mode_w ? nullptr : win->sdl;
  }
  if (sdl && sized) SDL_SetWindowSize(sdl, std::max(cw, 1), std::max(ch, 1));
  if (sized) Send(hwnd, WM_SIZE, 0, MkLparam(cw, ch));
  if (moved) Send(hwnd, WM_MOVE, 0, MkLparam(x, y));
}

HLE("user32.dll", SetWindowPos, 7) {
  uint32_t hwnd = Arg(c, 0), flags = Arg(c, 6);
  bool nomove = flags & 0x2, nosize = flags & 0x1;
  ApplyRect(hwnd, int32_t(Arg(c, 2)), int32_t(Arg(c, 3)), int32_t(Arg(c, 4)),
            int32_t(Arg(c, 5)), !nomove, !nosize);
  if (flags & 0x40) {  // SWP_SHOWWINDOW
    if (SDL_Window* s = SdlWindowFor(hwnd)) SDL_ShowWindow(s);
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(hwnd)) w->visible = true;
  }
  return 1;
}

HLE("user32.dll", MoveWindow, 6) {
  ApplyRect(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
            int32_t(Arg(c, 3)), int32_t(Arg(c, 4)), true, true);
  return 1;
}

HLE("user32.dll", AdjustWindowRect, 3) {
  uint32_t rc = Arg(c, 0);
  int border, caption;
  FrameSize(Arg(c, 1), &border, &caption);
  Wr32(rc + 0, Rd32(rc + 0) - border);
  Wr32(rc + 4, Rd32(rc + 4) - border - caption);
  Wr32(rc + 8, Rd32(rc + 8) + border);
  Wr32(rc + 12, Rd32(rc + 12) + border);
  return 1;
}

HLE("user32.dll", GetClientRect, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t rc = Arg(c, 1);
  if (!w) return 0;
  int cw, ch;
  ClientSize(*w, &cw, &ch);
  Wr32(rc, 0);
  Wr32(rc + 4, 0);
  Wr32(rc + 8, uint32_t(cw));
  Wr32(rc + 12, uint32_t(ch));
  return 1;
}

HLE("user32.dll", GetWindowRect, 2) {
  uint32_t hwnd = Arg(c, 0), rc = Arg(c, 1);
  if (hwnd == 0x00010000) {  // desktop
    Wr32(rc, 0);
    Wr32(rc + 4, 0);
    Wr32(rc + 8, uint32_t(g_display_w));
    Wr32(rc + 12, uint32_t(g_display_h));
    return 1;
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return 0;
  Wr32(rc, uint32_t(w->x));
  Wr32(rc + 4, uint32_t(w->y));
  Wr32(rc + 8, uint32_t(w->x + w->w));
  Wr32(rc + 12, uint32_t(w->y + w->h));
  return 1;
}

HLE("user32.dll", ClientToScreen, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) + uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) + uint32_t(oy));
  return 1;
}

HLE("user32.dll", ScreenToClient, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) - uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) - uint32_t(oy));
  return 1;
}

HLE("user32.dll", MapWindowPoints, 4) {
  int fx, fy, tx, ty;
  WindowOrigin(Arg(c, 0), &fx, &fy);
  WindowOrigin(Arg(c, 1), &tx, &ty);
  uint32_t pts = Arg(c, 2), n = Arg(c, 3);
  int dx = fx - tx, dy = fy - ty;
  for (uint32_t i = 0; i < n * 2; i += 2) {
    Wr32(pts + 4 * i, Rd32(pts + 4 * i) + uint32_t(dx));
    Wr32(pts + 4 * i + 4, Rd32(pts + 4 * i + 4) + uint32_t(dy));
  }
  return MkLparam(dx, dy);
}

HLE("user32.dll", GetDesktopWindow, 0) { return 0x00010000; }

HLE("user32.dll", GetParent, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? w->parent : 0;
}

HLE("user32.dll", GetWindow, 2) {
  // GW_OWNER (4) / GW_CHILD (5) / GW_HWNDNEXT (2) ... only owner is used.
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  if (!w) return 0;
  return Arg(c, 1) == 4 ? w->parent : 0;
}

HLE("user32.dll", GetLastActivePopup, 1) { return Arg(c, 0); }

HLE("user32.dll", FindWindowA, 2) {
  std::string cls = Arg(c, 0) ? Lower(GStr(Arg(c, 0))) : "";
  std::string title = Arg(c, 1) ? GStr(Arg(c, 1)) : "";
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || (w.style & WS_CHILD)) continue;
    if (Arg(c, 0) && w.cls != cls) continue;
    if (Arg(c, 1) && w.title != title) continue;
    return h;
  }
  return 0;
}

HLE("user32.dll", SetWindowTextA, 2) {
  std::string t = GStr(Arg(c, 1));
  SDL_Window* s = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    w->title = t;
    s = w->sdl;
  }
  if (s) SDL_SetWindowTitle(s, t.c_str());
  return 1;
}

HLE("user32.dll", GetWindowTextA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t buf = Arg(c, 1), cap = Arg(c, 2);
  if (!w || !cap) return 0;
  std::string t = w->title.substr(0, cap - 1);
  PutStr(buf, t);
  return uint32_t(t.size());
}

HLE("user32.dll", GetWindowTextLengthA, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? uint32_t(w->title.size()) : 0;
}

HLE("user32.dll", GetWindowLongA, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_HINSTANCE: return w->hinst;
    case GWL_HWNDPARENT: return w->parent;
    case GWL_STYLE: return w->style | (w->visible ? WS_VISIBLE : 0);
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_ID: return w->id;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    uint32_t v;
    std::memcpy(&v, w->extra.data() + idx, 4);
    return v;
  }
  return 0;
}

HLE("user32.dll", SetWindowLongA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  uint32_t v = Arg(c, 2), old = 0;
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: old = w->wndproc; w->wndproc = v; return old;
    case GWL_HWNDPARENT: old = w->parent; w->parent = v; return old;
    case GWL_STYLE: old = w->style; w->style = v; return old;
    case GWL_EXSTYLE: old = w->exstyle; w->exstyle = v; return old;
    case GWL_USERDATA: old = w->userdata; w->userdata = v; return old;
    case GWL_ID: old = w->id; w->id = v; return old;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    std::memcpy(&old, w->extra.data() + idx, 4);
    std::memcpy(w->extra.data() + idx, &v, 4);
  }
  return old;
}

// ---- focus and activation ---------------------------------------------------------

HLE("user32.dll", GetFocus, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_focus;
}
HLE("user32.dll", SetFocus, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_focus;
  g_focus = Arg(c, 0);
  return old;
}
HLE("user32.dll", GetActiveWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
HLE("user32.dll", SetActiveWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_active;
  g_active = Arg(c, 0);
  return old;
}
HLE("user32.dll", GetForegroundWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
HLE("user32.dll", SetForegroundWindow, 1) {
  if (SDL_Window* s = SdlWindowFor(Arg(c, 0))) SDL_RaiseWindow(s);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_active = g_focus = Arg(c, 0);
  return 1;
}
HLE("user32.dll", BringWindowToTop, 1) {
  if (SDL_Window* s = SdlWindowFor(Arg(c, 0))) SDL_RaiseWindow(s);
  return 1;
}

// ---- messages ----------------------------------------------------------------------

HLE("user32.dll", PeekMessageA, 5) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3),
           flags = Arg(c, 4);
  PumpHostEvents();
  Msg m;
  if (!TakeMessage(hwnd, lo, hi, flags & 1 /* PM_REMOVE */, &m)) return 0;
  PutMsg(msg_va, m);
  return 1;
}

HLE("user32.dll", GetMessageA, 4) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3);
  Msg m;
  for (;;) {
    PumpHostEvents();
    if (TakeMessage(hwnd, lo, hi, true, &m)) break;
    if (OnMainThread() && g_sdl_up) {
      SDL_WaitEventTimeout(nullptr, 10);
    } else {
      std::unique_lock<std::recursive_mutex> l(g_mu);
      g_queue_cv.wait_for(l, std::chrono::milliseconds(10));
    }
  }
  PutMsg(msg_va, m);
  return m.msg == WM_QUIT ? 0 : 1;
}

HLE("user32.dll", PostMessageA, 4) {
  uint32_t hwnd = Arg(c, 0);
  if (!hwnd) {
    PostTo(CurrentThread()->tid,
           Msg{0, Arg(c, 1), Arg(c, 2), Arg(c, 3), TickCount(), g_mouse_x, g_mouse_y});
    return 1;
  }
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return 0;
  }
  Post(hwnd, Arg(c, 1), Arg(c, 2), Arg(c, 3));
  return 1;
}

HLE("user32.dll", PostQuitMessage, 1) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Queue& q = g_queues[CurrentThread()->tid];
    q.quit = true;
    q.quit_code = Arg(c, 0);
  }
  g_queue_cv.notify_all();
  return 0;
}

HLE("user32.dll", SendMessageA, 4) {
  return Send(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

HLE("user32.dll", DispatchMessageA, 1) {
  uint32_t m = Arg(c, 0);
  uint32_t hwnd = Rd32(m), msg = Rd32(m + 4);
  // WM_TIMER from a timer with a TIMERPROC goes to the procedure.
  if (msg == WM_TIMER && Rd32(m + 12))
    return CallGuest(c, Rd32(m + 12), {hwnd, msg, Rd32(m + 8), Rd32(m + 16)});
  if (!hwnd) return 0;
  return Send(hwnd, msg, Rd32(m + 8), Rd32(m + 12));
}

// ---- hot keys ------------------------------------------------------------------------

HLE("user32.dll", RegisterHotKey, 4) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1), mods = Arg(c, 2), vk = Arg(c, 3);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t tid = CurrentThread()->tid;
  if (hwnd) {
    Window* w = Win(hwnd);
    if (!w) return 0;
    tid = w->tid;
  }
  for (const HotKey& k : g_hotkeys)
    if ((k.hwnd == hwnd && k.id == id) || (k.vk == vk && (k.mods & 7) == (mods & 7))) {
      SetLastError(1409);  // ERROR_HOTKEY_ALREADY_REGISTERED
      return 0;
    }
  g_hotkeys.push_back({hwnd, id, mods, vk, tid});
  return 1;
}

HLE("user32.dll", UnregisterHotKey, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = std::find_if(g_hotkeys.begin(), g_hotkeys.end(), [&](const HotKey& k) {
    return k.hwnd == Arg(c, 0) && k.id == Arg(c, 1);
  });
  if (it == g_hotkeys.end()) {
    SetLastError(1419);  // ERROR_HOTKEY_NOT_REGISTERED
    return 0;
  }
  g_hotkeys.erase(it);
  return 1;
}

// ---- timers ------------------------------------------------------------------------

HLE("user32.dll", SetTimer, 4) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1), elapse = Arg(c, 2), proc = Arg(c, 3);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t tid = CurrentThread()->tid;
  if (hwnd) {
    Window* w = Win(hwnd);
    if (!w) return 0;
    tid = w->tid;
  } else {
    // A thread timer: the system picks the id.
    static uint32_t next_id = 0x7F00;
    id = ++next_id;
  }
  elapse = std::clamp<uint32_t>(elapse, 10, 0x7FFFFFFF);  // USER_TIMER_MINIMUM
  for (Timer& t : g_timers)
    if (t.hwnd == hwnd && t.id == id) {  // same timer: reset it
      t = Timer{hwnd, id, elapse, proc, tid, int64_t(TickCount()) + elapse};
      return hwnd ? 1 : id;
    }
  g_timers.push_back(Timer{hwnd, id, elapse, proc, tid, int64_t(TickCount()) + elapse});
  return hwnd ? 1 : id;
}

HLE("user32.dll", KillTimer, 2) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = std::find_if(g_timers.begin(), g_timers.end(),
                         [&](const Timer& t) { return t.hwnd == hwnd && t.id == id; });
  if (it == g_timers.end()) return 0;
  g_timers.erase(it);
  return 1;
}

HLE("user32.dll", TranslateMessage, 1) {
  // Generate WM_CHAR from WM_KEYDOWN using the US layout and shift/caps.
  uint32_t m = Arg(c, 0);
  uint32_t msg = Rd32(m + 4), vk = Rd32(m + 8);
  if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
  bool shift = g_key_down[0x10], caps = g_key_toggled[0x14];
  bool ctrl = g_key_down[0x11];
  uint32_t ch = 0;
  if (vk >= 'A' && vk <= 'Z') {
    ch = (shift != caps) ? vk : vk + 32;
    if (ctrl) ch = vk - 'A' + 1;
  } else if (vk >= 0x60 && vk <= 0x69 && g_key_toggled[0x90]) {
    ch = '0' + (vk - 0x60);
  } else {
    static const struct { uint8_t vk; char n, s; } k[] = {
        {'0', '0', ')'}, {'1', '1', '!'}, {'2', '2', '@'}, {'3', '3', '#'},
        {'4', '4', '$'}, {'5', '5', '%'}, {'6', '6', '^'}, {'7', '7', '&'},
        {'8', '8', '*'}, {'9', '9', '('}, {0xBD, '-', '_'}, {0xBB, '=', '+'},
        {0xDB, '[', '{'}, {0xDD, ']', '}'}, {0xDC, '\\', '|'},
        {0xBA, ';', ':'}, {0xDE, '\'', '"'}, {0xC0, '`', '~'},
        {0xBC, ',', '<'}, {0xBE, '.', '>'}, {0xBF, '/', '?'},
        {0x20, ' ', ' '}, {0x0D, 13, 13}, {0x08, 8, 8}, {0x09, 9, 9},
        {0x1B, 27, 27}, {0x6A, '*', '*'}, {0x6B, '+', '+'},
        {0x6D, '-', '-'}, {0x6F, '/', '/'}, {0x6E, '.', '.'}};
    for (const auto& e : k)
      if (e.vk == vk) ch = uint8_t(shift ? e.s : e.n);
  }
  if (!ch) return 0;
  Post(Rd32(m), msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, Rd32(m + 12));
  return 1;
}

HLE("user32.dll", TranslateAcceleratorA, 3) { return 0; }
HLE("user32.dll", IsDialogMessageA, 2) { return 0; }

HLE("user32.dll", CallWindowProcA, 5) {
  return CallGuest(c, Arg(c, 0), {Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4)});
}

HLE("user32.dll", DefWindowProcA, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2);
  switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: {
      uint32_t destroy = ProcAddress(ModuleHandle("user32.dll"), "DestroyWindow");
      CallGuest(c, destroy, {hwnd});
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return 1;  // HTCLIENT
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == 0xF060) Post(hwnd, WM_CLOSE, 0, 0);  // SC_CLOSE
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == 0x73 /* VK_F4 */ && g_key_down[0x12]) Post(hwnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_GETTEXTLENGTH: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? uint32_t(w->title.size()) : 0;
    }
    default:
      return 0;
  }
}

// ---- built-in control classes ------------------------------------------------------
//
// "static", "edit" and "button" exist in every Windows process. Nothing is
// drawn; the text (the window title) and state are kept so the game can
// read them back. A read-only multi-line edit is a log the game appends to
// (Quake 3-style console windows): appended text is echoed to stdout,
// which is where that log is visible here.

constexpr uint32_t ES_MULTILINE = 0x0004, ES_READONLY = 0x0800;
constexpr uint32_t EM_GETSEL = 0x00B0, EM_SETSEL = 0x00B1, EM_LINESCROLL = 0x00B6,
                   EM_SCROLLCARET = 0x00B7, EM_GETLINECOUNT = 0x00BA,
                   EM_REPLACESEL = 0x00C2, EM_SETLIMITTEXT = 0x00C5,
                   EM_SETREADONLY = 0x00CF;
constexpr uint32_t BM_GETCHECK = 0x00F0, BM_SETCHECK = 0x00F1, BM_GETSTATE = 0x00F2;
constexpr uint32_t STM_SETICON = 0x0170, STM_GETICON = 0x0171, STM_SETIMAGE = 0x0172,
                   STM_GETIMAGE = 0x0173;

uint32_t CallDefWindowProc(Ctx& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t def = ProcAddress(ModuleHandle("user32.dll"), "DefWindowProcA");
  return CallGuest(c, def, {hwnd, msg, wp, lp});
}

// Messages every control handles the same way; false if `msg` is not one.
bool CommonControlMsg(Window& w, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t* r) {
  switch (msg) {
    case WM_NCCREATE: *r = 1; return true;
    case WM_CREATE: *r = 0; return true;
    case WM_SETTEXT: w.title = lp ? GStr(lp) : ""; *r = 1; return true;
    case WM_GETTEXT: {
      if (!wp) { *r = 0; return true; }
      std::string t = w.title.substr(0, wp - 1);
      PutStr(lp, t);
      *r = uint32_t(t.size());
      return true;
    }
    case WM_GETTEXTLENGTH: *r = uint32_t(w.title.size()); return true;
    case WM_SETFONT: w.font = wp; *r = 0; return true;
    case WM_GETFONT: *r = w.font; return true;
    default: return false;
  }
}

// Writes s to stdout with CRLF and lone CR as plain newlines.
void EchoLog(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') continue;
      out += '\n';
    } else {
      out += s[i];
    }
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
}

// Unknown control messages are reported once each and return 0.
void UnhandledControlMsg(const char* cls, uint32_t msg) {
  static std::set<std::pair<std::string, uint32_t>> seen;
  if (seen.insert({cls, msg}).second)
    WarnF("%s control: message %04X not modelled, returning 0\n", cls, msg);
}

HLE_NAMED("user32.dll", "<static>", StaticWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    switch (msg) {
      case STM_SETIMAGE: case STM_SETICON: r = w->image; w->image = msg == STM_SETICON ? wp : lp; return r;
      case STM_GETIMAGE: case STM_GETICON: return w->image;
    }
    if (msg >= 0x0170 && msg <= 0x017F) {
      UnhandledControlMsg("static", msg);
      return 0;
    }
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

HLE_NAMED("user32.dll", "<button>", ButtonWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    switch (msg) {
      case BM_GETCHECK: return w->check;
      case BM_SETCHECK: w->check = wp; return 0;
      case BM_GETSTATE: return w->check;
    }
    if (msg >= 0x00F0 && msg <= 0x00FF) {
      UnhandledControlMsg("button", msg);
      return 0;
    }
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

HLE_NAMED("user32.dll", "<edit>", EditWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  std::string echo;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (msg == WM_SETTEXT) w->sel_lo = w->sel_hi = 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    const uint32_t len = uint32_t(w->title.size());
    w->sel_lo = std::min(w->sel_lo, len);
    w->sel_hi = std::min(w->sel_hi, len);
    // Replaces the selection with t (cut to the text limit), caret after it.
    auto replace = [&](const std::string& t) {
      uint32_t room = w->limit - std::min(w->limit, len - (w->sel_hi - w->sel_lo));
      std::string ins = t.substr(0, room);
      w->title.replace(w->sel_lo, w->sel_hi - w->sel_lo, ins);
      w->sel_lo = w->sel_hi = w->sel_lo + uint32_t(ins.size());
      return ins;
    };
    switch (msg) {
      case EM_GETSEL:
        if (wp) Wr32(wp, w->sel_lo);
        if (lp) Wr32(lp, w->sel_hi);
        return MkLparam(int(std::min(w->sel_lo, 0xFFFFu)), int(std::min(w->sel_hi, 0xFFFFu)));
      case EM_SETSEL: {
        int32_t a = int32_t(wp), b = int32_t(lp);
        if (a == -1) {  // remove the selection, keep the caret
          w->sel_lo = w->sel_hi;
          return 1;
        }
        uint32_t ua = a < 0 ? len : std::min(uint32_t(a), len);
        uint32_t ub = b < 0 ? len : std::min(uint32_t(b), len);
        w->sel_lo = std::min(ua, ub);
        w->sel_hi = std::max(ua, ub);
        return 1;
      }
      case EM_REPLACESEL: {
        std::string ins = replace(lp ? GStr(lp) : "");
        if ((w->style & (ES_MULTILINE | ES_READONLY)) == (ES_MULTILINE | ES_READONLY))
          echo = ins;
        break;
      }
      case EM_LINESCROLL: return (w->style & ES_MULTILINE) ? 1 : 0;
      case EM_SCROLLCARET: return 1;
      case EM_GETLINECOUNT:
        return 1 + uint32_t(std::count(w->title.begin(), w->title.end(), '\n'));
      case EM_SETLIMITTEXT:
        w->limit = wp ? wp : ((w->style & ES_MULTILINE) ? 0xFFFFFFFFu : 0x7FFFFFFEu);
        return 0;
      case EM_SETREADONLY:
        w->style = wp ? (w->style | ES_READONLY) : (w->style & ~ES_READONLY);
        return 1;
      case WM_CHAR:
        if (w->style & ES_READONLY) return 0;
        if (wp == 8) {  // backspace: the selection, or the character before
          if (w->sel_lo == w->sel_hi && w->sel_lo > 0) w->sel_lo--;
          replace("");
        } else if (wp >= 32 && wp < 256) {
          replace(std::string(1, char(wp)));
        }
        return 0;
      default:
        if (msg >= 0x00B0 && msg <= 0x00DF) {
          UnhandledControlMsg("edit", msg);
          return 0;
        }
        break;
    }
  }
  if (msg == EM_REPLACESEL) {
    if (!echo.empty()) EchoLog(echo);
    return 0;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

HLE("user32.dll", MsgWaitForMultipleObjects, 5) {
  // Wait for objects or for any queued input/message (wake mask ignored).
  uint32_t n = Arg(c, 0), arr = Arg(c, 1), wait_all = Arg(c, 2),
           timeout = Arg(c, 3);
  uint32_t wfm = ProcAddress(ModuleHandle("kernel32.dll"), "WaitForMultipleObjects");
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  for (;;) {
    PumpHostEvents();
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Queue& q = g_queues[CurrentThread()->tid];
      if (!q.msgs.empty() || q.quit) return n;  // WAIT_OBJECT_0 + n
    }
    if (n) {
      uint32_t r = CallGuest(c, wfm, {n, arr, wait_all, 0});
      if (r != 0x102) return r;
    }
    if (timeout != 0xFFFFFFFF && std::chrono::steady_clock::now() >= deadline)
      return 0x102;  // WAIT_TIMEOUT
    if (OnMainThread() && g_sdl_up)
      SDL_WaitEventTimeout(nullptr, 1);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---- hooks ---------------------------------------------------------------------------
// The only hook is WH_KEYBOARD_LL (13), installed to swallow the Windows
// keys. Our input never produces system hotkeys, so the hook gets a handle
// and is never called.

HLE("user32.dll", SetWindowsHookExA, 4) {
  static uint32_t next = 0x00F00010;
  RECOMP_TRACE("  SetWindowsHookExA(type %d, proc %08X): ignored\n",
            int32_t(Arg(c, 0)), Arg(c, 1));
  return next += 0x10;
}
HLE("user32.dll", CallNextHookEx, 4) { return 0; }

// ---- cursor ------------------------------------------------------------------------

HLE("user32.dll", ShowCursor, 1) {
  g_cursor_count += Arg(c, 0) ? 1 : -1;
  if (g_sdl_up && !Background()) {
    if (g_cursor_count >= 0)
      SDL_ShowCursor();
    else
      SDL_HideCursor();
  }
  return uint32_t(g_cursor_count);
}

// Some games re-centre the pointer every frame and move their own cursor
// by trunc((pos - ref) * sensitivity), clamped to the screen, where ref is
// the last position they read or set (Dungeon Siege fullscreen does). Over
// a desktop window that drifts from the host pointer, so while the pointer
// is over the window and the game is re-centring, GetCursorPos returns the
// position that lands the game's cursor exactly under the pointer.
// GameConfig::game_cursor says where that cursor is.
bool SyncedCursorPos(int32_t* sx, int32_t* sy) {
  if (!g_pointer_inside || !Game().game_cursor) return false;
  // Only while the game is re-centring (otherwise it reads absolute
  // positions and the plain pointer position is right).
  if (int64_t(TickCount()) - g_last_recentre_ms > 250) return false;
  uint32_t app = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.mode_w && !w.destroyed) app = w.userdata;
  }
  int32_t gx, gy;
  float kx = 1, ky = 1;
  if (!app || !Game().game_cursor(app, &gx, &gy, &kx, &ky)) return false;
  int32_t dx = int32_t(std::lround((g_real_x - gx) / kx));
  int32_t dy = int32_t(std::lround((g_real_y - gy) / ky));
  *sx = g_recentre_x + dx;
  *sy = g_recentre_y + dy;
  return true;
}

HLE("user32.dll", GetCursorPos, 1) {
  int32_t x = g_mouse_x, y = g_mouse_y;
  SyncedCursorPos(&x, &y);
  Wr32(Arg(c, 0), uint32_t(x));
  Wr32(Arg(c, 0) + 4, uint32_t(y));
  return 1;
}

HLE("user32.dll", SetCursorPos, 2) {
  g_mouse_x = g_recentre_x = int32_t(Arg(c, 0));
  g_mouse_y = g_recentre_y = int32_t(Arg(c, 1));
  g_last_recentre_ms = TickCount();
  return 1;
}

// ClipCursor(RECT const*): NULL frees the cursor. A rectangle inside one
// of our windows makes its cursor virtual and grabs the host pointer.
HLE("user32.dll", ClipCursor, 1) {
  uint32_t rc = Arg(c, 0);
  SDL_Window* release = nullptr;
  SDL_Window* grab = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (g_clip.on && g_clip.hwnd)
      if (Window* w = Win(g_clip.hwnd)) release = w->sdl;
    g_clip = {};
    if (rc) {
      g_clip.on = true;
      g_clip.l = int32_t(Rd32(rc));
      g_clip.t = int32_t(Rd32(rc + 4));
      g_clip.r = int32_t(Rd32(rc + 8));
      g_clip.b = int32_t(Rd32(rc + 12));
      for (auto& [h, w] : g_windows) {
        if (w.destroyed || !w.sdl) continue;
        int ox, oy, cw, ch;
        WindowOrigin(h, &ox, &oy);
        ClientSize(w, &cw, &ch);
        if (g_clip.l >= ox && g_clip.t >= oy && g_clip.r <= ox + cw + 1 &&
            g_clip.b <= oy + ch + 1)
          g_clip.hwnd = h, grab = w.sdl;
      }
      g_mouse_x = std::clamp(g_mouse_x, g_clip.l, std::max(g_clip.l, g_clip.r - 1));
      g_mouse_y = std::clamp(g_mouse_y, g_clip.t, std::max(g_clip.t, g_clip.b - 1));
    }
  }
  if (release && release != grab) SDL_SetWindowRelativeMouseMode(release, false);
  if (grab && !Background()) SDL_SetWindowRelativeMouseMode(grab, true);
  return 1;
}

// Mouse capture: there is one game window, so mouse messages go to it
// either way; the capture window is only remembered.
HLE("user32.dll", SetCapture, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_capture;
  g_capture = Arg(c, 0);
  return old;
}

HLE("user32.dll", ReleaseCapture, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_capture = 0;
  return 1;
}

// ---- display modes -------------------------------------------------------------------

struct Mode {
  int w, h, bpp;
};

std::vector<Mode> Modes() {
  std::vector<Mode> m;
  const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864},
                          {1280, 960}, {1280, 1024}, {1600, 1200}};
  for (int bpp : {16, 32})
    for (const auto& s : sizes)
      if (s[0] <= std::max(g_display_w, 1024) && s[1] <= std::max(g_display_h, 768))
        m.push_back({s[0], s[1], bpp});
  return m;
}

void PutDevMode(uint32_t dm, const Mode& m) {
  std::memset(GPtr(dm), 0, 156);
  PutStr(dm, win32::Upper(Game().id) + " Display");
  Wr16(dm + 32, 0x0401);  // dmSpecVersion
  Wr16(dm + 36, 156);     // dmSize
  Wr32(dm + 40, 0x001C0000 | 0x00400000);  // BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY
  Wr32(dm + 104, uint32_t(m.bpp));
  Wr32(dm + 108, uint32_t(m.w));
  Wr32(dm + 112, uint32_t(m.h));
  Wr32(dm + 120, 60);
}

HLE("user32.dll", EnumDisplaySettingsA, 3) {
  EnsureSdlVideo();
  uint32_t idx = Arg(c, 1), dm = Arg(c, 2);
  if (idx == 0xFFFFFFFF || idx == 0xFFFFFFFE) {  // ENUM_CURRENT/REGISTRY_SETTINGS
    PutDevMode(dm, {g_display_w, g_display_h, 32});
    return 1;
  }
  auto modes = Modes();
  if (idx >= modes.size()) return 0;
  PutDevMode(dm, modes[idx]);
  return 1;
}

// Display modes are virtual: the host's mode never changes. A mode the
// game switches to becomes the screen it sees (GetSystemMetrics,
// EnumDisplaySettings), as after a real switch; its fullscreen window is
// then an SDL window of that size. NULL restores the desktop size.
HLE("user32.dll", ChangeDisplaySettingsA, 2) {
  EnsureSdlVideo();
  static int desktop_w = 0, desktop_h = 0;
  if (!desktop_w) {
    desktop_w = g_display_w;
    desktop_h = g_display_h;
  }
  uint32_t dm = Arg(c, 0), flags = Arg(c, 1);
  if (!dm) {
    g_display_w = desktop_w;
    g_display_h = desktop_h;
    return 0;  // DISP_CHANGE_SUCCESSFUL
  }
  uint32_t fields = Rd32(dm + 40);
  int w = (fields & 0x00080000) ? int(Rd32(dm + 108)) : g_display_w;  // DM_PELSWIDTH
  int h = (fields & 0x00100000) ? int(Rd32(dm + 112)) : g_display_h;  // DM_PELSHEIGHT
  int bpp = (fields & 0x00040000) ? int(Rd32(dm + 104)) : 32;         // DM_BITSPERPEL
  bool known = false;
  for (const Mode& m : Modes()) known |= m.w == w && m.h == h && m.bpp == bpp;
  if (!known) return uint32_t(-2);  // DISP_CHANGE_BADMODE
  if (flags & 2) return 0;          // CDS_TEST
  g_display_w = w;
  g_display_h = h;
  return 0;
}

// ---- painting and GDI device contexts (no GDI drawing reaches the screen;
// the game renders through DirectDraw) -------------------------------------------------

// Device contexts are named after their window (see DcForWindow); the
// screen DC (GetDC(NULL)) is a constant.
HLE("user32.dll", GetDC, 1) { return DcForWindow(Arg(c, 0)); }
HLE("user32.dll", ReleaseDC, 2) { return 1; }
HLE("user32.dll", BeginPaint, 2) {
  uint32_t ps = Arg(c, 1), dc = DcForWindow(Arg(c, 0));
  std::memset(GPtr(ps), 0, 64);
  Wr32(ps, dc);
  return dc;
}
HLE("user32.dll", EndPaint, 2) { return 1; }
HLE("user32.dll", FillRect, 3) { return 1; }
HLE("user32.dll", DrawTextA, 5) { return 16; }
HLE("user32.dll", DrawFrameControl, 4) { return 1; }

// ---- clipboard (not wired to the host yet) --------------------------------------------

HLE("user32.dll", OpenClipboard, 1) { return 0; }
HLE("user32.dll", CloseClipboard, 0) { return 1; }
HLE("user32.dll", EmptyClipboard, 0) { return 0; }
HLE("user32.dll", GetClipboardData, 1) { return 0; }
HLE("user32.dll", SetClipboardData, 2) { return 0; }

}  // namespace
}  // namespace hle
}  // namespace recomp
