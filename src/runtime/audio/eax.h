#pragma once
// EAX (Creative's environmental audio extensions, versions 1-3) as Miles'
// EAX providers take it: listener and source property sets in EAX units
// (millibels, seconds, ratios), the EAX 2.0/3.0 struct layouts games pass
// whole, and how they are rendered here: the listener becomes a Reverb
// bus's ReverbParams, a source becomes two-band direct and reverb-send
// levels for its mixer voice.

#include <cstdint>

#include "runtime/audio/reverb.h"

namespace recomp::audio::eax {

// EAX environments (EAX_ENVIRONMENT_GENERIC .. _PSYCHOTIC); also Miles'
// room types.
inline constexpr uint32_t kEnvironments = 26;

// EAX 3.0 listener properties. EAX 2.0's are a subset (no LF, pans, echo,
// modulation or references; those keep their defaults).
struct Listener {
  uint32_t environment = 0;
  float environment_size = 7.5f, environment_diffusion = 1.0f;
  int32_t room = -1000, room_hf = -100, room_lf = 0;
  float decay_time = 1.49f, decay_hf_ratio = 0.83f, decay_lf_ratio = 1.0f;
  int32_t reflections = -2602;
  float reflections_delay = 0.007f;
  float reflections_pan[3] = {};
  int32_t reverb = 200;
  float reverb_delay = 0.011f;
  float reverb_pan[3] = {};
  float echo_time = 0.25f, echo_depth = 0.0f;
  float modulation_time = 0.25f, modulation_depth = 0.0f;
  float air_absorption_hf = -5.0f;  // mB per metre
  float hf_reference = 5000.0f, lf_reference = 250.0f;
  float room_rolloff_factor = 0.0f;
  uint32_t flags = 0x3F;  // the *SCALE flags and DECAYHFLIMIT (0x20)
};

// The listener for an environment (EAX's presets; out-of-range values
// give GENERIC).
Listener Preset(uint32_t environment);

ReverbParams ToReverb(const Listener& l);

// EAXLISTENERPROPERTIES as EAX 2.0 (56 bytes) and EAX 3.0 (112 bytes)
// lay them out, read from / written to guest memory at `p`.
inline constexpr uint32_t kListener2Size = 56, kListener3Size = 112;
void ReadListener2(const uint8_t* p, Listener* l);
void WriteListener2(const Listener& l, uint8_t* p);
void ReadListener3(const uint8_t* p, Listener* l);
void WriteListener3(const Listener& l, uint8_t* p);

// EAX 2.0/3.0 source (buffer) properties, plus EAX 1.0's reverb mix.
struct Source {
  int32_t direct = 0, direct_hf = 0, room = 0, room_hf = 0;
  int32_t obstruction = 0;
  float obstruction_lf_ratio = 0.0f;
  int32_t occlusion = 0;
  float occlusion_lf_ratio = 0.25f, occlusion_room_ratio = 1.5f;
  float occlusion_direct_ratio = 1.0f;
  int32_t exclusion = 0;
  float exclusion_lf_ratio = 1.0f;
  int32_t outside_volume_hf = 0;
  float doppler_factor = 0.0f, rolloff_factor = 0.0f, room_rolloff_factor = 0.0f;
  float air_absorption_factor = 1.0f;
  uint32_t flags = 7;  // DIRECTHFAUTO 1, ROOMAUTO 2, ROOMHFAUTO 4
  float reverb_mix = -1.0f;  // EAX 1.0, 0..1; < 0: the default (full)
};

// EAX 2.0's occlusion room ratio default; EAX 3.0 raised it to 1.5.
inline constexpr float kOcclusionRoomRatio2 = 0.5f;

// EAXBUFFERPROPERTIES as EAX 2.0 (52 bytes) and EAX 3.0 (72 bytes).
inline constexpr uint32_t kSource2Size = 52, kSource3Size = 72;
void ReadSource2(const uint8_t* p, Source* s);
void WriteSource2(const Source& s, uint8_t* p);
void ReadSource3(const uint8_t* p, Source* s);
void WriteSource3(const Source& s, uint8_t* p);

// Linear levels of a source's direct path and its send to the listener's
// reverb, below and above the HF reference.
struct Levels {
  float direct_lf = 1, direct_hf = 1, room_lf = 1, room_hf = 1;
};
// `distance` and `min_distance` in metres (air absorption, room rolloff).
Levels SourceLevels(const Source& s, const Listener& l, float distance,
                    float min_distance);

float MbToGain(float mb);
int32_t GainToMb(float gain);

}  // namespace recomp::audio::eax
