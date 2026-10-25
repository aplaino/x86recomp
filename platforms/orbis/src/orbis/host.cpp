#include "orbis/host.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/hle/user32_window.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis {
namespace {

std::thread::id g_host_id = std::this_thread::get_id();  // static init: the main thread
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_tasks;
std::vector<void (*)(const SDL_Event&)> g_sinks;
bool g_video_up = false;

void PumpEvents() {
  if (!g_video_up) return;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    // Closing the window is switching the console off.
    if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      WarnF("host: window closed, exiting\n");
      std::fflush(nullptr);
      std::_Exit(0);
    }
    for (auto* sink : g_sinks) sink(e);
  }
}

}  // namespace

bool OnHostThread() { return std::this_thread::get_id() == g_host_id; }

void PostToHost(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_tasks.push_back(std::move(fn));
  }
  g_cv.notify_one();
}

void RunOnHost(const std::function<void()>& fn) {
  if (OnHostThread()) return fn();
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  PostToHost([&] {
    fn();
    std::lock_guard<std::mutex> l(mu);
    done = true;
    cv.notify_one();
  });
  std::unique_lock<std::mutex> l(mu);
  cv.wait(l, [&] { return done; });
}

void EnsureHostVideo() {
  if (g_video_up) return;
  if (!OnHostThread()) FatalF("SDL video requested off the host thread");
  if (Background()) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // as user32_window.cpp
  if (!SDL_Init(SDL_INIT_VIDEO)) FatalF("SDL_Init: %s", SDL_GetError());
  g_video_up = true;
}

void AddHostEventSink(void (*sink)(const SDL_Event&)) {
  RunOnHost([&] { g_sinks.push_back(sink); });
}

// The host thread's loop (process.cpp): tasks as they come, events every
// few milliseconds.
void RunHostLoop(const std::atomic<bool>& done) {
  while (!done.load(std::memory_order_acquire)) {
    std::deque<std::function<void()>> tasks;
    {
      std::unique_lock<std::mutex> l(g_mu);
      g_cv.wait_for(l, std::chrono::milliseconds(4), [] { return !g_tasks.empty(); });
      tasks.swap(g_tasks);
    }
    for (auto& t : tasks) t();
    PumpEvents();
  }
}

}  // namespace recomp::orbis
