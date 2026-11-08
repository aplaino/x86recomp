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
- RtlTimeToTimeFields fills TIME_FIELDS, eight shorts: year, month, day,
  hour, minute, second, milliseconds, weekday (0 is Sunday); xam's
  FileTimeToSystemTime copies them into a SYSTEMTIME in that order.

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
  exit status. Flag 1 creates it suspended. Bits 24-31 are an affinity
  mask (XAPI's CreateThread builds `1 << processor << 24` in titles): the
  thread reports the mask's lowest processor (KPCR+0x10C, KTHREAD+0xBF)
  from its first instruction, as after KeSetAffinityThread; titles index
  per-processor tables by it. With flag 0x80 *handle is
  the KTHREAD, not a handle (Xenia's; a title hands it straight to
  KeSetBasePriorityThread), referenced for the caller as a handle would
  be (chosen). Console check.
- NtSuspendThread: a host thread cannot be stopped where it runs. A
  thread suspending itself (titles' workers do, to sleep until another
  thread resumes them) stops in the call until its count is 0 again;
  another running thread stops at its next wait or delay, before or after
  it blocks (chosen; on the console at once); one not yet started stops
  before its start routine. Console check.
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
  (size 0 releases the whole reservation; a size releases that range of
  one reservation, and what follows it becomes a reservation of its own:
  titles' heaps give back the tail of a block they shrink in place this
  way), in whole pages of the region's own size: 64 KiB in the
  large-page heap from 0x40000000, for commits and decommits too (a
  title's heap grows by the size a commit returns, and decommits 64 KiB
  at a time). MEM_COMMIT with no base
  reserves the region as well (titles' heaps commit that way, with
  MEM_TOP_DOWN); a commit outside a reservation fails. The first 64 KiB
  are never mapped (a null pointer faults), and no call commits, protects
  or frees them.

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
- Named events, mutants and timers: XAPI's CreateEvent, CreateMutex and
  CreateWaitableTimer pass a name with RootDirectory -4
  (\BaseNamedObjects) and OBJ_OPENIF (0x80), and turn
  STATUS_OBJECT_NAME_EXISTS (0x40000000) into ERROR_ALREADY_EXISTS. An
  existing name opens that object (with OBJ_OPENIF; otherwise
  STATUS_OBJECT_NAME_COLLISION), or fails with
  STATUS_OBJECT_TYPE_MISMATCH if it is another type. Names compare
  exactly unless OBJ_CASE_INSENSITIVE (0x40) is set, as on NT, and go
  with the object's last reference. Other roots are fatal.

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
  handle. xam and titles ask for `xam.xex` by name (a title, on a system
  of build 2610 or later, to call xam's NetDll_XNetStartupEx through
  XexGetProcedureAddress): `xboxkrnl.exe` and `xam.xex` have loader
  entries of their own, named `\SystemRoot\<name>`, with no entry point
  and, as the image, the area that holds their exports' addresses (word n
  for ordinal n, a trap instruction; a call to it runs the handler).
  The names, the image and the layout are chosen. Console check.
- XexGetModuleSection(handle, name, data, size) finds the module's XEX
  resources (header 0x000002FF: name[8], address, size each), by exact
  name. XexGetProcedureAddress(handle, ordinal, address) by ordinal; a
  value above 0xFFFF names an export by a string. A system module's
  function without a handler has an address too, and a call through it is
  fatal, as a call to its import is; its variables are the ones imports
  get. Failures are Xenia's
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

## Audio
- XAudioRegisterRenderDriverClient(*{callback, argument}, *driver): the
  driver handle is 0x4155 ("AU") and the client's index, up to 8 clients;
  a missing callback is E_INVALIDARG. Xenia's. The callback gets the
  address of a word holding the argument, not the argument, on the
  driver's thread (not as a DPC), as Xenia calls it. Console check.
- The driver calls each client once per frame of 256 samples at 48 kHz
  (16/3 ms), and holds back while the host has 16 of its frames queued.
- XAudioSubmitRenderDriverFrame(driver, samples): six channels (front
  left, right, centre, LFE, back left, right) of 256 big-endian floats
  each, one channel after another (Xenia converts them so).
- XAudioGetVoiceCategoryVolumeChangeMask writes 0 and
  XAudioGetVoiceCategoryVolume 1.0: the system's category volumes never
  change here.

## XMA
- The decoder's registers are at 0x7FEA0000, register r at + 4r, and
  little-endian: xam reads the context array's physical address from
  0x600 a byte at a time (low byte first) and keeps it; xam and titles'
  audio libraries write the command registers with byte-reversed stores,
  each followed by eieio.
- After each XMACreateContext, xam and titles' libraries take the
  context's index from its physical address, (pa - array) >> 6, and set
  its bit in the clear registers (0x6A0 + index / 32, bit index % 32).
  The other groups: kick 0x650 (decode), lock 0x690 (stop). Their
  meaning and the clear's effect (no input or output buffer valid, the
  input read offset 32 bits in, past the packet header, the output
  offsets 0) are Xenia's. Console check.
- Titles' audio libraries lock a context whenever a voice stops or
  changes, then poll register 0x606 and read the context's buffer-valid
  bits back. A lock stops the context's decoding (Xenia's); here a kick
  decodes within the poll that takes it, so there is none to stop: it is
  taken and leaves the context as it was.
- 320 contexts of 64 bytes; XMACreateContext(*context) writes a free
  one's address, or 0 with STATUS_NO_MEMORY when none is free;
  XMAReleaseContext zeroes it. Xenia's (which allocates the array in its
  system heap and gives the lowest free context, as here). Console
  check. A context released that is not in use is fatal here.
- The console's decoder takes a write to a command register at once;
  here a thread polls them (yielding, then every 50 us when idle), so a
  title's later writes to a context can race a clear the thread has not
  taken yet. Within a poll it takes locks, then clears, then kicks, so a
  context cleared and kicked between two polls is cleared first. Commands
  reaching it in the order the title wrote them stays open (at eieio, or
  by another route).
- The context's fields are Xenia's layout (word 0: input buffer 0's
  packets, the loop count, both input buffers' valid bits, the output
  ring's blocks and write offset; word 1: buffer 1's packets, the loop's
  subframe end and skip, subframe_decode_count, the output padding, the
  sample rate (24, 32, 44.1, 48 kHz), stereo, the output's valid bit;
  word 2 the input read offset in bits; 3 and 4 the loop's start and end
  offsets, word 4's top bit the current input buffer; 5 to 7 the input
  buffers' and the output ring's physical addresses; word 9 the output
  read offset). Console check.
- Input is packets of 2 KiB: a 32-bit header (the frames that begin in
  it, the first one's bit offset past the header, metadata, the packets
  of other streams before this stream's next) and frames' bits; a frame
  starts with its length in bits (15 bits, 0x7FFF none: padding) and may
  run on into the stream's next packet, the next buffer's when it passes
  the end of its own. Its last bit says whether another frame follows in
  the packet; a frame that runs on into the next packet is followed by
  that packet's first frame whatever that bit (ffmpeg's xma decoders and
  Xenia ignore it). A skip count of 0xFF is the next packet (Xenia's).
- A kick decodes frames from the read offset (past the packet's header
  and the end of a frame begun before it) into the output ring, 16-bit
  big-endian samples, channels interleaved, in blocks of 256 bytes (a
  frame is 512 samples a channel: 4 blocks mono, 8 stereo), until the
  ring has no room for subframe_decode_count blocks (at least one) plus
  the padding, which each frame written reserves too, or no frame is
  available (its packets' buffer not valid). It writes the read offset,
  the current buffer, the write offset and clears the valid bit of an
  input buffer it has left; when the ring is full (the write offset back
  at the title's read offset) it clears the output's valid bit, which the
  title sets again once it has read the ring. A frame partly written
  goes on at the next kick. All of this is Xenia's (one pass a kick, as
  there). Console check.
- A frame is decoded by ffmpeg's wmapro decoder (XMA's frames are WMA
  Pro's, with the decode flags 0x10D6 that ffmpeg's xma decoders use),
  one frame to a packet built for it. The first frame after a clear (or a
  change of rate or channels) gives no samples, as there (and in Xenia):
  the overlap it would complete is with nothing. Samples are saturated
  to [-1, 1] and rounded to 16 bits; a frame's trim is not applied, every
  frame is written whole. Console check.
- Loops (loop count nonzero; 255 is endless): the frame at the loop end
  offset is the loop's last, written up to its subframe loop_subframe_end
  (counting from 0, a subframe a block a channel); decoding goes on at the
  loop start offset, whose first loop_subframe_skip subframes are left
  out, and the count goes down by one. Offsets below 32 are 32. The
  loop's subframe start is not used (as in Xenia). Console check.

## Strings and statuses
- RtlMultiByteToUnicodeN and RtlUnicodeToMultiByteN convert as many
  characters as both buffers hold and report the bytes written; the ANSI
  side is Latin-1 (a byte is its code point, a character above 0xFF
  becomes '?'), as Xenia converts. RtlUnicodeStringToAnsiString uses the
  same conversion, into a kernel pool buffer (allocate) or the string's
  own, truncated with STATUS_BUFFER_OVERFLOW (NT's behaviour). Whether
  the console's ANSI code page is Latin-1 or 1252 is a console check.
- RtlNtStatusToDosError maps the statuses this kernel returns, and those
  titles' own code passes (STATUS_REQUEST_ABORTED from a cancelled file
  copy), as Windows NT does (Xenia's table is NT's, not copied here); a
  customer status (bit 29) stays itself, 0x8007xxxx gives xxxx, any other
  is fatal.
- MmQueryStatistics fills MM_STATISTICS (0x68 bytes, its size first, else
  STATUS_BUFFER_TOO_SMALL): 0x20000 physical pages, the kernel's 0x100
  (Xenia's), the title's available pages (what no committed page or
  physical frame uses: on the console every committed page takes a
  physical one), its 0x2FFE0000 virtual bytes (Xenia's) and those
  reserved in the virtual heaps, the highest physical page; pages by use
  are 0. A title reads the totals and the title's available pages and
  virtual bytes. Console check.
- KeEnableFpuExceptions(enable) sets the KTHREAD's flag at 0x71 (Xenia's
  layout); the C runtime's floating-point control calls it with whether
  any exception is unmasked. Enabled exceptions do not trap here: the
  CPU model stops on one either way (ARCHITECTURE.md, "Floating point").

## The title's end
- ExRegisterTitleTerminateNotification(registration, create) adds or
  removes a registration (routine, priority, list entry). The routines are
  not run: the title's end is the host process's (Xenia does not run them
  either).
- ExLoadedCommandLine is the title's name in quotes (Xenia's), 1024
  bytes; a title's C runtime splits it into argv. Console check.

## Timers
- NtCreateTimer(*handle, attributes, type): a KTIMER (0x28 bytes), a
  notification (0) or synchronization (1) timer, not set; a name as for
  the other objects (Objects).
- NtSetTimerEx(handle, *due, APC routine, APC mode, APC context, resume,
  period, *previous): clears the timer and signals it at the due time (a
  wait's timeout: negative relative, otherwise absolute), then every
  period milliseconds; the previous state is whether it was signalled.
  NtCancelTimer(handle, *current) stops it. A synchronization timer is
  reset by the wait it satisfies. With an APC routine each expiry queues
  routine(context, system time low, high) to the thread that set the
  timer (NT's timer APC: the timer holds one KAPC, so an expiry while the
  last one is still queued has none); setting the timer again or
  cancelling it takes a queued one back. Resume (waking the console
  from standby) does not apply. A title's waitable timers
  (CreateWaitableTimer, SetWaitableTimer) come here.

## APCs
- KAPC (0x28 bytes): type 18 (byte 0), mode (2: 0 kernel, 1 user),
  inserted (3), KTHREAD (4), list links (8), kernel routine (0x10),
  rundown routine (0x14), normal routine (0x18), normal context (0x1C),
  system arguments (0x20, 0x24): Xenia's layout. KeInitializeApc(apc,
  thread, kernel routine, rundown routine, normal routine, mode, context)
  makes an APC with no normal routine a kernel one, as NT does.
  KeInsertQueueApc(apc, argument 1, 2, increment) is FALSE for an APC
  already queued or a thread that has ended. NtQueueApcThread(thread,
  routine, context, argument 1, 2) queues a user APC of the kernel's own.
- An APC runs on its thread as NT runs it: the kernel routine with the
  APC and pointers to the normal routine, its context and the two
  arguments (it may change them, and may free the APC), then the normal
  routine with those. A host thread cannot be interrupted, so a kernel
  APC runs in the thread's next wait or delay (on the console at once),
  outside a critical region (KeLeaveCriticalRegion runs those queued
  inside one), and the wait goes on; user APCs run in an alertable
  user-mode wait or delay, which then returns STATUS_USER_APC (0xC0). An
  object already signalled satisfies the wait first. The queues are the
  kernel's own: the KAPC's list links are not written.
- A thread's end runs its queued APCs' rundown routines (the kernel's
  own APCs are freed), and nothing more can be queued to it.
- I/O: the APC routine of NtReadFile, NtWriteFile and
  NtQueryDirectoryFile is queued as a user APC, routine(context, status
  block, 0), unless the I/O failed (an NTSTATUS error), as NT's I/O
  manager does; XAPI's ReadFileEx and WriteFileEx come here.

## Files
- FscSetCacheElementCount sizes the file system cache; file data is
  cached by the host here, so it succeeds and changes nothing.
- Device names: xam opens `\Device\CdRom0\default.xex` and makes the
  `\??\GAME:` link; `\Device\Harddisk0\Partition1` is the hard
  disk's data partition. D: as Xenia has it.
- Paths in OBJECT_ATTRIBUTES (RootDirectory, ObjectName, Attributes) are
  ANSI_STRINGs, not Unicode; links are named `\??\name:`. A RootDirectory
  of -3 (ObDosDevices) looks the name up among the links, as `\??\`
  does: XAPI's file functions open `D:\...` that way (xam's
  GetFileAttributesExA passes it, with OBJ_CASE_INSENSITIVE).
- NtCreateFile(handle, access, attributes, io status, allocation size,
  file attributes, share, disposition, options) takes nine arguments;
  dispositions, options and IO_STATUS_BLOCK.Information are NT's.
- NtQueryDirectoryFile(handle, event, APC routine, APC context, io status,
  info, length, mask, restart) has no class argument: it returns
  FILE_DIRECTORY_INFORMATION with the name in ANSI at 0x40, one entry per
  call (Xenia, ReXGlue). A mask that matches nothing on the first query is
  STATUS_NO_SUCH_FILE, the end STATUS_NO_MORE_FILES.
- FILE_NETWORK_OPEN_INFORMATION, FILE_STANDARD_INFORMATION,
  FILE_RENAME_INFORMATION (replace, a BOOLEAN whose padding XAPI's
  MoveFile leaves unset; root directory; an ANSI_STRING) and the volume
  information are NT's layouts in big-endian, as ReXGlue has them. A
  rename to another device fails with STATUS_NOT_SAME_DEVICE, as on NT. FILE_BASIC_INFORMATION's attributes are NT's 32-bit field at
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
