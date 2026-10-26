# xboxkrnl: behaviour the runtime reimplements

What the kernel does, as far as the runtime (ARCHITECTURE.md, "Runtime")
depends on it, and where each fact comes from. The kernel itself is not
available (its system update is a delta over the console's own flash), so
the evidence is the system's other code (xam 2.0.17559.0, which imports
xboxkrnl 2.0.17559.0 and ships unencrypted in the system update), what
titles' code reads, and Xenia's and ReXGlue's implementations (BSD-3).
"Console check" marks what only a console can settle.

## Time
- The time base runs at 49.875 MHz, the Xenon's 3.192 GHz / 64: xam
  converts time base ticks with 49,875,000 in nine places (once with
  50,000,000). KeQueryPerformanceFrequency returns the same here (Xenia
  and ReXGlue return 50,000,000). Console check.
- KeTimeStampBundle: interrupt time (be64, 100 ns), system time (be64,
  100 ns since 1601), tick count (be32, ms). Titles read the tick count
  inline.

## r13, KPCR and KTHREAD
r13 holds the current processor's KPCR. Field use in xam and a title
(D-form loads off r13 and off the pointer at KPCR+0x100):

| Field | Read by | What |
|---|---|---|
| KPCR+0x000 | neither (Xenia, ReXGlue: `__declspec(thread)` code) | the thread's static TLS |
| KPCR+0x100 | xam 50, title 14 | the current KTHREAD (the PRCB starts here) |
| KPCR+0x10C | xam 9, title 10 (bytes) | the current processor |
| KPCR+0x150 | xam 3, title 3 | DPC active |
| KTHREAD+0x058 | xam 3, title 6 | unknown; 0 here |
| KTHREAD+0x0C9 | xam 4 (bytes) | unknown; 0 here |
| KTHREAD+0x14C | xam 4, title 1 | the thread id |
| KTHREAD+0x160 | xam, title (read and written) | the last error |
| KTHREAD+0x164 | xam 7 | the fiber (ReXGlue); 0 here |

The other fields set (stack base and limit, TLS, process type, PRCB
pointers, create time, start address, creation flags, the KPCR's stack
bounds and self pointer) and the sizes (KTHREAD 0xAB0, KPCR 0x2D8) are
Xenia's and ReXGlue's layouts. Each guest thread has a KPCR of its own
here; on the console a KPCR belongs to a hardware thread.

## Threads
- ExCreateThread(handle, stack size, thread id, XAPI startup, start,
  context, flags): with a startup routine the thread runs
  `startup(start, context)`, otherwise `start(context)`; the result is the
  exit status. Flag 1 creates it suspended.
- A stack size of 0 takes the title's (the XEX's default stack size
  header); without one 1 MiB, as Xenia chose. At least 16 KiB, in whole
  4 KiB pages. Console check.
- Dynamic TLS: the XEX's slot count, 1024 without TLS info (ReXGlue's
  choice). Console check.
- VSCR[NJ] is set in new threads, as operating systems for this VMX set
  it. Console check.

## Memory
- The views and their page sizes: ARCHITECTURE.md, "Memory". The 4 KiB
  physical view's offset of 0x1000 into physical memory is Xenia's
  (MmGetPhysicalAddress returns va - 0xE0000000 + 0x1000 there).
- MmAllocatePhysicalMemoryEx(flags, size, protect, min, max, alignment):
  the protection's MEM_LARGE_PAGES (0x20000000) picks 64 KiB pages,
  MEM_16MB_PAGES (0x80000000) 16 MiB pages, otherwise 4 KiB; the
  alignment and the bounds apply to the physical address.
- NtAllocateVirtualMemory and NtFreeVirtualMemory follow NT: base and
  size in and out, MEM_RESERVE / MEM_COMMIT, MEM_DECOMMIT / MEM_RELEASE
  (size 0 releases the whole reservation).

## Objects
- DISPATCHER_HEADER: type (byte 0), absolute (1, a critical section's
  spin count / 256), size in words (2), inserted (3), signal state (be32
  at 4), wait list (8). Types: 0 notification event, 1 synchronization
  event, 2 mutant, 3 process, 4 queue, 5 semaphore, 6 thread,
  8 notification timer, 9 synchronization timer.
- KSEMAPHORE: the limit at 0x10. KMUTANT: owner thread at 0x18, abandoned
  at 0x1C; signal state 1 when free, one less per acquisition.
- RTL_CRITICAL_SECTION: a synchronization event's header, lock count at
  0x10 (-1 free), recursion count at 0x14, owning KTHREAD at 0x18.
- SLIST_HEADER: the first entry (be32), depth (be16), sequence (be16).
- Handles: multiples of 4 from 0xF8000004; -2 the current thread, -1 the
  current process.

## Modules
- XexExecutableModuleHandle holds the address of the title's loader entry
  (LDR_DATA_TABLE_ENTRY), whose 0x58 is the address of its XEX headers
  in memory: xam and a title read both so, then pass the headers to
  RtlImageXexHeaderField. The entry's other fields (image base 0x1C and
  size 0x20, full and base names 0x24 and 0x2C as UNICODE_STRINGs, entry
  point 0x3C, load count 0x40, time stamp 0x50) are Xenia's layout. Here
  DllBase (0x18) is the image base, as NT has it (Xenia leaves it 0), the
  full name is `\Device\CdRom0\` and the file's name, and the entry is in
  no list of loaded modules. Console check.
- RtlImageXexHeaderField(headers, key): for a key whose low byte is 1 the
  address of the dword in the header table (a title reads the workspace
  size, 0x00020401, through it), for larger data the address of the data
  (xam reads the execution info's title id at 0xC). For low byte 0 the
  value itself, as Xenia and ReXGlue have it; no caller in xam shows it.
  0 for a key the headers lack. Console check.
- XexGetModuleHandle(name, handle) returns an NTSTATUS: xam's callers test
  it for < 0, and one zeroes the handle first and tests that. A null name
  is the title's module (xam and a title ask so). For a module that is not
  loaded Xenia and ReXGlue return ERROR_NOT_FOUND (0x490), which those
  callers would take for success; here STATUS_DLL_NOT_FOUND and a zero
  handle. xam asks for `xam.xex` by name: the system modules' entries are
  not modelled yet, so asking for one is fatal. Console check.
- XexGetModuleSection(handle, name, data, size) finds the module's XEX
  resources (header 0x000002FF: name[8], address, size each), by exact
  name. XexGetProcedureAddress(handle, ordinal, address) by ordinal; a
  value above 0xFFFF names an export by a string. Failures are Xenia's
  STATUS_NOT_FOUND and STATUS_DRIVER_ENTRYPOINT_NOT_FOUND. Console check.
- XexCheckExecutablePrivilege(n) is bit n of the system flags header
  (0x00030000), as in Xenia and ReXGlue.

## Settings and the display
- ExGetXConfigSetting(category, setting, buffer, size, required size)
  returns an NTSTATUS (xam's callers test it for < 0); the required size
  is a 16-bit count (xam passes a halfword). A null buffer with a size is
  STATUS_INVALID_PARAMETER_3, a short buffer STATUS_BUFFER_TOO_SMALL, as
  Xenia has them. Console check.
- The settings here, all 4 bytes: secured (category 2) AV region, user
  (3) time zone bias, standard and daylight names, dates and biases (1-7),
  language (9), video flags (10), retail flags (12): what a title's
  startup reads. Values: AV region NTSC-M (0x00400100), English (1),
  widescreen (0x00010000), dashboard set up (0x40), as Xenia defaults
  them; the time zone the host's current offset, its abbreviation as both
  names, no daylight saving dates.
- X_VIDEO_MODE (0x30 bytes): width, height, interlaced, widescreen,
  high definition, refresh rate (float), video standard, then two words
  Xenia sets to 0x8A and 1 (3 when not widescreen). xam reads the width
  and height (it treats 1280x1470 and 1920x2205, HDMI's frame-packed 3D
  modes, apart), the rate at 0x14 and the standard at 0x18. The rate is
  50 Hz with the AV region's bit 23, the standard its bits 8-15 (Xenia).
- The AV pack is 8, HDMI in Xenia's numbering. Console check.
- VdGpuClockInMHz is 500 (the Xenos's clock); VdGlobalDevice starts 0
  (titles store their device there); VdHSIOCalibrationLock is a critical
  section, which titles enter (spin count 10000, Xenia's).

## The title's end
- ExRegisterTitleTerminateNotification(registration, create) adds or
  removes a registration (routine, priority, list entry). The routines are
  not run: the title's end is the host process's (Xenia does not run them
  either).
- ExLoadedCommandLine is the title's name in quotes (Xenia's), 1024
  bytes; a title's C runtime splits it into argv. Console check.

## Files
- FscSetCacheElementCount sizes the file system cache; file data is
  cached by the host here, so it succeeds and changes nothing.
- Device names: xam opens `\Device\CdRom0\default.xex` and makes the
  `\??\GAME:` link; `\Device\Harddisk0\Partition1` is the hard
  disk's data partition. D: as Xenia has it.
- Paths in OBJECT_ATTRIBUTES (RootDirectory, ObjectName, Attributes) are
  ANSI_STRINGs, not Unicode; links are named `\??\name:`.
- NtCreateFile(handle, access, attributes, io status, allocation size,
  file attributes, share, disposition, options) takes nine arguments;
  dispositions, options and IO_STATUS_BLOCK.Information are NT's.
- NtQueryDirectoryFile(handle, event, APC routine, APC context, io status,
  info, length, mask, restart) has no class argument: it returns
  FILE_DIRECTORY_INFORMATION with the name in ANSI at 0x40, one entry per
  call (Xenia, ReXGlue). A mask that matches nothing on the first query is
  STATUS_NO_SUCH_FILE, the end STATUS_NO_MORE_FILES.
- FILE_NETWORK_OPEN_INFORMATION, FILE_STANDARD_INFORMATION,
  FILE_RENAME_INFORMATION (replace, root directory, an ANSI_STRING) and
  the volume information are NT's layouts in big-endian, as ReXGlue has
  them. FILE_BASIC_INFORMATION's attributes are NT's 32-bit field at
  0x20; ReXGlue has a 64-bit one there. Console check.
- File systems: GDFX on the disc, FATX on the hard disk (names of at most
  42 characters), 512-byte sectors (Xenia notes code that needs that).
- Files on the disc are read-only (FILE_ATTRIBUTE_READONLY), as on a
  disc image in Xenia.

## Calls
- Arguments past the eighth are 8-byte slots of the caller's parameter
  area from r1 + 0x50 (ReXGlue reads 32-bit ones at r1 + 0x54 + 8 * (i - 8)).
- Variadic arguments (DbgPrint, sprintf) are in the same slots, a double
  as its bit pattern; a va_list (_vsnprintf) points at the next slot.
- The C runtime's formatting is Microsoft's: %S is a wide string in a
  narrow format, %ls and %ws too, I64 a 64-bit size.
