// <ID>_PERF counters (perf.h).

#include "runtime/perf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/runtime.h"
#include "runtime/sse.h"

namespace recomp::perf {

bool g_on = false;

namespace {

// Counts by key without locks: open addressing, keys never removed. A
// full table counts the rest as `lost`.
struct KeyCounts {
  static constexpr uint32_t kSlots = 512;
  std::atomic<uint64_t> key[kSlots] = {};
  std::atomic<uint64_t> n[kSlots] = {};
  std::atomic<uint64_t> lost{0};

  void Add(uint64_t k) {
    uint32_t i = uint32_t((k * 0x9E3779B97F4A7C15ull) >> 55) & (kSlots - 1);
    for (uint32_t probe = 0; probe < 32; probe++, i = (i + 1) & (kSlots - 1)) {
      uint64_t cur = key[i].load(std::memory_order_relaxed);
      if (cur == 0 && key[i].compare_exchange_strong(cur, k, std::memory_order_relaxed)) cur = k;
      if (cur == k) {
        n[i].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    lost.fetch_add(1, std::memory_order_relaxed);
  }

  // The counts since the last call, largest first.
  std::vector<std::pair<uint64_t, uint64_t>> Take() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (uint32_t i = 0; i < kSlots; i++) {
      const uint64_t k = key[i].load(std::memory_order_relaxed);
      if (!k) continue;
      const uint64_t c = n[i].exchange(0, std::memory_order_relaxed);
      if (c) v.push_back({k, c});
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return v;
  }
};

// Keys are the caller plus one: a key of zero marks a free slot.
KeyCounts g_polls[size_t(Poll::kCount)];
KeyCounts g_ldmxcsr;  // (va << 32 | value) + 1

void NoteLdmxcsr(uint32_t v, uint64_t va) { g_ldmxcsr.Add(((va << 32) | v) + 1); }

constexpr double kReportSeconds = 2.0;
constexpr int kSamplesPerReport = 40;  // x87 control word and MXCSR, per thread

void Report(double seconds, std::map<uint32_t, std::map<uint64_t, int>>& fp) {
  static const char* const kNames[] = {"Sleep(0)", "Sleep(n)", "SwitchToThread", "zero-timeout wait"};
  for (size_t p = 0; p < size_t(Poll::kCount); p++) {
    auto v = g_polls[p].Take();
    if (v.empty()) continue;
    uint64_t total = 0;
    for (const auto& [k, n] : v) total += n;
    std::string top;
    for (size_t i = 0; i < v.size() && i < 6; i++) {
      char b[40];
      std::snprintf(b, sizeof b, " %08X:%.0f", uint32_t(v[i].first - 1), double(v[i].second) / seconds);
      top += b;
    }
    WarnF("perf: %s %.0f/s from%s\n", kNames[p], double(total) / seconds, top.c_str());
  }
  {
    auto v = g_ldmxcsr.Take();
    std::string all;
    for (const auto& [k, n] : v) {
      char b[48];
      std::snprintf(b, sizeof b, " %08X=%04X x%llu", uint32_t((k - 1) >> 32), uint32_t(k - 1),
                    (unsigned long long)n);
      all += b;
    }
    if (!all.empty()) WarnF("perf: ldmxcsr%s\n", all.c_str());
  }
  // Per thread: control word / MXCSR pairs seen, as a share of samples.
  std::string line;
  for (auto& [tid, seen] : fp) {
    int total = 0;
    for (const auto& [k, n] : seen) total += n;
    char b[24];
    std::snprintf(b, sizeof b, " %04X:", tid);
    line += b;
    bool first = true;
    for (const auto& [k, n] : seen) {
      std::snprintf(b, sizeof b, "%s%04X/%04X", first ? "" : ",", uint32_t(k >> 32), uint32_t(k));
      line += b;
      if (n != total) {
        std::snprintf(b, sizeof b, "(%d%%)", 100 * n / total);
        line += b;
      }
      first = false;
    }
  }
  fp.clear();
  if (!line.empty()) WarnF("perf: x87 cw/mxcsr by thread%s\n", line.c_str());
}

void ReporterMain() {
  platform::SetThreadPriorityClass(-1);
  std::map<uint32_t, std::map<uint64_t, int>> fp;  // tid -> (cw << 32 | mxcsr) -> samples
  const auto period = std::chrono::duration<double>(kReportSeconds / kSamplesPerReport);
  for (;;) {
    for (int i = 0; i < kSamplesPerReport; i++) {
      std::this_thread::sleep_for(period);
      // Racy reads of words the threads write: a sample, nothing more.
      ForEachThread([&](Thread& t) {
        const uint32_t cw = __atomic_load_n(&t.ctx.fpu.cw, __ATOMIC_RELAXED);
        const uint32_t mx = __atomic_load_n(&t.ctx.mxcsr, __ATOMIC_RELAXED);
        fp[t.tid][(uint64_t(cw) << 32) | mx]++;
      });
    }
    Report(kReportSeconds, fp);
  }
}

}  // namespace

void CountPoll(Poll p, uint32_t caller) { g_polls[size_t(p)].Add(uint64_t(caller) + 1); }

void NoteThread(uint32_t tid, int priority, const char* what) {
  WarnF("perf: thread %04X %s, priority %d, QoS %s\n", tid, what, priority,
        platform::ThreadQosName());
}

void Start() {
  const char* e = Env("PERF");
  if (!e || !*e || *e == '0') return;
  g_on = true;
  sse::g_ldmxcsr_note = NoteLdmxcsr;
  std::thread(ReporterMain).detach();
}

}  // namespace recomp::perf
