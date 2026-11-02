#pragma once
// Counters for tuning threads and synchronisation. <ID>_PERF=1 turns
// them on; every 2 s a "perf:" report goes to stderr. Off, each counting
// point costs a load and a branch.
//   - each guest thread's start: entry, parameter, priority and the host
//     QoS class it runs at (and changes by SetThreadPriority);
//   - polls per second by guest caller (the return address of the
//     import): Sleep(0), Sleep(n > 0), SwitchToThread, and waits with a
//     zero timeout;
//   - every guest thread's x87 control word and MXCSR, sampled every
//     50 ms (the values seen and how often);
//   - ldmxcsr: each place and value loaded, with how often.

#include <cstdint>

namespace recomp::perf {

extern bool g_on;
inline bool On() { return g_on; }

enum class Poll { kSleep0, kSleep, kSwitchToThread, kZeroWait, kCount };
// Counts one poll from guest return address `caller`.
void CountPoll(Poll p, uint32_t caller);

// Logs the calling guest thread's priority and host QoS class; `what`
// says why ("starts at 00401000 (param 00000000)", "SetThreadPriority").
void NoteThread(uint32_t tid, int priority, const char* what);

// Reads <ID>_PERF and, when set, starts the reporting thread.
void Start();

}  // namespace recomp::perf
