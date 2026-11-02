# The Win32 platform module

Rules only, on top of the repository's CLAUDE.md. Design in the
repository's ARCHITECTURE.md; observed behaviour of the Windows APIs
and middleware reimplemented here in docs/ (e.g. docs/miles.md).

## Build and test
- Built by default (RECOMP_PLATFORM_WIN32); its tests run with the
  repository's (`cmake --preset debug`, ctest in out/build/debug).
- The PC game projects (the repository's CLAUDE.md, Layout) are its real
  validation: rebuild and test each after a change here.

## Rules
- No Wine in any product, ever. Wine running an original EXE is allowed
  only as a dev-time behaviour reference.
- 32-bit guests: addresses stay below 0x80000000 unless a game has done
  the large-address audit (ARCHITECTURE.md), and then only behind its
  flag. x64 guests get nothing below 4 GiB.
- HLE handlers live in the library of the DLL that exports them
  (recomp_win32 for the core of Windows, recomp_<dll> for DirectX and
  the middleware), registered with HLE(dll, Name, nargs); x64 guests
  call only those declared with HLE64.
- The Win32 process (PE image, PEB, TEBs, static TLS, SEH) stays here:
  the x86 core sees it only through the hooks RunProcess sets
  (x86/runtime.h).
