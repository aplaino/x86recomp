# xam: behaviour the runtime reimplements

What xam (2.0.17559.0, which the console's system update ships
unencrypted) does, as far as the runtime depends on it, and where each
fact comes from: xam's own code first, then what titles read, then
Xenia's and ReXGlue's implementations (BSD-3). "Console check" marks what
only a console can settle.

## The console
- XGetAVPack and XGetVideoMode are each a single branch to the kernel's
  HalGetCurrentAVPack and VdQueryVideoMode, so they return what those do
  (docs/xboxkrnl.md, "Settings and the display").
- XGetGameRegion returns a halfword the system keeps in memory (xam reads
  0x8E038602): here 0x00FF, North America, with the NTSC-M AV region
  (chosen). XamGetSystemVersion packs XboxKrnlVersion: major and minor in
  4 bits each, the build, the QFE's low byte (xam's code).

## The loader
- XamLoaderGetLaunchDataSize writes the size of the launch data (what a
  title hands the next title it launches) and returns ERROR_NOT_FOUND
  when there is none; XamLoaderGetLaunchData returns ERROR_NOT_FOUND
  then too (ERROR_BUFFER_OVERFLOW for a short buffer). A title the system
  starts from its disc has none (xam's code).

## Networking
- There is no network interface: a console with no cable. NetDll_* take
  the caller first, a title (1) or the system (2), and refuse others with
  WSAEINVAL (xam's code). NetDll_WSAStartup fills WSADATA as xam does:
  the version asked for, the highest 2.2, empty description and status,
  no limits. NetDll_XNetStartup succeeds.

## Memory
- XamAlloc(attributes, size, *address) returns an HRESULT (E_INVALIDARG
  without an address); the attributes pick xam's heaps and physical
  memory, and only 0 is read here (heap memory, zeroed). XamFree is xam's
  XMemFree, which frees heap memory below 0xA0000000 and physical memory
  above (xam's code).

## Messages
- XMsgInProcessCall(app, message, p1, p2) calls the app's handler and
  returns its result; XMsgStartIORequest(Ex)(app, message, overlapped,
  buffer, size[, xparams]) queues the request as a task of the app, which
  completes it with XMsgCompleteIORequest(overlapped, result, extended
  error, length): length in the XOVERLAPPED's word 1, extended error in
  word 6, result in word 0 (ERROR_IO_PENDING until then), then its event
  set or its completion routine queued as an APC. Without an XOVERLAPPED,
  xam waits and leaves the extended error as the thread's last error,
  returning the scheduling's status (xam's code).
- Here every request completes before the call returns, with the
  handler's HRESULT as the extended error and a Win32 result of 0, the
  code of a FACILITY_WIN32 HRESULT, or ERROR_FUNCTION_FAILED (chosen: each
  app makes its own result). Completion routines are fatal (no APCs yet).
- Apps titles call: 0xFA (the music player, XMP), 0xFB and 0xFC (Xenia's
  XGI and XLiveBase).

## The music player
- XMPSetPlaybackController (0x7001A, {client, controller, locked}): xam
  refuses a title (client 2) any controller but 0 (the title) or 4 (give
  it back) with E_INVALIDARG; with no user music here the title keeps
  playback. XMPGetPlaybackController (0x7001B, {client, *controller,
  *locked}): the title's (0), unlocked. Message names are Xenia's.
