// libSceRtc HLE: ticks (microseconds since 0001-01-01 on the proleptic
// Gregorian calendar), SceRtcDateTime ({u16 year, month, day, hour,
// minute, second; u32 microsecond}), and the RFC 3339 / 2822 parsers and
// formatters, as libSceRtc 11.00 has them: its checks in its order, its
// codes and its quirks (noted where they are surprising). Some checks
// depend on the game's SDK version, as there. The local time zone is the
// platform's, UTC (kernel_time.cpp); the network clocks read the host's
// clock, as the system clock does. Setting a clock is not implemented.

#include <time.h>

#include <cstring>
#include <string>

#include "orbis/hle.h"
#include "orbis/process.h"
#include "x86/runtime.h"

namespace {

using recomp::gaddr;
using recomp::Rd16;
using recomp::Rd32;
using recomp::Rd64;
using recomp::Wr16;
using recomp::Wr32;
using recomp::Wr64;

constexpr uint32_t kErrNetwork = 0x80B50001, kErrNull = 0x80B50002, kErrRange = 0x80B50003,
                   kErrParse = 0x80B50007, kErrYear = 0x80B50008, kErrMonth = 0x80B50009,
                   kErrDay = 0x80B5000A, kErrHour = 0x80B5000B, kErrMinute = 0x80B5000C,
                   kErrSecond = 0x80B5000D, kErrMicrosecond = 0x80B5000E;
constexpr uint64_t kUnixEpoch = 62135596800000000ull;  // 1970-01-01 as a tick
constexpr uint64_t kWin32Epoch = 50491123200000000ull;  // 1601-01-01
constexpr uint64_t kSecond = 1000000, kMinute = 60 * kSecond, kHour = 60 * kMinute,
                   kDay = 24 * kHour;

// SDK 3.00 changed some checks.
bool NewSdk() { return recomp::orbis::SdkVersion() >= 0x03000000; }

struct DateTime {
  uint16_t year, month, day, hour, minute, second;
  uint32_t microsecond;
};

DateTime ReadDt(gaddr p) {
  return {Rd16(p), Rd16(p + 2), Rd16(p + 4), Rd16(p + 6), Rd16(p + 8), Rd16(p + 10), Rd32(p + 12)};
}
void WriteDt(gaddr p, const DateTime& d) {
  Wr16(p, d.year), Wr16(p + 2, d.month), Wr16(p + 4, d.day), Wr16(p + 6, d.hour);
  Wr16(p + 8, d.minute), Wr16(p + 10, d.second), Wr32(p + 12, d.microsecond);
}

bool Leap(uint32_t y) { return y % 400 == 0 || (y % 100 != 0 && y % 4 == 0); }
int DaysIn(uint32_t y, uint32_t m) {
  static const int kDays[2][12] = {{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
                                   {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
  return kDays[Leap(y)][m - 1];
}

// sceRtcCheckValid's checks after the null one.
uint32_t Check(const DateTime& d) {
  if (uint16_t(d.year - 1) > 9998) return kErrYear;
  if (uint16_t(d.month - 1) > 11) return kErrMonth;
  if (d.day == 0 || d.day > DaysIn(d.year, d.month)) return kErrDay;
  if (d.hour > 23) return kErrHour;
  if (d.minute > 59) return kErrMinute;
  if (d.second > 59) return kErrSecond;
  if (d.microsecond >= 1000000) return kErrMicrosecond;
  return 0;
}

// Unchecked: the date's fields as they are.
uint64_t ToTick(const DateTime& d) {
  const uint64_t y = d.year;
  uint64_t days = 365 * y + (y + 3) / 4 - (y + 99) / 100 + (y + 399) / 400 - 366;
  for (uint32_t m = 1; m < d.month && m <= 12; m++) days += DaysIn(d.year, m);
  days += d.day;
  return ((((days * 24 + d.hour) * 60 + d.minute) * 60 + d.second) * kSecond + d.microsecond) -
         kDay;
}

DateTime FromTick(uint64_t t) {
  DateTime d;
  uint64_t days = t / kDay, rem = t % kDay;
  d.microsecond = uint32_t(rem % kSecond);
  const uint64_t sod = rem / kSecond;
  d.second = uint16_t(sod % 60), d.minute = uint16_t(sod / 60 % 60), d.hour = uint16_t(sod / 3600);
  uint64_t y = 1 + days / 146097 * 400;
  days %= 146097;
  // 100-year and 4-year blocks, then single years: the year a day falls in.
  const uint64_t c = std::min<uint64_t>(days / 36524, 3);
  y += c * 100, days -= c * 36524;
  y += days / 1461 * 4, days %= 1461;
  while (days >= (Leap(uint32_t(y)) ? 366u : 365u)) days -= Leap(uint32_t(y)) ? 366 : 365, y++;
  uint32_t m = 1;
  while (days >= uint64_t(DaysIn(uint32_t(y), m))) days -= DaysIn(uint32_t(y), m), m++;
  d.year = uint16_t(y), d.month = uint16_t(m), d.day = uint16_t(days + 1);
  return d;
}

uint64_t NowTick() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return uint64_t(ts.tv_sec) * kSecond + uint64_t(ts.tv_nsec / 1000) + kUnixEpoch;
}

// The local time zone's offset in minutes: the platform's zone is UTC.
constexpr int32_t kLocalMinutes = 0;

// (u64 out, in, delta) checked against wrapping, as the library's
// internal add: nothing written on failure.
uint32_t CheckedAdd(gaddr out, gaddr in, int64_t delta) {
  if (!out || !in) return kErrNull;
  const uint64_t v = Rd64(in);
  if (delta >= 0 ? v + uint64_t(delta) < v : v < uint64_t(-delta)) return kErrRange;
  Wr64(out, v + uint64_t(delta));
  return 0;
}

// ---- parsing

bool Digit(char ch) { return ch >= '0' && ch <= '9'; }

// Exactly n digits at s (advanced), else -1.
int Fixed(const char*& s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (!Digit(s[i])) return -1;
    v = v * 10 + (s[i] - '0');
  }
  s += n;
  return v;
}

// One digit, or two if the next is one too, else -1.
int OneOrTwo(const char*& s) {
  if (!Digit(*s)) return -1;
  int v = *s++ - '0';
  if (Digit(*s)) v = v * 10 + (*s++ - '0');
  return v;
}

// The library's case folding: bytes equal but for bit 5.
bool SameFold(char a, char b) { return ((a ^ b) & 0xDF) == 0; }

bool PrefixFold(const char* s, const char* name, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (!SameFold(s[i], name[i])) return false;
  return true;
}

// The index of the name at s, matched in full or by its first 3
// letters (s advanced past it), else -1.
int MatchName(const char*& s, const char* const* names, int count) {
  for (int i = 0; i < count; i++) {
    const size_t n = std::strlen(names[i]);
    if (PrefixFold(s, names[i], n)) {
      s += n;
      return i;
    }
    if (PrefixFold(s, names[i], 3)) {
      s += 3;
      return i;
    }
  }
  return -1;
}

const char* const kDayNames[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
const char* const kMonthNames[12] = {"January", "February", "March",     "April",
                                     "May",     "June",     "July",      "August",
                                     "September", "October", "November", "December"};

// sceRtcParseRFC3339 on a host copy of the string.
uint32_t ParseRfc3339(gaddr utc, const char* s) {
  DateTime d{};
  int v;
  if ((v = Fixed(s, 4)) < 0 || *s++ != '-') return kErrParse;
  d.year = uint16_t(v);
  if ((v = Fixed(s, 2)) < 0 || *s++ != '-') return kErrParse;
  d.month = uint16_t(v);
  if ((v = Fixed(s, 2)) < 0 || (*s++ | 0x20) != 't') return kErrParse;
  d.day = uint16_t(v);
  if ((v = Fixed(s, 2)) < 0 || *s++ != ':') return kErrParse;
  d.hour = uint16_t(v);
  if ((v = Fixed(s, 2)) < 0 || *s++ != ':') return kErrParse;
  d.minute = uint16_t(v);
  if ((v = Fixed(s, 2)) < 0) return kErrParse;
  d.second = uint16_t(v);
  if (*s == '.') {  // any number of digits, even none; past 6 dropped
    s++;
    uint32_t weight = 100000;
    for (; Digit(*s); s++, weight /= 10) d.microsecond += uint32_t(*s - '0') * weight;
  }
  int offset;
  const char sign = *s++;
  if (sign == 'Z' || sign == 'z') {
    offset = 0;
  } else if (sign == '+' || sign == '-') {
    const int hh = Fixed(s, 2);
    if (hh < 0 || *s++ != ':') return kErrParse;
    const int mm = Fixed(s, 2);
    if (mm < 0) return kErrParse;
    offset = hh * 60 + mm;
  } else {
    return kErrParse;
  }
  const uint32_t r = Check(d);
  // A leap second passes; the tick is then not written (GetTick fails),
  // and the zone's shift applies to what the output held.
  if (r && !(r == kErrSecond && d.second == 60)) return r;
  if (!r) Wr64(utc, ToTick(d));
  Wr64(utc, Rd64(utc) + uint64_t(int64_t(sign == '+' ? -offset : offset)) * kMinute);
  return 0;
}

// sceRtcParseDateTime past its whitespace: asctime's form or RFC 2822's
// (the field values unchecked but RFC 2822's hour; an invalid date leaves
// the output as it was but for the zone's shift, and still returns 0).
uint32_t ParseDateTime(gaddr utc, const char* s) {
  if (Digit(s[0]) && Digit(s[1]) && Digit(s[2]) && Digit(s[3])) return ParseRfc3339(utc, s);
  if (MatchName(s, kDayNames, 7) < 0) return kErrParse;
  if (*s == ',') s++;
  while (*s == ' ' || *s == '\t') s++;
  DateTime d{};
  int offset = 0, v;
  const char* at = s;
  if (int m = MatchName(at, kMonthNames, 12); m >= 0) {
    // Www Mmm d hh:mm:ss yyyy, UTC.
    s = at;
    d.month = uint16_t(m + 1);
    if (*s++ != ' ') return kErrParse;
    if (*s == ' ') {
      if (s[1] > '9') return kErrParse;  // the library checks only this bound
      d.day = uint16_t(s[1] - '0');
      s += 2;
    } else if ((v = OneOrTwo(s)) >= 0) {
      d.day = uint16_t(v);
    } else {
      return kErrParse;
    }
    if (*s++ != ' ' || (v = OneOrTwo(s)) < 0) return kErrParse;
    d.hour = uint16_t(v);
    if (*s++ != ':' || (v = OneOrTwo(s)) < 0) return kErrParse;
    d.minute = uint16_t(v);
    if (*s++ != ':' || (v = OneOrTwo(s)) < 0) return kErrParse;
    d.second = uint16_t(v);
    if (*s++ != ' ' || (v = Fixed(s, 4)) < 0) return kErrParse;
    d.year = uint16_t(v);
  } else {
    // Www[,] d[d] Mmm yy[yy] hh:mm[:ss] [zone]
    if ((v = OneOrTwo(s)) < 0) return kErrParse;
    d.day = uint16_t(v);
    if (*s != '-' && *s != ' ') return kErrParse;
    s++;
    const int mon = MatchName(s, kMonthNames, 12);
    if (mon < 0) return kErrParse;
    d.month = uint16_t(mon + 1);
    if (*s != '-' && *s != ' ') return kErrParse;
    s++;
    if ((v = Fixed(s, 4)) < 0) {
      if ((v = Fixed(s, 2)) < 0) return kErrParse;
      v += v < 50 ? 2000 : 1900;
    }
    d.year = uint16_t(v);
    if (*s++ != ' ' || (v = OneOrTwo(s)) < 0 || v > 25) return kErrParse;
    d.hour = uint16_t(v);
    if (*s++ != ':' || (v = OneOrTwo(s)) < 0) return kErrParse;
    d.minute = uint16_t(v);
    if (*s == ':') {
      s++;
      if ((v = OneOrTwo(s)) < 0) return kErrParse;
      d.second = uint16_t(v);
    } else if (*s == ' ') {
      s++;  // so the zone needs a second space
    } else {
      return kErrParse;
    }
    if (*s == ' ') {
      const char* z = s + 1;
      if (z[0] == '+' || z[0] == '-') {
        const char* p = z + 1;
        const int hh = Fixed(p, 2), mm = hh < 0 ? -1 : Fixed(p, 2);
        if (hh >= 0 && mm >= 0) offset = (z[0] == '-' ? -1 : 1) * (hh * 60 + mm);
      } else if (z[0] == 'U' || z[1] == 'T') {
        offset = 0;
      } else {
        // RFC 2822's zone names (which the library checks first, with
        // the RFC's offsets), then the military letters as the library
        // has them (an hour off RFC 822's). The other names it knows (CET,
        // JST, ...) are not implemented.
        static const struct {
          const char* name;
          int minutes;
        } kRfcZones[] = {{"GMT", 0},    {"EST", -300}, {"EDT", -240}, {"CST", -360}, {"CDT", -300},
                         {"MST", -420}, {"MDT", -360}, {"PST", -480}, {"PDT", -420}};
        bool named = false;
        for (const auto& e : kRfcZones)
          if (PrefixFold(z, e.name, 3)) offset = e.minutes, named = true;
        const char l = char(z[0] & ~0x20), l2 = char(z[1] & ~0x20);
        if (named) {
        } else if (l2 >= 'A' && l2 <= 'Z') {
          recomp::FatalF("sceRtcParseDateTime: time zone %s not implemented", z);
        } else if (l >= 'A' && l <= 'I') {
          offset = (l - 'A') * 60;
        } else if (l >= 'K' && l <= 'M') {
          offset = (l - 'K' + 10) * 60;
        } else if (l >= 'N' && l <= 'Y') {
          offset = -(l - 'N') * 60;
        } else if (l != 'Z') {
          return kErrParse;
        }
      }
    }
  }
  if (!Check(d)) Wr64(utc, ToTick(d));
  Wr64(utc, Rd64(utc) - uint64_t(int64_t(offset)) * kMinute);
  return 0;
}

// ---- formatting

void Put(std::string& out, uint64_t v, int digits) {
  for (int i = digits - 1; i >= 0; i--) {
    uint64_t p = 1;
    for (int k = 0; k < i; k++) p *= 10;
    out += char('0' + v / p % 10);
  }
}

// sceRtcFormatRFC3339Precise.
uint32_t FormatRfc3339(gaddr buf, gaddr utc, int32_t tz, uint32_t digits) {
  if (!buf) return kErrNull;
  const uint64_t t = utc ? Rd64(utc) : NowTick();
  if (tz < -1439 || tz > 1439 || digits > 6) return kErrRange;
  const int64_t delta = int64_t(tz) * int64_t(kMinute);
  if (NewSdk() && (delta >= 0 ? t + uint64_t(delta) < t : t < uint64_t(-delta))) return kErrRange;
  const DateTime d = FromTick(t + uint64_t(delta));
  if (const uint32_t r = Check(d)) return r;
  std::string o;
  Put(o, d.year, 4), o += '-', Put(o, d.month, 2), o += '-', Put(o, d.day, 2), o += 'T';
  Put(o, d.hour, 2), o += ':', Put(o, d.minute, 2), o += ':', Put(o, d.second, 2);
  if (digits) {
    uint32_t p = 1;
    for (uint32_t k = digits; k < 6; k++) p *= 10;
    o += '.', Put(o, d.microsecond / p, int(digits));
  }
  if (tz == 0) {
    o += 'Z';
  } else {
    const int32_t a = tz < 0 ? -tz : tz;
    o += tz < 0 ? '-' : '+', Put(o, a / 60, 2), o += ':', Put(o, a % 60, 2);
  }
  recomp::PutStr(buf, o);
  return 0;
}

// sceRtcFormatRFC2822: "Www, DD Mmm YYYY hh:mm:ss +HHMM".
uint32_t FormatRfc2822(gaddr buf, gaddr utc, int32_t tz) {
  if (!buf) return kErrNull;
  if (tz < -1439 || tz > 1439) return kErrRange;
  const uint64_t t = utc ? Rd64(utc) : NowTick();
  const int64_t delta = int64_t(tz) * int64_t(kMinute);
  if (delta >= 0 ? t + uint64_t(delta) < t : t < uint64_t(-delta)) return kErrRange;
  const uint64_t local = t + uint64_t(delta);
  if (int64_t(local) < 0) return kErrRange;
  const DateTime d = FromTick(local);
  if (const uint32_t r = Check(d)) return r;
  const uint64_t day = local / kDay;  // 0001-01-01 was a Monday
  std::string o = std::string(kDayNames[(day + 1) % 7], 3) + ", ";
  Put(o, d.day, 2), o += ' ', o += std::string(kMonthNames[d.month - 1], 3), o += ' ';
  Put(o, d.year, 4), o += ' ', Put(o, d.hour, 2), o += ':', Put(o, d.minute, 2), o += ':';
  Put(o, d.second, 2);
  const int32_t a = tz < 0 ? -tz : tz;
  o += tz < 0 ? " -" : " +", Put(o, a / 60, 2), Put(o, a % 60, 2);
  recomp::PutStr(buf, o);
  return 0;
}

}  // namespace

using recomp::hle::Arg;

ORBIS_HLE("libSceRtc", sceRtcInit, 0) { return 0; }
ORBIS_HLE("libSceRtc", sceRtcEnd, 0) { return 0; }
ORBIS_HLE("libSceRtc", sceRtcGetTickResolution, 0) { return kSecond; }

// int sceRtcGetCurrentTick(SceRtcTick* tick)
ORBIS_HLE("libSceRtc", sceRtcGetCurrentTick, 1) {
  if (!Arg(c, 0)) return kErrNull;
  Wr64(Arg(c, 0), NowTick());
  return 0;
}
// The network clocks (Raw, Debug and Ad: the system's own variants).
ORBIS_HLE("libSceRtc", sceRtcGetCurrentNetworkTick, 1) {
  if (!Arg(c, 0)) return kErrNull;
  Wr64(Arg(c, 0), NowTick());
  return 0;
}
ORBIS_HLE("libSceRtc", sceRtcGetCurrentRawNetworkTick, 1) {
  if (!Arg(c, 0)) return kErrNull;
  Wr64(Arg(c, 0), NowTick());
  return 0;
}
ORBIS_HLE("libSceRtc", sceRtcGetCurrentDebugNetworkTick, 1) {
  if (!Arg(c, 0)) return kErrNull;
  Wr64(Arg(c, 0), NowTick());
  return 0;
}
ORBIS_HLE("libSceRtc", sceRtcGetCurrentAdNetworkTick, 1) {
  if (!Arg(c, 0)) return kErrNull;
  Wr64(Arg(c, 0), NowTick());
  return 0;
}

// int sceRtcGetCurrentClock(SceRtcDateTime* dt, int tz_minutes)
ORBIS_HLE("libSceRtc", sceRtcGetCurrentClock, 2) {
  if (!Arg(c, 0)) return kErrNull;
  WriteDt(Arg(c, 0), FromTick(NowTick() + uint64_t(int64_t(int32_t(Arg(c, 1)))) * kMinute));
  return 0;
}
ORBIS_HLE("libSceRtc", sceRtcGetCurrentClockLocalTime, 1) {
  if (!Arg(c, 0)) return kErrNull;
  WriteDt(Arg(c, 0), FromTick(NowTick() + uint64_t(int64_t(kLocalMinutes)) * kMinute));
  return 0;
}

// int sceRtcConvertUtcToLocalTime(const SceRtcTick* utc, SceRtcTick* local)
ORBIS_HLE("libSceRtc", sceRtcConvertUtcToLocalTime, 2) {
  if (!Arg(c, 0)) return kErrNull;
  return CheckedAdd(Arg(c, 1), Arg(c, 0), int64_t(kLocalMinutes) * int64_t(kMinute));
}
// int sceRtcConvertLocalTimeToUtc(const SceRtcTick* local, SceRtcTick* utc)
ORBIS_HLE("libSceRtc", sceRtcConvertLocalTimeToUtc, 2) {
  if (!Arg(c, 0)) return kErrNull;
  if (!Arg(c, 1)) return kErrNull;
  Wr64(Arg(c, 1), Rd64(Arg(c, 0)) - uint64_t(int64_t(kLocalMinutes)) * kMinute);
  return 0;
}

ORBIS_HLE("libSceRtc", sceRtcCheckValid, 1) {
  return Arg(c, 0) ? Check(ReadDt(Arg(c, 0))) : kErrNull;
}

// int sceRtcGetTick(const SceRtcDateTime* dt, SceRtcTick* tick)
ORBIS_HLE("libSceRtc", sceRtcGetTick, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  const DateTime d = ReadDt(Arg(c, 0));
  if (const uint32_t r = Check(d)) return r;
  Wr64(Arg(c, 1), ToTick(d));
  return 0;
}
// int sceRtcSetTick(SceRtcDateTime* dt, const SceRtcTick* tick)
ORBIS_HLE("libSceRtc", sceRtcSetTick, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  WriteDt(Arg(c, 0), FromTick(Rd64(Arg(c, 1))));
  return 0;
}

// int sceRtcGetTime_t(const SceRtcDateTime* dt, time_t* t)
ORBIS_HLE("libSceRtc", sceRtcGetTime_t, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  const DateTime d = ReadDt(Arg(c, 0));
  if (const uint32_t r = Check(d)) return r;
  const uint64_t t = ToTick(d);
  if (t < kUnixEpoch) {
    Wr64(Arg(c, 1), 0);
    return kErrYear;
  }
  Wr64(Arg(c, 1), (t - kUnixEpoch) / kSecond);
  return 0;
}
// int sceRtcSetTime_t(SceRtcDateTime* dt, time_t t): before SDK 3.00, t's
// low 32 bits, unsigned.
ORBIS_HLE("libSceRtc", sceRtcSetTime_t, 2) {
  const int64_t t = int64_t(Arg(c, 1));
  if (NewSdk() && t < 0) return kErrRange;
  if (!Arg(c, 0)) return kErrNull;
  const uint64_t secs = NewSdk() ? uint64_t(t) : uint64_t(uint32_t(t));
  WriteDt(Arg(c, 0), FromTick(secs * kSecond + kUnixEpoch));
  return 0;
}

// int sceRtcGetDosTime(const SceRtcDateTime* dt, uint32_t* dos)
ORBIS_HLE("libSceRtc", sceRtcGetDosTime, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  const DateTime d = ReadDt(Arg(c, 0));
  if (const uint32_t r = Check(d)) return r;
  if (d.year < 1980 || d.year > 2107) {
    Wr32(Arg(c, 1), d.year < 1980 ? 0 : 0xFF9FBF7D);  // 2107-12-31 23:59:58
    return kErrYear;
  }
  Wr32(Arg(c, 1), uint32_t(d.year - 1980) << 25 | uint32_t(d.month) << 21 |
                      uint32_t(d.day) << 16 | uint32_t(d.hour) << 11 |
                      uint32_t(d.minute) << 5 | d.second >> 1);
  return 0;
}
// int sceRtcSetDosTime(SceRtcDateTime* dt, uint32_t dos): unchecked.
ORBIS_HLE("libSceRtc", sceRtcSetDosTime, 2) {
  if (!Arg(c, 0)) return kErrNull;
  const uint32_t v = uint32_t(Arg(c, 1));
  WriteDt(Arg(c, 0), {uint16_t((v >> 25) + 1980), uint16_t(v >> 21 & 0xF), uint16_t(v >> 16 & 0x1F),
                      uint16_t(v >> 11 & 0x1F), uint16_t(v >> 5 & 0x3F), uint16_t((v & 0x1F) * 2), 0});
  return 0;
}

// int sceRtcGetWin32FileTime(const SceRtcDateTime* dt, uint64_t* ft)
ORBIS_HLE("libSceRtc", sceRtcGetWin32FileTime, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  const DateTime d = ReadDt(Arg(c, 0));
  if (const uint32_t r = Check(d)) return r;
  const uint64_t t = ToTick(d);
  if (t < kWin32Epoch) {
    Wr64(Arg(c, 1), 0);
    return kErrYear;
  }
  Wr64(Arg(c, 1), (t - kWin32Epoch) * 10);
  return 0;
}
ORBIS_HLE("libSceRtc", sceRtcSetWin32FileTime, 2) {
  if (!Arg(c, 0)) return kErrNull;
  WriteDt(Arg(c, 0), FromTick(Arg(c, 1) / 10 + kWin32Epoch));
  return 0;
}

ORBIS_HLE("libSceRtc", sceRtcIsLeapYear, 1) {
  const int32_t y = int32_t(Arg(c, 0));
  return y <= 0 ? kErrYear : Leap(uint32_t(y));
}
ORBIS_HLE("libSceRtc", sceRtcGetDaysInMonth, 2) {
  const int32_t y = int32_t(Arg(c, 0)), m = int32_t(Arg(c, 1));
  if (y <= 0) return kErrYear;
  if (m < 1 || m > 12) return kErrMonth;
  return DaysIn(uint32_t(y), uint32_t(m));
}
// int sceRtcGetDayOfWeek(int year, int month, int day): 0 Sunday.
ORBIS_HLE("libSceRtc", sceRtcGetDayOfWeek, 3) {
  const int32_t y = int32_t(Arg(c, 0)), m = int32_t(Arg(c, 1)), d = int32_t(Arg(c, 2));
  if (NewSdk()) {
    if (m < 1 || m > 12) return kErrMonth;
    if (y < 1 || y > 9999) return kErrYear;
  } else {
    if (y <= 0) return kErrYear;
    if (m < 1 || m > 12) return kErrMonth;
  }
  if (d <= 0 || d > DaysIn(uint32_t(y), uint32_t(m))) return kErrDay;
  const int64_t yy = m <= 2 ? y - 1 : y, mm = m <= 2 ? m + 12 : m;
  return uint64_t((d + yy + yy / 4 - yy / 100 + yy / 400 + (13 * mm + 8) / 5) % 7);
}

ORBIS_HLE("libSceRtc", sceRtcCompareTick, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  const uint64_t a = Rd64(Arg(c, 0)), b = Rd64(Arg(c, 1));
  return uint64_t(int64_t(a < b ? -1 : a > b ? 1 : 0));
}

// sceRtcTickAdd*(SceRtcTick* out, const SceRtcTick* in, n): unchecked,
// wrapping; hours, days and weeks take an int.
#define RTC_TICK_ADD(name, type, unit)                                       \
  ORBIS_HLE("libSceRtc", name, 3) {                                          \
    if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;                           \
    Wr64(Arg(c, 0), Rd64(Arg(c, 1)) + uint64_t(int64_t(type(Arg(c, 2)))) * (unit)); \
    return 0;                                                                \
  }
RTC_TICK_ADD(sceRtcTickAddTicks, int64_t, 1)
RTC_TICK_ADD(sceRtcTickAddMicroseconds, int64_t, 1)
RTC_TICK_ADD(sceRtcTickAddSeconds, int64_t, kSecond)
RTC_TICK_ADD(sceRtcTickAddMinutes, int64_t, kMinute)
RTC_TICK_ADD(sceRtcTickAddHours, int32_t, kHour)
RTC_TICK_ADD(sceRtcTickAddDays, int32_t, kDay)
RTC_TICK_ADD(sceRtcTickAddWeeks, int32_t, 7 * kDay)

// sceRtcTickAddMonths / Years: the day clamped to the new month; a year
// out of 1-9999 returns 0 with nothing written.
ORBIS_HLE("libSceRtc", sceRtcTickAddMonths, 3) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  DateTime d = FromTick(Rd64(Arg(c, 1)));
  const int32_t total = int32_t(d.year) * 12 + (d.month - 1) + int32_t(Arg(c, 2));
  const int32_t y = total / 12, m = total % 12 + 1;
  if (uint16_t(y) == 0 || m < 1 || m > 12 || uint16_t(uint16_t(y) - 1) > 9998) return 0;
  d.year = uint16_t(y), d.month = uint16_t(m);
  if (d.day > DaysIn(d.year, d.month)) d.day = uint16_t(DaysIn(d.year, d.month));
  Wr64(Arg(c, 0), ToTick(d));
  return 0;
}
ORBIS_HLE("libSceRtc", sceRtcTickAddYears, 3) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  DateTime d = FromTick(Rd64(Arg(c, 1)));
  const uint16_t y = uint16_t(d.year + uint16_t(Arg(c, 2)));
  if (y == 0 || y > 9999) return 0;
  d.year = y;
  if (d.day > DaysIn(d.year, d.month)) d.day = uint16_t(DaysIn(d.year, d.month));
  Wr64(Arg(c, 0), ToTick(d));
  return 0;
}

// int sceRtcParseRFC3339(SceRtcTick* utc, const char* s)
ORBIS_HLE("libSceRtc", sceRtcParseRFC3339, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  return ParseRfc3339(Arg(c, 0), recomp::GStr(Arg(c, 1)).c_str());
}
// int sceRtcParseDateTime(SceRtcTick* utc, const char* s)
ORBIS_HLE("libSceRtc", sceRtcParseDateTime, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrNull;
  const std::string s = recomp::GStr(Arg(c, 1));
  const char* p = s.c_str();
  while (*p == ' ' || *p == '\t') p++;
  return ParseDateTime(Arg(c, 0), p);
}

// Formatting: buf, utc (null: now), then the zone in minutes east and
// (Precise) the fraction's digits; the LocalTime ones in the local zone.
ORBIS_HLE("libSceRtc", sceRtcFormatRFC3339, 3) {
  return FormatRfc3339(Arg(c, 0), Arg(c, 1), int32_t(Arg(c, 2)), 2);
}
ORBIS_HLE("libSceRtc", sceRtcFormatRFC3339Precise, 4) {
  return FormatRfc3339(Arg(c, 0), Arg(c, 1), int32_t(Arg(c, 2)), uint32_t(Arg(c, 3)));
}
ORBIS_HLE("libSceRtc", sceRtcFormatRFC3339LocalTime, 2) {
  return FormatRfc3339(Arg(c, 0), Arg(c, 1), kLocalMinutes, 2);
}
ORBIS_HLE("libSceRtc", sceRtcFormatRFC3339PreciseLocalTime, 3) {
  if (!Arg(c, 0)) return kErrNull;
  if (uint32_t(Arg(c, 2)) > 6) return kErrRange;
  return FormatRfc3339(Arg(c, 0), Arg(c, 1), kLocalMinutes, uint32_t(Arg(c, 2)));
}
ORBIS_HLE("libSceRtc", sceRtcFormatRFC2822, 3) {
  return FormatRfc2822(Arg(c, 0), Arg(c, 1), int32_t(Arg(c, 2)));
}
ORBIS_HLE("libSceRtc", sceRtcFormatRFC2822LocalTime, 2) {
  return FormatRfc2822(Arg(c, 0), Arg(c, 1), kLocalMinutes);
}
