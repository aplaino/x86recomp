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

## Users
- Four slots; xam keeps a profile's state per slot: signed in locally
  (bit 0), to Live (bit 1), with its offline XUID at 8 and online XUID at
  0x3C. Here the game project's profile (GameConfig::profile, "Player"
  unless it says otherwise) is signed in locally in slot 0, with the
  offline XUID 0xE000000000000001 (chosen; offline XUIDs have 0xE0 at the
  top); none is signed in to Live.
- XamUserGetSigninState(slot): 0, 1 (locally) or 2 (Live).
  XamUserGetXUID(slot, types, *xuid) returns an HRESULT: E_INVALIDARG for
  a slot past 3, ERROR_NOT_LOGGED_ON (0x80070525) when the slot has no
  profile or no XUID of the types asked (1 offline, 2 and 4 online), and
  always writes the XUID (0 on failure). XamUserGetName(slot, buffer,
  size) converts the name (RtlUnicodeToMultiByteN) into size - 1 bytes
  and a NUL, returning a Win32 code. All from xam's code.

## Profile settings
- XamUserReadProfileSettings(title, slot, XUID count, XUIDs, count, ids,
  *size, results, overlapped) is xam's: it inserts a flags argument (0)
  and calls its Ex form, which refuses no ids, more than 32, a missing
  size pointer, or a size without a buffer (ERROR_INVALID_PARAMETER); sizes
  the results (0x28 bytes per setting and user, plus the string's or
  bytes' size for types 4 and 6, plus 8); writes the size only when the
  caller passed 0 and returns ERROR_INSUFFICIENT_BUFFER when it is short;
  then sends the XGI app (0xFB) message 0xB000C with the request {title,
  slot, XUID count, XUIDs, count, ids, buffer size, results, flags}, and
  returns ERROR_IO_PENDING with an XOVERLAPPED, otherwise ERROR_SUCCESS or
  ERROR_FUNCTION_FAILED by the last error.
- The results: XUSER_READ_PROFILE_SETTING_RESULT (the settings' count,
  their address), then XUSER_PROFILE_SETTING per setting and user
  (source, the slot or XUID at 8, the id at 0x10, XUSER_DATA at 0x18: its
  type byte, the value or a size and an address at 0x20), then the
  strings' and bytes' space.
- XamUserWriteProfileSettings(title, slot, count, settings, overlapped)
  is xam's: it refuses no settings and any id a title may not write
  (ERROR_INVALID_PARAMETER): the profile's own (number below 0x58) and,
  for any title but the dashboard, title specific 1-3. It sends the XGI
  app's message 0xB000D with {?, title, slot, count, settings} (xam leaves
  the first word unset) and returns as the read does.
- What a title writes is kept as the profile keeps it: the profile's own
  settings for the profile (read under 0xFFFE07D1), a title's own for that
  title, source TITLE when read back. They are saved in the data
  directory, profiles/<XUID>/<title>/<id>, a file each holding the
  string's or bytes' own bytes or the 8 bytes XUSER_DATA holds a number
  in; the console keeps them in the profile's package. A string or bytes
  of size 0 is stored empty, address or not: a title writes title
  specific 3 with 0 bytes and an address when it saves its settings, and
  memcpy into an empty std::vector's data(), which may be null, is
  undefined even for 0 bytes (another HLE corrupted its heap on exactly
  that write, rexglue-sdk#502). The runtime test title makes that write;
  under -O3 and UBSan the planted memcpy is reported.
- A setting not written reads as NO_VALUE, except controller vibration,
  DEFAULT 3 (Xenia's); a title's own settings with no bytes. An id not
  listed is fatal. Title 0 is the running title (chosen). Console check.

## Notifications
- An id is an area (bits 25-30), a version (16-24) and a number.
  XamNotifyCreateListener(areas, highest version) makes a listener for
  the areas of the mask, up to that version (10 at most): an object with
  an event (synchronization, signalled while a notification is pending)
  and a pending set holding each id once with its latest value, walked by
  area and number (xam's code). A broadcast reaches each listener whose
  areas and version take it.
- A new listener gets what the system's own listener still holds pending
  (xam copies it). What that is when a title starts is Xenia's choice for
  a title's first listeners: XN_SYS_UI (9) 0, XN_SYS_SIGNINCHANGED (0xA)
  the signed-in slots' mask, XN_LIVE_CONNECTIONCHANGED (0x02000001)
  XONLINE_S_LOGON_DISCONNECTED (0x001510F1), XN_LIVE_LINKSTATE_CHANGED
  (0x02000003) 0. Console check.
- XNotifyGetNext(listener, id or 0, *id, *value) zeroes both, then takes
  the first pending notification (of that id) and returns TRUE; the event
  is cleared when none is left. XNotifyPositionUI positions the system's
  notification popups, of which there are none here.
