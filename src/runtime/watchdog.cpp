#include "runtime/watchdog.h"

#include <cxxabi.h>
#include <dlfcn.h>
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
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/platform.h"
#include "runtime/runtime.h"

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
};

std::mutex g_mu;
std::vector<HostThread> g_threads;
std::atomic<int64_t> g_beat{0};  // steady-clock ms of the last heartbeat
std::atomic<int64_t> g_frame{0};  // ... and of the last presented frame
std::atomic<bool> g_active{true};  // the game presents only while active
std::atomic<int> g_dump_fd{-1};
std::atomic<bool> g_dump_done{false};
// <ID>_HITCH_STACKS: one sample of a thread's stack (OnDumpSignal).
std::atomic<int> g_sample_state{0};  // 1 asked, 2 taken
void* g_sample_frames[96];
int g_sample_n = 0;
std::atomic<pthread_t> g_present_th{};  // the thread that presents frames

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
  if (g_sample_state.load() == 1) {
    g_sample_n = backtrace(g_sample_frames, 96);
    g_sample_state.store(2);
    return;
  }
  int fd = g_dump_fd.load();
  if (fd >= 0) {
    if (const char* n = t_wd_hle_name) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "in import (last entered): %s, called from guest %08llX\n",
                    n, (unsigned long long)t_wd_hle_ret);
      WriteAll(fd, buf);
    }
    void* frames[512];
    int n = backtrace(frames, 512);
    backtrace_symbols_fd(frames, n, fd);
  }
  g_dump_done.store(true);
}

void Dump(const char* what, double stalled) {
  std::string path = ReportPath("hang");
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s hang dump: %s for %.1f s\n",
                Game().id.c_str(), what, stalled);
  WriteAll(fd, buf);
  std::vector<HostThread> threads;
  {
    std::lock_guard<std::mutex> l(g_mu);
    threads = g_threads;
  }
  for (const HostThread& t : threads) {
    std::snprintf(buf, sizeof buf, "\n=== thread %s (guest tid %04X) ===\n",
                  t.name.c_str(), t.tid);
    WriteAll(fd, buf);
    g_dump_fd.store(fd);
    g_dump_done.store(false);
    if (pthread_kill(t.th, SIGUSR2) != 0) {
      WriteAll(fd, "(thread gone)\n");
      continue;
    }
    for (int i = 0; i < 200 && !g_dump_done.load(); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!g_dump_done.load()) WriteAll(fd, "(no response)\n");
  }
  g_dump_fd.store(-1);
  close(fd);
  std::fprintf(stderr,
               "warning: watchdog: %s for %.1f s; stacks of all guest threads "
               "written to %s\n",
               what, stalled, path.c_str());
}

// A thread's stack now, as the lifted functions and imports on it (the
// innermost first), "" if it did not answer.
std::string SampleStack(pthread_t th) {
  g_sample_state.store(1);
  if (pthread_kill(th, SIGUSR2) != 0) {
    g_sample_state.store(0);
    return "";
  }
  for (int i = 0; i < 100 && g_sample_state.load() != 2; i++)
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  if (g_sample_state.load() != 2) {
    g_sample_state.store(0);
    return "";
  }
  std::string out;
  int kept = 0;
  for (int i = 2; i < g_sample_n && kept < 12; i++) {
    Dl_info info;
    if (!dladdr(g_sample_frames[i], &info) || !info.dli_sname) continue;
    int st = 0;
    char* dm = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &st);
    std::string n = st == 0 && dm ? dm : info.dli_sname;
    std::free(dm);
    for (size_t p; (p = n.find("(anonymous namespace)::")) != std::string::npos;) n.erase(p, 23);
    if (const size_t p = n.find('('); p != std::string::npos) n.resize(p);
    if (n.rfind("recomp::gen::", 0) == 0) n = n.substr(13);
    else if (n.find("recomp::") == std::string::npos && kept) continue;  // system frames past ours
    else if (n.find("SampleStack") != std::string::npos || n.find("OnDumpSignal") != std::string::npos) continue;
    if (!out.empty()) out += " < ";
    out += n.size() > 60 ? n.substr(0, 60) : n;
    kept++;
  }
  g_sample_state.store(0);
  return out;
}

// <ID>_HITCH_STACKS=ms: while a frame takes longer than that, the main
// thread's and the presenting thread's stacks every 20 ms; logged with
// the frame's time once it is presented.
void HitchMain(double limit_ms) {
  platform::SetThreadQos({platform::ThreadQos::kUtility, 0});
  std::map<std::string, int> samples;
  int64_t frame_seen = 0, last_sample = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int64_t frame = g_frame.load(), now = NowMs();
    if (!frame || !g_active.load()) continue;
    if (frame != frame_seen) {
      if (!samples.empty()) {
        std::string out;
        std::vector<std::pair<int, std::string>> v;
        for (const auto& [k, n] : samples) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 6; i++)
          out += "\n    " + std::to_string(v[i].first) + "x " + v[i].second;
        std::fprintf(stderr, "hitch: a frame of %lld ms:%s\n", (long long)(frame - frame_seen), out.c_str());
        samples.clear();
      }
      frame_seen = frame;
      continue;
    }
    if (now - frame < limit_ms || now - last_sample < 20) continue;
    last_sample = now;
    pthread_t main_th{};
    {
      std::lock_guard<std::mutex> l(g_mu);
      if (g_threads.empty()) continue;
      main_th = g_threads[0].th;
    }
    if (std::string m = SampleStack(main_th); !m.empty()) samples["main: " + m]++;
    const pthread_t pt = g_present_th.load();
    if (pt && !pthread_equal(pt, main_th))
      if (std::string r = SampleStack(pt); !r.empty()) samples["present: " + r]++;
  }
}

void WatchdogMain() {
  platform::SetThreadQos({platform::ThreadQos::kUtility, 0});
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
  if (const char* e = Env("HITCH_STACKS"); e && std::atof(e) > 0)
    std::thread(HitchMain, std::atof(e)).detach();
}

void RegisterGuestHostThread(const char* name, unsigned tid) {
  std::lock_guard<std::mutex> l(g_mu);
  g_threads.push_back({pthread_self(), name, tid});
}

void WatchdogHeartbeat() { g_beat.store(NowMs()); }
void WatchdogFrame() {
  g_frame.store(NowMs());
  g_present_th.store(pthread_self());
}
void WatchdogAppActive(bool active) {
  g_active.store(active);
  g_frame.store(NowMs());
}

}  // namespace recomp
