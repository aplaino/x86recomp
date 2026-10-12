#include "runtime/audio/eax.h"

#include <AL/efx-presets.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace recomp::audio::eax {

namespace {

// OpenAL Soft's EFX versions of the EAX environment presets, in
// environment order. EFX keeps density instead of EAX's environment size
// (density = size^3 / 16, capped at 1), so the sizes are listed too.
const EFXEAXREVERBPROPERTIES kPresets[kEnvironments] = {
    EFX_REVERB_PRESET_GENERIC,         EFX_REVERB_PRESET_PADDEDCELL,
    EFX_REVERB_PRESET_ROOM,            EFX_REVERB_PRESET_BATHROOM,
    EFX_REVERB_PRESET_LIVINGROOM,      EFX_REVERB_PRESET_STONEROOM,
    EFX_REVERB_PRESET_AUDITORIUM,      EFX_REVERB_PRESET_CONCERTHALL,
    EFX_REVERB_PRESET_CAVE,            EFX_REVERB_PRESET_ARENA,
    EFX_REVERB_PRESET_HANGAR,          EFX_REVERB_PRESET_CARPETEDHALLWAY,
    EFX_REVERB_PRESET_HALLWAY,         EFX_REVERB_PRESET_STONECORRIDOR,
    EFX_REVERB_PRESET_ALLEY,           EFX_REVERB_PRESET_FOREST,
    EFX_REVERB_PRESET_CITY,            EFX_REVERB_PRESET_MOUNTAINS,
    EFX_REVERB_PRESET_QUARRY,          EFX_REVERB_PRESET_PLAIN,
    EFX_REVERB_PRESET_PARKINGLOT,      EFX_REVERB_PRESET_SEWERPIPE,
    EFX_REVERB_PRESET_UNDERWATER,      EFX_REVERB_PRESET_DRUGGED,
    EFX_REVERB_PRESET_DIZZY,           EFX_REVERB_PRESET_PSYCHOTIC,
};
const float kSizes[kEnvironments] = {
    7.5f, 1.4f, 1.9f, 1.4f, 2.5f, 11.6f, 21.6f, 19.6f, 14.6f, 36.2f,
    50.3f, 1.9f, 1.8f, 13.5f, 7.5f, 38.0f, 7.5f, 100.0f, 17.5f, 42.5f,
    8.3f, 1.7f, 1.8f, 1.9f, 1.8f, 1.0f,
};

constexpr uint32_t kDecayHfLimit = 0x20;
constexpr uint32_t kDirectHfAuto = 1, kRoomAuto = 2, kRoomHfAuto = 4;

int32_t I32(const uint8_t* p, int o) {
  int32_t v;
  std::memcpy(&v, p + o, 4);
  return v;
}
uint32_t U32(const uint8_t* p, int o) { return uint32_t(I32(p, o)); }
float F32(const uint8_t* p, int o) {
  float v;
  std::memcpy(&v, p + o, 4);
  return v;
}
template <typename T>
void Put(uint8_t* p, int o, T v) {
  static_assert(sizeof(T) == 4);
  std::memcpy(p + o, &v, 4);
}

// EAX vectors are left-handed (DirectSound's), EFX's right-handed; the
// conversion is its own inverse.
void FlipZ(const float in[3], float out[3]) {
  out[0] = in[0];
  out[1] = in[1];
  out[2] = -in[2];
}

// Single-property attributes: the EAX versions having each (bit 0 EAX
// 1.0, bit 1 2.0, bit 2 3.0) and the field's type.
constexpr uint8_t kE1 = 1, kE2 = 2, kE3 = 4, kE23 = kE2 | kE3;

struct Field {
  const char* name;
  uint8_t versions;
  char type;  // 'i' S32, 'u' U32, 'f' F32, 'v' 3 x F32
  size_t offset;
};

#define LF(name, v, t, m) {name, v, t, offsetof(Listener, m)}
constexpr Field kListenerFields[] = {
    LF("EAX decay time", kE1 | kE23, 'f', decay_time),
    LF("EAX damping", kE1, 'f', decay_hf_ratio),
    LF("EAX2 room", kE23, 'i', room),
    LF("EAX2 room HF", kE23, 'i', room_hf),
    LF("EAX2 room rolloff", kE23, 'f', room_rolloff_factor),
    LF("EAX2 decay HF ratio", kE23, 'f', decay_hf_ratio),
    LF("EAX2 reflections", kE23, 'i', reflections),
    LF("EAX2 reflections delay", kE23, 'f', reflections_delay),
    LF("EAX2 reverb", kE23, 'i', reverb),
    LF("EAX2 reverb delay", kE23, 'f', reverb_delay),
    LF("EAX2 environment size", kE23, 'f', environment_size),
    LF("EAX2 environment diffusion", kE23, 'f', environment_diffusion),
    LF("EAX2 air absorption", kE23, 'f', air_absorption_hf),
    LF("EAX2 flags", kE23, 'u', flags),
    LF("EAX3 room LF", kE3, 'i', room_lf),
    LF("EAX3 decay LF ratio", kE3, 'f', decay_lf_ratio),
    LF("EAX3 reflections pan", kE3, 'v', reflections_pan),
    LF("EAX3 reverb pan", kE3, 'v', reverb_pan),
    LF("EAX3 echo time", kE3, 'f', echo_time),
    LF("EAX3 echo depth", kE3, 'f', echo_depth),
    LF("EAX3 modulation time", kE3, 'f', modulation_time),
    LF("EAX3 modulation depth", kE3, 'f', modulation_depth),
    LF("EAX3 HF reference", kE3, 'f', hf_reference),
    LF("EAX3 LF reference", kE3, 'f', lf_reference),
};
#undef LF

#define SF(name, v, t, m) {name, v, t, offsetof(Source, m)}
constexpr Field kSourceFields[] = {
    SF("EAX sample reverb mix", kE1 | kE23, 'f', reverb_mix),
    SF("EAX2 sample direct", kE23, 'i', direct),
    SF("EAX2 sample direct HF", kE23, 'i', direct_hf),
    SF("EAX2 sample room", kE23, 'i', room),
    SF("EAX2 sample room HF", kE23, 'i', room_hf),
    SF("EAX2 sample room rolloff", kE23, 'f', room_rolloff_factor),
    SF("EAX2 sample obstruction", kE23, 'i', obstruction),
    SF("EAX2 sample obstruction LF ratio", kE23, 'f', obstruction_lf_ratio),
    SF("EAX2 sample occlusion", kE23, 'i', occlusion),
    SF("EAX2 sample occlusion LF ratio", kE23, 'f', occlusion_lf_ratio),
    SF("EAX2 sample occlusion room ratio", kE23, 'f', occlusion_room_ratio),
    SF("EAX2 sample outside volume HF", kE23, 'i', outside_volume_hf),
    SF("EAX2 sample air absorption", kE23, 'f', air_absorption_factor),
    SF("EAX2 sample flags", kE23, 'u', flags),
    SF("EAX3 sample exclusion", kE3, 'i', exclusion),
    SF("EAX3 sample exclusion LF ratio", kE3, 'f', exclusion_lf_ratio),
    SF("EAX3 sample occlusion direct ratio", kE3, 'f', occlusion_direct_ratio),
    SF("EAX3 sample doppler factor", kE3, 'f', doppler_factor),
};
#undef SF

// Copies field `name` between the property set at `obj` and `value`.
template <typename T, size_t N>
bool AccessField(const Field (&fields)[N], const std::string& name, int version, T* obj,
                 void* value, bool set) {
  for (const Field& f : fields) {
    if (name != f.name || version < 1 || version > 3 || !(f.versions & (1 << (version - 1))))
      continue;
    uint8_t* field = reinterpret_cast<uint8_t*>(obj) + f.offset;
    size_t size = f.type == 'v' ? 12 : 4;
    if (set)
      std::memcpy(field, value, size);
    else
      std::memcpy(value, field, size);
    return true;
  }
  return false;
}

}  // namespace

float MbToGain(float mb) { return std::pow(10.0f, mb / 2000.0f); }

int32_t GainToMb(float gain) {
  if (!(gain > 0.0f)) return -10000;
  return std::clamp<int32_t>(int32_t(std::lround(2000.0f * std::log10(gain))), -10000, 10000);
}

Listener Preset(uint32_t environment) {
  if (environment >= kEnvironments) environment = 0;
  const EFXEAXREVERBPROPERTIES& e = kPresets[environment];
  Listener l;
  l.environment = environment;
  l.environment_size = kSizes[environment];
  l.environment_diffusion = e.flDiffusion;
  l.room = GainToMb(e.flGain);
  l.room_hf = GainToMb(e.flGainHF);
  l.room_lf = GainToMb(e.flGainLF);
  l.decay_time = e.flDecayTime;
  l.decay_hf_ratio = e.flDecayHFRatio;
  l.decay_lf_ratio = e.flDecayLFRatio;
  l.reflections = GainToMb(e.flReflectionsGain);
  l.reflections_delay = e.flReflectionsDelay;
  FlipZ(e.flReflectionsPan, l.reflections_pan);
  l.reverb = GainToMb(e.flLateReverbGain);
  l.reverb_delay = e.flLateReverbDelay;
  FlipZ(e.flLateReverbPan, l.reverb_pan);
  l.echo_time = e.flEchoTime;
  l.echo_depth = e.flEchoDepth;
  l.modulation_time = e.flModulationTime;
  l.modulation_depth = e.flModulationDepth;
  l.air_absorption_hf = 2000.0f * std::log10(e.flAirAbsorptionGainHF);
  l.hf_reference = e.flHFReference;
  l.lf_reference = e.flLFReference;
  l.room_rolloff_factor = e.flRoomRolloffFactor;
  l.flags = (0x3F & ~kDecayHfLimit) | (e.iDecayHFLimit ? kDecayHfLimit : 0);
  return l;
}

ReverbParams ToReverb(const Listener& l) {
  ReverbParams r;
  float size = std::max(l.environment_size, 0.0f);
  r.density = std::min(size * size * size / 16.0f, 1.0f);
  r.diffusion = l.environment_diffusion;
  r.gain = MbToGain(float(l.room));
  r.gain_hf = MbToGain(float(l.room_hf));
  r.gain_lf = MbToGain(float(l.room_lf));
  r.decay_time = l.decay_time;
  r.decay_hf_ratio = l.decay_hf_ratio;
  r.decay_lf_ratio = l.decay_lf_ratio;
  r.reflections_gain = MbToGain(float(l.reflections));
  r.reflections_delay = l.reflections_delay;
  FlipZ(l.reflections_pan, r.reflections_pan);
  r.late_reverb_gain = MbToGain(float(l.reverb));
  r.late_reverb_delay = l.reverb_delay;
  FlipZ(l.reverb_pan, r.late_reverb_pan);
  r.echo_time = l.echo_time;
  r.echo_depth = l.echo_depth;
  r.modulation_time = l.modulation_time;
  r.modulation_depth = l.modulation_depth;
  r.air_absorption_gain_hf = MbToGain(l.air_absorption_hf);
  r.hf_reference = l.hf_reference;
  r.lf_reference = l.lf_reference;
  r.room_rolloff_factor = l.room_rolloff_factor;
  r.decay_hf_limit = (l.flags & kDecayHfLimit) != 0;
  return r;
}

void ReadListener2(const uint8_t* p, Listener* l) {
  l->room = I32(p, 0);
  l->room_hf = I32(p, 4);
  l->room_rolloff_factor = F32(p, 8);
  l->decay_time = F32(p, 12);
  l->decay_hf_ratio = F32(p, 16);
  l->reflections = I32(p, 20);
  l->reflections_delay = F32(p, 24);
  l->reverb = I32(p, 28);
  l->reverb_delay = F32(p, 32);
  l->environment = U32(p, 36);
  l->environment_size = F32(p, 40);
  l->environment_diffusion = F32(p, 44);
  l->air_absorption_hf = F32(p, 48);
  l->flags = U32(p, 52);
}

void WriteListener2(const Listener& l, uint8_t* p) {
  Put(p, 0, l.room);
  Put(p, 4, l.room_hf);
  Put(p, 8, l.room_rolloff_factor);
  Put(p, 12, l.decay_time);
  Put(p, 16, l.decay_hf_ratio);
  Put(p, 20, l.reflections);
  Put(p, 24, l.reflections_delay);
  Put(p, 28, l.reverb);
  Put(p, 32, l.reverb_delay);
  Put(p, 36, l.environment);
  Put(p, 40, l.environment_size);
  Put(p, 44, l.environment_diffusion);
  Put(p, 48, l.air_absorption_hf);
  Put(p, 52, l.flags);
}

void ReadListener3(const uint8_t* p, Listener* l) {
  l->environment = U32(p, 0);
  l->environment_size = F32(p, 4);
  l->environment_diffusion = F32(p, 8);
  l->room = I32(p, 12);
  l->room_hf = I32(p, 16);
  l->room_lf = I32(p, 20);
  l->decay_time = F32(p, 24);
  l->decay_hf_ratio = F32(p, 28);
  l->decay_lf_ratio = F32(p, 32);
  l->reflections = I32(p, 36);
  l->reflections_delay = F32(p, 40);
  for (int i = 0; i < 3; i++) l->reflections_pan[i] = F32(p, 44 + 4 * i);
  l->reverb = I32(p, 56);
  l->reverb_delay = F32(p, 60);
  for (int i = 0; i < 3; i++) l->reverb_pan[i] = F32(p, 64 + 4 * i);
  l->echo_time = F32(p, 76);
  l->echo_depth = F32(p, 80);
  l->modulation_time = F32(p, 84);
  l->modulation_depth = F32(p, 88);
  l->air_absorption_hf = F32(p, 92);
  l->hf_reference = F32(p, 96);
  l->lf_reference = F32(p, 100);
  l->room_rolloff_factor = F32(p, 104);
  l->flags = U32(p, 108);
}

void WriteListener3(const Listener& l, uint8_t* p) {
  Put(p, 0, l.environment);
  Put(p, 4, l.environment_size);
  Put(p, 8, l.environment_diffusion);
  Put(p, 12, l.room);
  Put(p, 16, l.room_hf);
  Put(p, 20, l.room_lf);
  Put(p, 24, l.decay_time);
  Put(p, 28, l.decay_hf_ratio);
  Put(p, 32, l.decay_lf_ratio);
  Put(p, 36, l.reflections);
  Put(p, 40, l.reflections_delay);
  for (int i = 0; i < 3; i++) Put(p, 44 + 4 * i, l.reflections_pan[i]);
  Put(p, 56, l.reverb);
  Put(p, 60, l.reverb_delay);
  for (int i = 0; i < 3; i++) Put(p, 64 + 4 * i, l.reverb_pan[i]);
  Put(p, 76, l.echo_time);
  Put(p, 80, l.echo_depth);
  Put(p, 84, l.modulation_time);
  Put(p, 88, l.modulation_depth);
  Put(p, 92, l.air_absorption_hf);
  Put(p, 96, l.hf_reference);
  Put(p, 100, l.lf_reference);
  Put(p, 104, l.room_rolloff_factor);
  Put(p, 108, l.flags);
}

void ReadSource2(const uint8_t* p, Source* s) {
  s->direct = I32(p, 0);
  s->direct_hf = I32(p, 4);
  s->room = I32(p, 8);
  s->room_hf = I32(p, 12);
  s->room_rolloff_factor = F32(p, 16);
  s->obstruction = I32(p, 20);
  s->obstruction_lf_ratio = F32(p, 24);
  s->occlusion = I32(p, 28);
  s->occlusion_lf_ratio = F32(p, 32);
  s->occlusion_room_ratio = F32(p, 36);
  s->outside_volume_hf = I32(p, 40);
  s->air_absorption_factor = F32(p, 44);
  s->flags = U32(p, 48);
}

void WriteSource2(const Source& s, uint8_t* p) {
  Put(p, 0, s.direct);
  Put(p, 4, s.direct_hf);
  Put(p, 8, s.room);
  Put(p, 12, s.room_hf);
  Put(p, 16, s.room_rolloff_factor);
  Put(p, 20, s.obstruction);
  Put(p, 24, s.obstruction_lf_ratio);
  Put(p, 28, s.occlusion);
  Put(p, 32, s.occlusion_lf_ratio);
  Put(p, 36, s.occlusion_room_ratio);
  Put(p, 40, s.outside_volume_hf);
  Put(p, 44, s.air_absorption_factor);
  Put(p, 48, s.flags);
}

void ReadSource3(const uint8_t* p, Source* s) {
  s->direct = I32(p, 0);
  s->direct_hf = I32(p, 4);
  s->room = I32(p, 8);
  s->room_hf = I32(p, 12);
  s->obstruction = I32(p, 16);
  s->obstruction_lf_ratio = F32(p, 20);
  s->occlusion = I32(p, 24);
  s->occlusion_lf_ratio = F32(p, 28);
  s->occlusion_room_ratio = F32(p, 32);
  s->occlusion_direct_ratio = F32(p, 36);
  s->exclusion = I32(p, 40);
  s->exclusion_lf_ratio = F32(p, 44);
  s->outside_volume_hf = I32(p, 48);
  s->doppler_factor = F32(p, 52);
  s->rolloff_factor = F32(p, 56);
  s->room_rolloff_factor = F32(p, 60);
  s->air_absorption_factor = F32(p, 64);
  s->flags = U32(p, 68);
}

void WriteSource3(const Source& s, uint8_t* p) {
  Put(p, 0, s.direct);
  Put(p, 4, s.direct_hf);
  Put(p, 8, s.room);
  Put(p, 12, s.room_hf);
  Put(p, 16, s.obstruction);
  Put(p, 20, s.obstruction_lf_ratio);
  Put(p, 24, s.occlusion);
  Put(p, 28, s.occlusion_lf_ratio);
  Put(p, 32, s.occlusion_room_ratio);
  Put(p, 36, s.occlusion_direct_ratio);
  Put(p, 40, s.exclusion);
  Put(p, 44, s.exclusion_lf_ratio);
  Put(p, 48, s.outside_volume_hf);
  Put(p, 52, s.doppler_factor);
  Put(p, 56, s.rolloff_factor);
  Put(p, 60, s.room_rolloff_factor);
  Put(p, 64, s.air_absorption_factor);
  Put(p, 68, s.flags);
}

Levels SourceLevels(const Source& s, const Listener& l, float distance,
                    float min_distance) {
  // Obstruction cuts the direct path (its LF ratio scales the cut below
  // the HF reference); occlusion cuts both paths, the room path by its
  // room ratio; exclusion cuts only the room path.
  float occl_direct = float(s.occlusion) * s.occlusion_direct_ratio;
  float occl_room = float(s.occlusion) * s.occlusion_room_ratio;
  float d_hf = float(s.direct + s.direct_hf + s.obstruction) + occl_direct;
  float d_lf = float(s.direct) + float(s.obstruction) * s.obstruction_lf_ratio +
               occl_direct * s.occlusion_lf_ratio;
  float r_hf = float(s.room + s.room_hf + s.exclusion) + occl_room;
  float r_lf = float(s.room) + float(s.exclusion) * s.exclusion_lf_ratio +
               occl_room * s.occlusion_lf_ratio;
  // Air absorption: an HF cut per metre of distance.
  float air = l.air_absorption_hf * s.air_absorption_factor * std::max(distance, 0.0f);
  if (s.flags & kDirectHfAuto) d_hf += air;
  if (s.flags & kRoomHfAuto) r_hf += air;
  // Room rolloff: the reverb send falls off with distance like DS3D's
  // inverse law, scaled by the listener's and the source's factors.
  float rolloff = l.room_rolloff_factor + s.room_rolloff_factor;
  if ((s.flags & kRoomAuto) && rolloff > 0 && distance > min_distance && min_distance > 0) {
    float g = min_distance / (min_distance + rolloff * (distance - min_distance));
    float mb = 2000.0f * std::log10(g);
    r_hf += mb;
    r_lf += mb;
  }
  Levels v;
  v.direct_lf = MbToGain(std::min(d_lf, 0.0f));
  v.direct_hf = MbToGain(std::min(d_hf, 0.0f));
  v.room_lf = MbToGain(std::min(r_lf, 0.0f));
  v.room_hf = MbToGain(std::min(r_hf, 0.0f));
  if (s.reverb_mix >= 0) {
    v.room_lf *= s.reverb_mix;
    v.room_hf *= s.reverb_mix;
  }
  return v;
}

bool ListenerAttribute(Listener* l, const std::string& name, int version, void* value,
                       bool set) {
  if (name == "EAX environment selection") {
    if (set) {
      uint32_t env;
      std::memcpy(&env, value, 4);
      *l = Preset(env);
    } else {
      std::memcpy(value, &l->environment, 4);
    }
    return true;
  }
  if (name == "EAX effect volume" && version == 1) {
    // EAX 1.0's reverb volume (linear) is the room level here.
    float v;
    if (set) {
      std::memcpy(&v, value, 4);
      l->room = GainToMb(v);
    } else {
      v = MbToGain(float(l->room));
      std::memcpy(value, &v, 4);
    }
    return true;
  }
  return AccessField(kListenerFields, name, version, l, value, set);
}

bool SourceAttribute(Source* s, const std::string& name, int version, void* value,
                     bool set) {
  return AccessField(kSourceFields, name, version, s, value, set);
}

}  // namespace recomp::audio::eax
