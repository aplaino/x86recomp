#pragma once
// The console's system side, on the process main thread: guest threads
// all run on host threads of their own (RunMainThreadBeside), and the main
// thread brings up SDL video when first asked, owns the window, pumps its
// events and runs what the HLE posts to it (window creation,
// presentation), since SDL (and Cocoa on macOS) take those calls there
// only.

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>

namespace recomp::orbis {

// Runs fn on the host thread and waits for it (inline when already there).
void RunOnHost(const std::function<void()>& fn);
// Queues fn for the host thread without waiting.
void PostToHost(std::function<void()> fn);
bool OnHostThread();

// Ensures SDL video is up (host thread only).
void EnsureHostVideo();
// sink sees every host event the host thread pumps (input HLE).
void AddHostEventSink(void (*sink)(const SDL_Event&));

// The host thread's loop, until done (RunProcess).
void RunHostLoop(const std::atomic<bool>& done);

}  // namespace recomp::orbis
