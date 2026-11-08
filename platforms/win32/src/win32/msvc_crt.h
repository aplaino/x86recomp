// What a program tells the native functions of its statically linked MSVC
// CRT (msvc_crt*.cpp, docs/msvc_crt.md) about that CRT's data.

#pragma once

#include <cstdint>

namespace recomp::native::msvc_crt::vc6 {

// Where a VC6 CRT keeps its locale state (guest addresses of its
// globals): __lc_handle[LC_CTYPE] (0 in the C locale), __lc_codepage,
// __mb_cur_max and _pctype (the ctype table, from index -1). The natives
// of the locale-dependent functions (msvc_crt::vc6::) read them; the
// program sets them before it runs any.
struct Locale {
  uint32_t ctype_handle = 0, codepage = 0, mb_cur_max = 0, pctype = 0;
};
void SetLocale(const Locale& l);
const Locale& GetLocale();

}  // namespace recomp::native::msvc_crt::vc6
