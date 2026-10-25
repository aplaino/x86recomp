#include "xbox360/format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "xbox360/hle.h"

namespace recomp::xbox360 {
namespace {

void AppendUtf8(std::string& out, char32_t u) {
  if (u < 0x80) {
    out.push_back(char(u));
  } else if (u < 0x800) {
    out.push_back(char(0xC0 | u >> 6));
    out.push_back(char(0x80 | (u & 0x3F)));
  } else if (u < 0x10000) {
    out.push_back(char(0xE0 | u >> 12));
    out.push_back(char(0x80 | (u >> 6 & 0x3F)));
    out.push_back(char(0x80 | (u & 0x3F)));
  } else {
    out.push_back(char(0xF0 | u >> 18));
    out.push_back(char(0x80 | (u >> 12 & 0x3F)));
    out.push_back(char(0x80 | (u >> 6 & 0x3F)));
    out.push_back(char(0x80 | (u & 0x3F)));
  }
}

// A guest UTF-16 string as UTF-8, at most `max` units (-1: no limit).
std::string Utf16ToUtf8(uint32_t va, int max) {
  std::string out;
  for (int n = 0; max < 0 || n < max; n++, va += 2) {
    char32_t u = Rd16(va);
    if (!u) break;
    if (u >= 0xD800 && u < 0xDC00) {
      char32_t lo = Rd16(va + 2);
      if (lo >= 0xDC00 && lo < 0xE000) {
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        va += 2;
        n++;
      }
    }
    AppendUtf8(out, u);
  }
  return out;
}

}  // namespace

uint64_t GuestArgs::Next() {
  if (va_list) {
    uint64_t v = Rd64(va_list);
    va_list += 8;
    return v;
  }
  return Arg64(*c, next++);
}

std::string Format(const std::string& fmt, GuestArgs& args) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    // %[flags][width][.precision][size]type, rebuilt for the host's
    // snprintf with the size the guest's argument has.
    std::string spec = "%";
    size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec.push_back(fmt[j++]);
    auto number = [&](bool allow) {
      if (j < fmt.size() && fmt[j] == '*') {
        j++;
        if (allow) spec += std::to_string(int32_t(args.Next()));
        return;
      }
      while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec.push_back(fmt[j++]);
    };
    number(true);
    bool has_precision = false;
    int precision = -1;
    if (j < fmt.size() && fmt[j] == '.') {
      has_precision = true;
      spec.push_back(fmt[j++]);
      size_t start = spec.size();
      number(true);
      precision = spec.size() > start ? std::atoi(spec.c_str() + start) : 0;
    }
    enum { kDefault, kShort, kLong, kLong64, kWide } size = kDefault;
    for (bool more = true; more && j < fmt.size();) {
      if (fmt.compare(j, 3, "I64") == 0) { size = kLong64; j += 3; }
      else if (fmt.compare(j, 3, "I32") == 0) { size = kDefault; j += 3; }
      else if (fmt.compare(j, 2, "ll") == 0) { size = kLong64; j += 2; }
      else if (fmt[j] == 'h') { size = kShort; j++; }
      else if (fmt[j] == 'l' || fmt[j] == 'w') { size = kLong; j++; }
      else if (fmt[j] == 'I') { size = kDefault; j++; }  // pointer-sized: 32 bits
      else if (fmt[j] == 'L' || fmt[j] == 'z' || fmt[j] == 't' || fmt[j] == 'j') { j++; }
      else more = false;
    }
    if (j >= fmt.size()) {
      out += fmt.substr(i);
      break;
    }
    char type = fmt[j];
    char buf[512];
    switch (type) {
      case '%': out.push_back('%'); break;
      case 'd': case 'i': {
        uint64_t v = args.Next();
        int64_t s = size == kLong64 ? int64_t(v) : size == kShort ? int16_t(v) : int32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), static_cast<long long>(s));
        out += buf;
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        uint64_t v = args.Next();
        uint64_t u = size == kLong64 ? v : size == kShort ? uint16_t(v) : uint32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "ll" + type).c_str(), static_cast<unsigned long long>(u));
        out += buf;
        break;
      }
      case 'p': {
        std::snprintf(buf, sizeof buf, "%08X", uint32_t(args.Next()));
        out += buf;
        break;
      }
      case 'c': case 'C': {
        uint64_t v = args.Next();
        std::string ch;
        if (type == 'C' || size == kLong) AppendUtf8(ch, char32_t(uint16_t(v)));
        else ch.push_back(char(v));
        std::snprintf(buf, sizeof buf, (spec + "s").c_str(), ch.c_str());
        out += buf;
        break;
      }
      case 's': case 'S': {
        uint32_t va = uint32_t(args.Next());
        bool wide = type == 'S' ? size != kShort : size == kLong;
        std::string s;
        if (!va) s = "(null)";
        else if (wide) s = Utf16ToUtf8(va, has_precision ? precision : -1);
        else
          for (int n = 0; (!has_precision || n < precision) && Rd8(va + n); n++) s.push_back(char(Rd8(va + n)));
        std::string sspec = spec;
        if (has_precision) sspec = sspec.substr(0, sspec.find('.'));
        std::snprintf(buf, sizeof buf, (sspec + "s").c_str(), s.c_str());
        out += s.size() < sizeof buf - 1 ? std::string(buf) : s;
        break;
      }
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
        uint64_t bits = args.Next();
        double d;
        std::memcpy(&d, &bits, sizeof d);
        std::snprintf(buf, sizeof buf, (spec + type).c_str(), d);
        out += buf;
        break;
      }
      case 'n': args.Next(); break;  // nothing is written back
      default: out += fmt.substr(i, j + 1 - i); break;
    }
    i = j;
  }
  return out;
}

}  // namespace recomp::xbox360
