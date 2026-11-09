# The statically linked MSVC CRT

32-bit Windows programs of the VC6 to VC8 era link Microsoft's C runtime
into the EXE (and into each DLL of their own). recomp_msvc_crt
(src/win32/msvc_crt*.cpp) has native versions of its functions, which a
program binds to its copies in its manifest (ARCHITECTURE.md, "Shared
native functions"):

    native 0040CC70 msvc_crt::strlen

Finding the copies: identify the CRT in the program's EXE by signature
and give each function's address. A program may hold several copies
of one function (two names whose code is one, e.g. memcpy and memmove
from one source); bind each. The game project's CMakeLists.txt adds the
oracle:

    recomp_add_msvc_crt_oracle(EXE <exe> SYMBOLS <dir> MANIFEST <manifest>
      STUBS <manifest> SOURCES <stubs.cpp>)

It lifts the bound originals with `--only` and runs each against its
native version on random arguments from the same memory and registers
(platforms/win32/tests/msvc_crt_oracle.cpp has what it compares and how
it draws the arguments). What the originals call outside the CRT (the
program's allocator, the CRT's _lock) is stubbed: STUBS replaces those
functions, SOURCES defines them and `Setup()`.

## Versions
Checked against: VC6 (Visual C++ 6.0's static CRT, LIBC/LIBCMT). A
program with another version runs the oracle against its own copies; a
function whose behaviour differs gets a variant under the version's
namespace, as the locale functions, floor, ceil, _CIpow, _CIfmod,
_output and _woutput have (`msvc_crt::vc6::`).

## Strings, memory, integers (msvc_crt::)
memset, memcpy, memmove, memchr, strlen, strcpy, strcat, strncpy,
strncat, strcmp, strncmp, strchr, strrchr, strstr, strcspn, strpbrk,
wcslen, wcscpy, wcsncpy, wcschr, wcsrchr, wcscspn, and the compiler's
helpers _allmul, _aulldiv, _aullrem, _allshl, _alloca_probe and _ftol.
- strcmp and strncmp return -1, 0 or 1 (the asm versions), comparing
  unsigned bytes.
- memcpy is memmove's code: overlapping copies are as memmove's. strcpy
  and strcat between overlapping strings are undefined in C; the
  originals copy forwards a dword at a time, the natives as memmove.
- Byte and unit arguments (memset's, strchr's, wcschr's) are the low
  byte or unit of their int.
- _aulldiv/_aullrem with a zero divisor: the original runs, to raise the
  divide error from its div.
- _alloca_probe (_chkstk): touches each page from the top down, returns
  with ESP lowered by EAX, ECX kept and the return address in EAX, and
  its own pushes left in memory (ECX at the old ESP-4, the return
  address just below the new ESP).
- _ftol: fistp under RC=chop with the control word put back, on the x87
  model: out-of-range values, NaNs and exception flags as the hardware.

## Locale (msvc_crt::vc6::)
tolower, toupper, _strcmpi, _strnicmp, _memicmp, _strlwr, _strupr,
isalpha, isdigit, isspace, isalnum, iscntrl, _isctype, atoi, atol and
strtol, on VC6's locale globals: the program gives their addresses with
`msvc_crt::vc6::SetLocale` (win32/msvc_crt.h) before it runs.
- After setlocale(LC_CTYPE, ".<code page>") every one of them takes the
  CRT's locale path: classes from the ctype table setlocale built
  (GetStringTypeA), case from LCMapString. The natives read the table
  where the originals do; 'A'-'Z' and 'a'-'z' map to each other (as
  LCMapString does in every code page). The original runs, from the
  start, for another character the table marks for mapping, a string
  with a byte over 0x7F in _strlwr/_strupr (LCMapString maps the whole
  string; Windows-1252 has letters at 0x8A-0x9F too), a multibyte code
  page (__mb_cur_max > 1), and values from 256 up.
- Kept from the originals: tolower/toupper also leave their result in
  the argument's stack slot in the locale path; at a NUL, _strnicmp
  compares the bytes themselves; _strcmpi and _memicmp compare lowered
  bytes, _strnicmp lowered ints; _isctype reads the table for -1..255 in
  any code page.
- Not kept: the locale lock (programs bound to these call setlocale
  once, before other threads start), and _strlwr/_strupr's buffer from
  the program's allocator (the original maps into it and copies back).
- atol and atoi (atoi calls atol): spaces and digits as the table
  classes them (a byte the code page marks as a digit counts, its value
  its byte minus '0'), a sign, no overflow check (the value wraps).
- strtol (through strtoxl): spaces, a sign, a 0x/0X prefix in base 16,
  base 0 as 16 after 0x, 8 after another 0 and 10 otherwise; an invalid
  base or no digits return 0 with the end pointer at the string's start.
  The original runs for a value that does not fit (errno ERANGE, in the
  thread's data) and for a byte over 0x7F the table marks as a letter
  (it passes the signed char to toupper).
- Not native yet: _wcsicmp/_wcsnicmp. Their towlower returns 16 bits in
  AX over whatever EAX held, and they store whole registers into their
  argument slots.

## floor and ceil (msvc_crt::vc6::)
Their double result in ST0. VC6's code sets its own control word with
_ctrlfp (0x173F for floor, 0x1B3F for ceil: all exceptions masked, 64-bit
precision, RC down or up), rounds with frndint, compares the result with
the argument, puts the caller's control word back with _ctrlfp and loads
the result; EAX is left as _ctrlfp's return, the replaced control word
as fnstcw reads it (reserved bit 6 set: 0x177F, 0x1B7F), sign-extended.
The natives take the same steps on the x87 model, so the status word is
the same: PE for a fraction, DE for a denormal argument, the compare's
condition codes. The original runs for a NaN or an infinity (_except1,
_handle_qnan1), when the caller has an exception unmasked (an inexact
result with the precision exception unmasked is raised through
_except1), and when the stack has no free register.

## _CIpow and _CIfmod (msvc_crt::vc6::)
The compiler's intrinsics for pow and fmod: x in ST1, y in ST0, both
popped, the result pushed. VC6's code reads three switches, whose
addresses the program gives with `msvc_crt::vc6::SetMath`
(win32/msvc_crt.h): _fastflag (nonzero: no check of the result),
_adjust_fdiv (1: fprem through _adj_fprem, for the Pentium FDIV bug)
and _matherr_flag (0 when the program has its own _matherr). The
natives take the originals' steps on the x87 model, so the registers,
status word and EAX are the originals' (ECX and EDX are not kept).
- _CIpow stores x and y as doubles under the caller's control word, sets
  the caller's precision with round to nearest and exceptions masked
  (unless the control word is 0x27F), and computes 2^(y * log2 x) with
  fyl2x (on the register y, not its double), frndint, f2xm1 and fscale.
  A negative x needs an integer y (odd: the result negated). Without
  _fastflag it stores the result as a double and checks its exponent.
  EAX: that exponent's bits with 0x27F or 0x20 in the low half; with
  _fastflag, x's exponent bits (or, x denormal, its significand bits)
  with the status word of the integer check in the low half for a
  negative x.
- _CIfmod sets 0x133F (64-bit precision, round to nearest, exceptions
  masked), classifies both arguments with fxam, repeats fprem until it
  completes and, without _fastflag, stores the result as a double.
  Without _matherr_flag it stores x and y as doubles first. EAX: 0x20,
  or with _fastflag the status word of the last fprem.
- The original runs, from the start, for its error and special cases
  (pow: an infinite or NaN x or y as doubles, a zero x, a negative x
  with a fractional y, a double result that underflows or overflows;
  fmod: an argument that is not normal), with _adjust_fdiv set, when the
  caller has an exception unmasked, and when the stack lacks the
  registers the original pushes (_CIpow: two).

## _output and _woutput (msvc_crt::vc6::)
The engines of sprintf, _snprintf, _vsnprintf (and printf/fprintf) and
of _vsnwprintf (and the other wide ones): `_output(FILE*, format,
va_list)` and `_woutput`, the same machine over 16-bit units. Natively for string streams
(_IOSTRG, as the string functions set them up); a stream to a file goes
to the original. It reads the locale globals of `SetLocale`, with
`decimal_point` (__decimal_point) for the floating-point conversions.
- The format: VC6's state machine (its character classes for ' '..'x'
  and transitions), flags `-+ #0`, width and precision with `*` (a
  negative width means `-`, a negative precision none), sizes h, l, w,
  I64 (a bare I and what follows are text; F, L and N are ignored), and
  c C d i o u x X e E f g G n p s S Z. A lead byte of the code page
  (_pctype) takes the next byte with it, even the terminating NUL. A
  type it does not know (B) prints the last conversion's text again.
- Output: write_char's `--_cnt`, a byte at `_ptr`; below zero _flsbuf
  sets _IOERR and the count becomes -1, padding and strings stop, the
  format stops at its next character. The format and argument pointers
  are left in the argument slots as the original leaves them.
- Integers: 64-bit with I64, digits into the 512-byte buffer from its
  end, precision as the minimum digit count (zero padding off when one
  is given), `#` with o forcing a leading 0, with x/X a 0x/0X prefix
  (not for 0); %p is %.8X.
- Strings: %s up to the precision, "(null)" for a null pointer; %Z a
  counted string (ANSI_STRING/UNICODE_STRING; with w, Length / 2 wide
  characters).
- Floating point (VC6's own conversion, through the pointers
  _cfltcvt_init sets): the double goes through fld/fstp (on the x87
  model: a signalling NaN comes out quiet with IE, a denormal sets DE),
  __dtold widens it to 80 bits, $I10_OUTPUT estimates the decimal
  exponent, scales by a power of ten in VC6's 12-byte arithmetic
  (__multtenpow12, __ld12mul: only the high partial products, a carry
  bumping one 16-bit word, round to nearest even), takes 18 digits and
  rounds them to 17. Its tables of 10^(j 8^g) round the significand to
  nearest at 64 bits and keep, as guard bits, the low 16 bits of the
  80-bit rounding (__multtenpow12 takes one off where they are 0x8000 or
  more). _fptostr rounds those digits again to the precision; _cftoe
  writes three exponent digits ("e+000"), _cftog chooses %e when the
  exponent is below -4 or not below the precision; NaN and infinity
  print as 1#QNAN, 1#IND, 1#INF.
- The original runs, from the start (what the native code wrote put
  back), for: a file stream; a character to convert that the line
  below does not cover (or one beyond 0xFF in the C locale: EILSEQ in
  errno); `#` with %.0e/f/g in a multibyte code page (_isctype); a float
  under a control word with an exception unmasked; text that would not
  fit the original's 512-byte buffer (it overruns its stack there).
- _woutput: the format and the output are 16-bit units (_putwc_lk:
  `_cnt` less 2, a unit at `_ptr`; write_char compares what it returns
  with 0xFFFF, so writing U+FFFF itself also makes the count -1). It has
  no `l` prefix (ignored): %s is wide unless h, %S narrow unless w, %c
  and %C alike. Narrow text (digits, floats, %hs, "(null)", %Z without
  w) goes out a character at a time through mbtowc (a lead byte taking
  its trail, a NUL ending it); %hs counts characters, not bytes. Its
  buffer is 1 KiB, so floats have twice _output's room.
- Wide and narrow characters across the engines (wctomb, mbtowc): in
  the C locale any byte and any unit up to 0xFF; in a code page only
  ASCII, which every Windows ANSI code page maps to itself (anything
  else is WideCharToMultiByte's or MultiByteToWideChar's, for the
  original).
- Not kept: wctomb's, mbtowc's and tolower's locale lock (as for the
  locale functions).
