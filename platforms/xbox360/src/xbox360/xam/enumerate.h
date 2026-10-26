#pragma once
// xam's enumerators (XamCreateEnumeratorHandle, XamEnumerate): an object
// naming the app and message that produce the items and how many each
// call returns, with the app's own state after its 0x18-byte header.
// XamEnumerate sends that message an I/O request {caller, flags |
// 0x80000000, the state's address, buffer, size, items per call, *items
// returned, *overlapped}.

#include <cstdint>

namespace recomp::xbox360 {

// A handle to a new enumerator. `close` runs with the address of its state
// when the last reference goes (xam sends the app its close message
// then). Returns the handle; the state's address in `state`, zeroed.
uint32_t CreateEnumerator(uint32_t app, uint32_t message, uint32_t items, uint32_t state_size,
                          void (*close)(uint32_t state), uint32_t& state);

}  // namespace recomp::xbox360
