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

// EAX 1.0's effect volume and damping per environment, as the Audigy 2 ZS
// reports them after "EAX environment selection" (read back through
// Miles 6.5e's EAX 1 provider; tools/eax_capture). Several differ from
// the EAX 1.0 SDK's preset table, so the hardware's values are the source.
struct Eax1Preset {
  float volume, damping;
};
const Eax1Preset kEax1Presets[kEnvironments] = {
    {0.5000f, 0.5022f}, {0.2500f, 0.4026f}, {0.4173f, 0.6638f}, {0.6531f, 0.1625f},
    {0.2082f, 0.0967f}, {0.5000f, 0.8880f}, {0.4032f, 0.5038f}, {0.5000f, 0.5032f},
    {0.5000f, 1.3000f}, {0.3614f, 0.3281f}, {0.5000f, 0.2974f}, {0.1531f, 0.5474f},
    {0.3614f, 0.5038f}, {0.4441f, 0.6352f}, {0.2500f, 0.5000f}, {0.1110f, 0.5026f},
    {0.1110f, 0.4965f}, {0.1941f, 0.4957f}, {1.0000f, 0.5022f}, {0.0970f, 0.1712f},
    {0.2082f, 1.5000f}, {0.6524f, 0.2473f}, {1.0000f, 0.8074f}, {0.8760f, 1.3900f},
    {0.1392f, 0.6625f}, {0.4864f, 0.8069f},
};

// How the hardware renders EAX 1.0 (measured against the same presets
// through the EAX 2 provider, which renders them identically otherwise):
//  - every environment's reverb is 3 dB louder (+300 mB on the room);
//  - "EAX effect volume" v sets the room to 2000 log10(v / 0.5) mB from
//    the preset's -1000 (+-6 dB per doubling around generic's 0.5); a
//    selected environment renders as volume 0.5 whatever it reports;
//  - "EAX damping" d sets the decay HF ratio to 0.4 + 0.8 d (fitted to
//    damping 0.2 and 2.0: their HF decay matches EAX 2.0 ratios of about
//    0.56 and 2.0);
//  - "EAX decay time" keeps the reverb's initial level: its energy grows
//    with the decay time (EAX 2.0+ keeps the energy instead).
constexpr int32_t kEax1RoomBoost = 300;
constexpr int32_t kEax1RoomAtHalfVolume = -1000;

// EAX 2.0+ report a source's room level as EAX 1.0's reverb mix (and take
// a mix that way): mix = 10^(room / 3333), 0 at -10000 mB (read back:
// room 0 -> 1, -1000 -> 0.501, -10000 -> 0). EAX 1.0's own mix is taken
// to render through the same relation.
constexpr float kMixMbPerDecade = 10000.0f / 3.0f;

constexpr uint32_t kDecayTimeScale = 0x1, kReflectionsScale = 0x2,
                   kReflectionsDelayScale = 0x4, kReverbScale = 0x8,
                   kReverbDelayScale = 0x10, kDecayHfLimit = 0x20, kEchoTimeScale = 0x40,
                   kModulationTimeScale = 0x80;
constexpr uint32_t kDirectHfAuto = 1, kRoomAuto = 2, kRoomHfAuto = 4;

// Air absorption's HF cut grows faster than the distance beyond the
// minimum distance: measured, it is 0.025 (|air absorption| x (distance -
// min distance))^1.5 mB (-5 mB/m: 5, 15 and 50 mB at 8, 16 and 32 m;
// -100 mB/m: none at 1 m, 150 and 1300 mB at 4 and 16 m; 1 m minimum).
constexpr float kAirScale = 0.025f, kAirPower = 1.5f;
// Beyond the minimum distance the reverb send also falls by 12 mB per
// metre, at all frequencies and whatever the air absorption and room
// rolloff (measured from 4 to 32 m).
constexpr float kRoomDistanceMbPerMetre = -12.0f;

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

int32_t ClampMb(float mb, int32_t lo, int32_t hi) {
  return std::clamp<int32_t>(int32_t(mb), lo, hi);
}

// Occlusion's cut below the HF reference for a path taking `ratio` of it:
// the HF cut times the LF ratio, but never less than what the ratio adds
// on top of the direct path's own LF cut (measured on the room path with
// ratios 0.5 and 1.5).
float OcclusionLf(float occlusion, float ratio, float lf_ratio) {
  return occlusion * std::max(ratio * lf_ratio, ratio + lf_ratio - 1.0f);
}

// "EAX2 environment size" with the *SCALE flags: the size-dependent
// properties follow the size (levels truncated to whole mB, as the
// hardware reports them).
void ScaleSize(Listener* l, float size) {
  if (!(size > 0.0f) || !(l->environment_size > 0.0f) || size == l->environment_size) {
    l->environment_size = size;
    return;
  }
  float scale = size / l->environment_size;
  l->environment_size = size;
  uint32_t f = l->flags;
  if (f & kDecayTimeScale) l->decay_time = std::clamp(l->decay_time * scale, 0.1f, 20.0f);
  if ((f & kReflectionsScale) && (f & kReflectionsDelayScale))
    l->reflections = ClampMb(float(l->reflections) - 2000.0f * std::log10(scale), -10000, 1000);
  if (f & kReflectionsDelayScale)
    l->reflections_delay = std::clamp(l->reflections_delay * scale, 0.0f, 0.3f);
  if (f & kReverbScale) {
    float per_decade = (f & kDecayTimeScale) ? 2000.0f : 3000.0f;
    l->reverb = ClampMb(float(l->reverb) - per_decade * std::log10(scale), -10000, 2000);
  }
  if (f & kReverbDelayScale) l->reverb_delay = std::clamp(l->reverb_delay * scale, 0.0f, 0.1f);
  if (f & kEchoTimeScale) l->echo_time = std::clamp(l->echo_time * scale, 0.075f, 0.25f);
  if (f & kModulationTimeScale)
    l->modulation_time = std::clamp(l->modulation_time * scale, 0.04f, 4.0f);
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
    LF("EAX decay time", kE23, 'f', decay_time),
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
    SF("EAX sample reverb mix", kE1, 'f', reverb_mix),
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
  l.volume = kEax1Presets[environment].volume;
  l.damping = kEax1Presets[environment].damping;
  return l;
}

namespace {

// OpenAL Soft (1.2x) scales its late reverb's input so the energy stays
// the same for a decay time weighted by bandwidth up to 20 kHz: LF ref /
// 20k x the LF decay + (HF ref - LF ref) / 20k x the mid decay + (1 - HF
// ref / 20k) x the HF decay (after its decay HF limit). A lower HF ratio
// thus makes its reverb louder. The hardware's level follows the mid
// decay time only (measured: decay HF ratio 0.2 is 9 dB quieter than
// 0.83, 2.0 is 3 dB louder), so this undoes the weighting.
float LateLevelFix(const ReverbParams& r) {
  float hf_ratio = r.decay_hf_ratio;
  if (r.decay_hf_limit && r.air_absorption_gain_hf < 1.0f)
    hf_ratio = std::min(hf_ratio, -3.0f / (343.3f * r.decay_time *
                                           std::log10(r.air_absorption_gain_hf)));
  auto decay = [&](float ratio) { return std::clamp(r.decay_time * ratio, 0.1f, 20.0f); };
  float lf = r.lf_reference / 20000.0f, hf = r.hf_reference / 20000.0f;
  float weighted = lf * decay(r.decay_lf_ratio) + (hf - lf) * r.decay_time +
                   (1.0f - hf) * decay(hf_ratio);
  return std::sqrt(weighted / r.decay_time);
}

}  // namespace

ReverbParams ToReverb(const Listener& l, int version) {
  ReverbParams r;
  // OpenAL Soft's reverb sizes its delay lines max(5, cbrt(density x
  // 125000)) times an average 1 m room (EFX's presets put an EAX size s
  // at density s^3 / 16, which is 1 for every size over 2.5 m: lines for
  // a 50 m room, which build the late reverb up over about 80 ms). The
  // hardware's late reverb starts at full level after the reverb delay;
  // lines for a room twice the EAX size come closest. Shorter lines cost
  // HF decay time (OpenAL loses some HF per pass through a line, about
  // 0.4 dB at 4 kHz), so they stop at a 15 m room.
  float lines = std::max(l.environment_size * 2.0f, 15.0f);
  r.density = std::min(lines * lines * lines / 125000.0f, 1.0f);
  r.diffusion = l.environment_diffusion;
  // Air absorption also shortens and lowers the hardware's reverb, at mid
  // and low frequencies too (OpenAL's decay HF limit only touches the
  // HF): at -100 mB/m generic decays in 0.53 s instead of 1.43 (relatively
  // longer below 250 Hz) and its reverb is 13 dB down (3 dB at 125 Hz,
  // 22 dB at 4 kHz). Scaled like the direct path's cut, (|air| / 100)^1.5,
  // so the presets' -5 mB/m change little; the level is approximated by
  // -6.5 dB plus -10 dB above the HF reference.
  float air = std::pow(std::max(-l.air_absorption_hf, 0.0f) / 100.0f, 1.5f);
  r.gain = MbToGain(float(l.room + (version == 1 ? kEax1RoomBoost : 0)) - 650.0f * air);
  r.gain_hf = MbToGain(float(l.room_hf) - 1000.0f * air);
  r.gain_lf = MbToGain(float(l.room_lf));
  r.decay_time = l.decay_time * std::pow(0.37f, air);
  r.decay_hf_ratio = l.decay_hf_ratio;
  r.decay_lf_ratio = std::min(l.decay_lf_ratio * std::pow(1.46f, air), 2.0f);
  r.reflections_gain = MbToGain(float(l.reflections));
  r.reflections_delay = l.reflections_delay;
  FlipZ(l.reflections_pan, r.reflections_pan);
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
  r.late_reverb_gain = MbToGain(float(l.reverb)) * LateLevelFix(r);
  return r;
}

void ReadListener1(const uint8_t* p, Listener* l) {
  uint32_t env = U32(p, 0);
  float volume = F32(p, 4), decay = F32(p, 8), damping = F32(p, 12);
  ListenerAttribute(l, "EAX environment selection", 1, &env, true);
  ListenerAttribute(l, "EAX effect volume", 1, &volume, true);
  ListenerAttribute(l, "EAX decay time", 1, &decay, true);
  ListenerAttribute(l, "EAX damping", 1, &damping, true);
}

void WriteListener1(const Listener& l, uint8_t* p) {
  Put(p, 0, l.environment);
  Put(p, 4, l.volume);
  Put(p, 8, l.decay_time);
  Put(p, 12, l.damping);
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
                    float min_distance, int version) {
  // Obstruction cuts the direct path (its LF ratio scales the cut below
  // the HF reference); occlusion cuts both paths, each by its ratio;
  // exclusion cuts only the room path.
  float room_ratio = s.occlusion_room_ratio + (version == 2 ? 1.0f : 0.0f);
  float occ = float(s.occlusion);
  float d_hf = float(s.direct + s.direct_hf + s.obstruction) + occ * s.occlusion_direct_ratio;
  float d_lf = float(s.direct) + float(s.obstruction) * s.obstruction_lf_ratio +
               OcclusionLf(occ, s.occlusion_direct_ratio, s.occlusion_lf_ratio);
  float r_hf = float(s.room + s.room_hf + s.exclusion) + occ * room_ratio;
  float r_lf = float(s.room) + float(s.exclusion) * s.exclusion_lf_ratio +
               OcclusionLf(occ, room_ratio, s.occlusion_lf_ratio);
  // Air absorption: an HF cut growing with the distance.
  float far = std::max(distance - min_distance, 0.0f);
  float air_mb = -l.air_absorption_hf * s.air_absorption_factor * far;
  float air = air_mb > 0 ? -kAirScale * std::pow(air_mb, kAirPower) : 0.0f;
  if (s.flags & kDirectHfAuto) d_hf += air;
  if (s.flags & kRoomHfAuto) r_hf += air;
  r_hf += kRoomDistanceMbPerMetre * far;
  r_lf += kRoomDistanceMbPerMetre * far;
  // Room rolloff: the reverb send falls off with distance like DS3D's
  // inverse law, scaled by the listener's and the source's factors.
  float rolloff = l.room_rolloff_factor + s.room_rolloff_factor;
  if ((s.flags & kRoomAuto) && rolloff > 0 && distance > min_distance && min_distance > 0) {
    float g = min_distance / (min_distance + rolloff * (distance - min_distance));
    float mb = 2000.0f * std::log10(g);
    r_hf += mb;
    r_lf += mb;
  }
  if (s.reverb_mix >= 0) {
    float mb = s.reverb_mix > 0 ? kMixMbPerDecade * std::log10(s.reverb_mix) : -10000.0f;
    r_hf += mb;
    r_lf += mb;
  }
  Levels v;
  v.direct_lf = MbToGain(std::min(d_lf, 0.0f));
  v.direct_hf = MbToGain(std::min(d_hf, 0.0f));
  v.room_lf = MbToGain(std::min(r_lf, 0.0f));
  v.room_hf = MbToGain(std::min(r_hf, 0.0f));
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
  if (version == 1 && (name == "EAX effect volume" || name == "EAX damping" ||
                       name == "EAX decay time")) {
    float* field = name == "EAX effect volume" ? &l->volume
                   : name == "EAX damping"     ? &l->damping
                                               : &l->decay_time;
    if (!set) {
      std::memcpy(value, field, 4);
      return true;
    }
    float v;
    std::memcpy(&v, value, 4);
    if (field == &l->volume) {
      l->volume = std::clamp(v, 0.0f, 1.0f);
      l->room = l->volume > 0 ? ClampMb(kEax1RoomAtHalfVolume +
                                            2000.0f * std::log10(l->volume / 0.5f),
                                        -10000, 0)
                              : -10000;
    } else if (field == &l->damping) {
      l->damping = std::clamp(v, 0.0f, 2.0f);
      l->decay_hf_ratio = std::clamp(0.4f + 0.8f * l->damping, 0.1f, 2.0f);
    } else {
      float decay = std::clamp(v, 0.1f, 20.0f);
      l->reverb = ClampMb(float(l->reverb) + 1000.0f * std::log10(decay / l->decay_time),
                          -10000, 2000);
      l->decay_time = decay;
    }
    return true;
  }
  if (set && name == "EAX2 environment size" && version >= 2) {
    float size;
    std::memcpy(&size, value, 4);
    ScaleSize(l, std::clamp(size, 1.0f, 100.0f));
  } else if (!AccessField(kListenerFields, name, version, l, value, set)) {
    return false;
  }
  if (set && version == 3) l->environment = kEnvironmentUndefined;
  return true;
}

bool SourceAttribute(Source* s, const std::string& name, int version, void* value,
                     bool set) {
  if (name == "EAX sample reverb mix" && version >= 2) {
    // The room level, as a mix.
    float mix;
    if (set) {
      std::memcpy(&mix, value, 4);
      s->room = mix > 0 ? ClampMb(kMixMbPerDecade * std::log10(mix), -10000, 1000) : -10000;
    } else {
      mix = s->room > -10000 ? std::pow(10.0f, float(s->room) / kMixMbPerDecade) : 0.0f;
      std::memcpy(value, &mix, 4);
    }
    return true;
  }
  return AccessField(kSourceFields, name, version, s, value, set);
}

}  // namespace recomp::audio::eax
