#include "xbox360/audio.h"

#include <SDL3/SDL.h>

#include <bit>
#include <chrono>
#include <mutex>
#include <thread>

#include "runtime/log.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

using Clock = std::chrono::steady_clock;

// Frames a client may have queued on the host before its callback waits
// (about 85 ms).
constexpr int kQueuedFrames = 16;
constexpr int kFrameBytes = int(kAudioChannels * kAudioFrameSamples * sizeof(float));

struct Client {
  bool in_use = false;
  uint32_t callback = 0, arg = 0;  // arg: the word the callback is given
  SDL_AudioStream* stream = nullptr;
  Clock::time_point start;
  uint64_t frames = 0;  // callbacks so far, which set the next one's time
};

std::mutex g_lock;
Client g_clients[kAudioClients];
SDL_AudioDeviceID g_device = 0;
bool g_started = false;

// The default playback device, once; 0 if there is none.
void OpenDevice() {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    WarnF("xbox360 audio: SDL audio init failed: %s; frames are dropped\n", SDL_GetError());
    return;
  }
  SDL_AudioSpec spec{SDL_AUDIO_F32, int(kAudioChannels), int(kAudioRate)};
  g_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
  if (!g_device) WarnF("xbox360 audio: no playback device: %s; frames are dropped\n", SDL_GetError());
}

// A frame's time: 256 samples at 48 kHz, 16/3 ms, counted in samples so
// that it does not drift.
Clock::time_point Due(const Client& c) {
  return c.start + std::chrono::nanoseconds(c.frames * kAudioFrameSamples * 1000000000ull / kAudioRate);
}

// Calls each client's callback at its frame's time, unless the host
// already holds kQueuedFrames of its frames.
void Driver(void*) {
  for (;;) {
    int next = -1;
    Clock::time_point due{};
    {
      std::lock_guard<std::mutex> g(g_lock);
      for (uint32_t k = 0; k < kAudioClients; k++)
        if (g_clients[k].in_use && (next < 0 || Due(g_clients[k]) < due)) {
          next = int(k);
          due = Due(g_clients[k]);
        }
    }
    if (next < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    std::this_thread::sleep_until(due);
    uint32_t callback, arg;
    {
      std::lock_guard<std::mutex> g(g_lock);
      Client& c = g_clients[next];
      if (!c.in_use) continue;
      c.frames++;
      // A host stall is not made up beyond a second.
      if (Clock::now() - Due(c) > std::chrono::seconds(1)) {
        c.start = Clock::now();
        c.frames = 0;
      }
      if (c.stream && SDL_GetAudioStreamQueued(c.stream) >= kQueuedFrames * kFrameBytes) continue;
      callback = c.callback;
      arg = c.arg;
    }
    CallGuest(callback, arg, 0);
  }
}

}  // namespace

int RegisterAudioClient(uint32_t callback, uint32_t arg) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_started) {
    OpenDevice();
    CreateSystemThread(Driver, nullptr);
    g_started = true;
  }
  for (uint32_t k = 0; k < kAudioClients; k++) {
    Client& c = g_clients[k];
    if (c.in_use) continue;
    c = Client{};
    c.in_use = true;
    c.callback = callback;
    c.arg = SystemAlloc(4);
    Wr32(c.arg, arg);
    if (g_device) {
      SDL_AudioSpec spec{SDL_AUDIO_F32, int(kAudioChannels), int(kAudioRate)};
      c.stream = SDL_CreateAudioStream(&spec, &spec);
      if (c.stream && !SDL_BindAudioStream(g_device, c.stream)) {
        SDL_DestroyAudioStream(c.stream);
        c.stream = nullptr;
      }
    }
    c.start = Clock::now();
    return int(k);
  }
  return -1;
}

bool UnregisterAudioClient(uint32_t index) {
  std::lock_guard<std::mutex> g(g_lock);
  if (index >= kAudioClients || !g_clients[index].in_use) return false;
  Client& c = g_clients[index];
  if (c.stream) SDL_DestroyAudioStream(c.stream);
  SystemFree(c.arg);
  c = Client{};
  return true;
}

bool SubmitAudioFrame(uint32_t index, uint32_t samples) {
  std::lock_guard<std::mutex> g(g_lock);
  if (index >= kAudioClients || !g_clients[index].in_use) return false;
  SDL_AudioStream* s = g_clients[index].stream;
  if (!s) return true;
  // Interleaved for the host, the floats' bits unchanged.
  float out[kAudioChannels * kAudioFrameSamples];
  for (uint32_t i = 0; i < kAudioFrameSamples; i++)
    for (uint32_t ch = 0; ch < kAudioChannels; ch++)
      out[i * kAudioChannels + ch] = std::bit_cast<float>(Rd32(samples + 4 * (ch * kAudioFrameSamples + i)));
  SDL_PutAudioStreamData(s, out, kFrameBytes);
  return true;
}

}  // namespace recomp::xbox360
