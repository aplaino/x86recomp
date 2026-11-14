#include "x86/watchdog.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "x86/runtime.h"

namespace recomp {

thread_local const char* t_wd_hle_name = nullptr;
thread_local uint64_t t_wd_hle_ret = 0;

namespace {

using Clock = std::chrono::steady_clock;
// <ID>_WATCHDOG_SECS overrides (0 disables).
double StallSeconds() {
  static const double s = [] {
    const char* e = Env("WATCHDOG_SECS");
    return e ? std::atof(e) : 5.0;
  }();
  return s;
}

struct HostThread {
  pthread_t th;
  std::string name;
  unsigned tid;
  uint64_t id;  // unique per registration
};

// Registered threads, each removed when it exits (Registration): a thread
// is signalled only under g_mu while it is still listed, so never after
// it has finished.
std::mutex g_mu;
std::vector<HostThread> g_threads;
uint64_t g_next_id = 0;
struct Registration {
  uint64_t id = 0;
  ~Registration() {
    if (!id) return;
    std::lock_guard<std::mutex> l(g_mu);
    std::erase_if(g_threads, [&](const HostThread& t) { return t.id == id; });
  }
};
thread_local Registration t_registration;

std::atomic<int64_t> g_beat{0};  // steady-clock ms of the last heartbeat
std::atomic<int64_t> g_frame{0};  // ... and of the last presented frame
std::atomic<bool> g_active{true};  // the game presents only while active
// One dump request at a time: the thread asked (g_dump_th) claims it
// (requested -> writing) before it writes to g_dump_fd, so a late answer
// to an abandoned request writes nothing.
enum DumpState { kIdle, kRequested, kWriting, kDone };
std::atomic<int> g_dump_state{kIdle};
std::atomic<pthread_t> g_dump_th{};
std::atomic<int> g_dump_fd{-1};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             Clock::now().time_since_epoch())
      .count();
}

void WriteAll(int fd, const char* s) {
  size_t n = std::strlen(s);
  while (n) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) return;
    s += w;
    n -= size_t(w);
  }
}

// Runs on the interrupted thread.
void OnDumpSignal(int) {
  const int saved_errno = errno;
  int requested = kRequested;
  if (pthread_equal(g_dump_th.load(), pthread_self()) &&
      g_dump_state.compare_exchange_strong(requested, kWriting)) {
    const int fd = g_dump_fd.load();
    if (const char* n = t_wd_hle_name) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "in import (last entered): %s, called from guest %08llX\n",
                    n, (unsigned long long)t_wd_hle_ret);
      WriteAll(fd, buf);
    }
    void* frames[512];
    int n = backtrace(frames, 512);
    backtrace_symbols_fd(frames, n, fd);
    g_dump_state.store(kDone);
  }
  errno = saved_errno;
}

void Dump(const char* what, double stalled) {
  std::string path = ReportPath("hang");
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s hang dump: %s for %.1f s\n",
                g_proc->game.id.c_str(), what, stalled);
  WriteAll(fd, buf);
  std::vector<HostThread> threads;
  {
    std::lock_guard<std::mutex> l(g_mu);
    threads = g_threads;
  }
  g_dump_fd.store(fd);
  for (const HostThread& t : threads) {
    std::snprintf(buf, sizeof buf, "\n=== thread %s (guest tid %04X) ===\n",
                  t.name.c_str(), t.tid);
    WriteAll(fd, buf);
    g_dump_th.store(t.th);
    g_dump_state.store(kRequested);
    bool sent;
    {
      std::lock_guard<std::mutex> l(g_mu);
      sent = std::any_of(g_threads.begin(), g_threads.end(),
                         [&](const HostThread& u) { return u.id == t.id; }) &&
             pthread_kill(t.th, SIGUSR2) == 0;
    }
    if (!sent) {
      g_dump_state.store(kIdle);
      WriteAll(fd, "(thread gone)\n");
      continue;
    }
    for (int i = 0; i < 200 && g_dump_state.load() != kDone; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Withdraw the request unless the thread has started writing; then
    // wait for it to finish.
    int requested = kRequested;
    if (g_dump_state.compare_exchange_strong(requested, kIdle)) {
      WriteAll(fd, "(no response)\n");
      continue;
    }
    while (g_dump_state.load() != kDone)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    g_dump_state.store(kIdle);
  }
  g_dump_fd.store(-1);
  close(fd);
  std::fprintf(stderr,
               "warning: watchdog: %s for %.1f s; stacks of all guest threads "
               "written to %s\n",
               what, stalled, path.c_str());
}

void WatchdogMain() {
  int dumps = 0;
  int64_t last_dump = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int64_t beat = g_beat.load(), frame = g_frame.load(), now = NowMs();
    if (!beat || !frame) continue;  // not up yet
    // Either the main thread stopped servicing the window system, or it
    // still does (e.g. in MsgWaitForMultipleObjects) but no frame has
    // been presented: a game-level deadlock behind a responsive window.
    double no_pump = (now - beat) / 1000.0, no_frame = (now - frame) / 1000.0;
    if (!g_active.load()) no_frame = 0;
    double limit = StallSeconds();
    if (limit <= 0 || (no_pump < limit && no_frame < limit)) {
      dumps = 0;
      continue;
    }
    // While it stays stuck, dump again every 15 s (up to 4 times) to tell
    // a wedged thread from one that is busy going nowhere.
    if (dumps >= 4 || (dumps && now - last_dump < 15000)) continue;
    dumps++;
    last_dump = now;
    if (no_pump >= limit)
      Dump("main thread has not pumped window events", no_pump);
    else
      Dump("no frame presented (window events still pumped)", no_frame);
  }
}

}  // namespace

void StartWatchdog() {
  // backtrace() loads libgcc on first use, which must not happen inside
  // the signal handler.
  void* f[2];
  backtrace(f, 2);
  struct sigaction sa{};
  sa.sa_handler = OnDumpSignal;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR2, &sa, nullptr);
  std::thread(WatchdogMain).detach();
}

void RegisterGuestHostThread(const char* name, unsigned tid) {
  std::lock_guard<std::mutex> l(g_mu);
  t_registration.id = ++g_next_id;
  g_threads.push_back({pthread_self(), name, tid, t_registration.id});
}

void WatchdogHeartbeat() { g_beat.store(NowMs()); }
void WatchdogFrame() { g_frame.store(NowMs()); }
void WatchdogAppActive(bool active) {
  g_active.store(active);
  g_frame.store(NowMs());
}

}  // namespace recomp
