// What a program tells the native functions of its statically linked MSVC
// CRT (msvc_crt*.cpp, docs/msvc_crt.md) about that CRT's data.

#pragma once

#include <cstdint>

namespace recomp::native::msvc_crt::vc6 {

// Where a VC6 CRT keeps its locale state (guest addresses of its
// globals): __lc_handle[LC_CTYPE] (0 in the C locale), __lc_codepage,
// __mb_cur_max, _pctype (the ctype table, from index -1) and
// __decimal_point (the LC_NUMERIC decimal point, a char; _output's
// floating-point conversions). The natives of the locale-dependent
// functions (msvc_crt::vc6::) read them; the program sets them before it
// runs any.
struct Locale {
  uint32_t ctype_handle = 0, codepage = 0, mb_cur_max = 0, pctype = 0, decimal_point = 0;
};
void SetLocale(const Locale& l);
const Locale& GetLocale();

// Where a VC6 CRT keeps its floating-point switches (guest addresses):
// _fastflag (nonzero: the math intrinsics skip their checks of the
// result), _adjust_fdiv (1 on a Pentium with the FDIV bug: fprem through
// _adj_fprem) and _matherr_flag (0 when the program has its own
// _matherr: the intrinsics keep their arguments for it). The natives of
// _CIpow and _CIfmod read them; the program sets them before it runs any.
struct Math {
  uint32_t fastflag = 0, adjust_fdiv = 0, matherr_flag = 0;
};
void SetMath(const Math& m);
const Math& GetMath();

}  // namespace recomp::native::msvc_crt::vc6
