#pragma once
// The XMA decoder as the kernel's XMA* exports and titles' audio libraries
// see it (ARCHITECTURE.md, "Runtime: the XMA decoder"): an array of
// hardware contexts in physical memory, which the kernel hands out, and
// registers, which guest code reads and writes in guest memory where the
// console maps them, little-endian (byte-reversed loads and stores). The
// decoder takes the commands written to them; it decodes nothing yet.

#include <cstdint>

namespace recomp::xbox360 {

// Register r is the word at kXmaRegisters + 4 * r.
constexpr uint32_t kXmaRegisters = 0x7FEA0000;
// The context array's physical address; the first of each group of ten
// command registers, which take a bit per context (context 32k + n is bit
// n of the group's word k).
constexpr uint32_t kXmaRegContextArray = 0x600, kXmaRegKick = 0x650, kXmaRegLock = 0x690,
                   kXmaRegClear = 0x6A0;
constexpr uint32_t kXmaContexts = 320, kXmaContextSize = 64;

// The decoder at power-on: the context array, its register, the thread
// that takes commands. Once, after the process's TLS layout is known (it
// starts a system thread).
void StartXma();

// A free context, zeroed, by its guest address (in the 4 KiB physical
// view); 0 if all are in use.
uint32_t AllocateXmaContext();
// Zeroes the context at `va` and frees it. False if it is not a context in
// use.
bool ReleaseXmaContext(uint32_t va);

}  // namespace recomp::xbox360
