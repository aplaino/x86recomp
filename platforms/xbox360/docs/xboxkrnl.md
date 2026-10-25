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

## Calls
- Arguments past the eighth are 8-byte slots of the caller's parameter
  area from r1 + 0x50 (ReXGlue reads 32-bit ones at r1 + 0x54 + 8 * (i - 8)).
- Variadic arguments (DbgPrint, sprintf) are in the same slots, a double
  as its bit pattern; a va_list (_vsnprintf) points at the next slot.
- The C runtime's formatting is Microsoft's: %S is a wide string in a
  narrow format, %ls and %ws too, I64 a 64-bit size.
