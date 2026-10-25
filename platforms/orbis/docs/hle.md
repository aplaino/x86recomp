# Orbis system library HLE

What the HLE in src/orbis/hle/ does where the PS4's behaviour is a
choice, and the values it reports that have not been measured on
hardware yet. Each source file's header comment has the details of its
library.

## Libraries
- recomp_orbis_kernel: libkernel and the libScePosix names it also
  exports. Errors: sceKernel*/scePthread* return 0x80020000 + the FreeBSD
  errno; POSIX functions return -1 with errno (__error(): a word in the
  thread's TCB, orbis/process.h kTcbErrno); pthread_* return the errno.
  - Memory: flexible memory and anonymous mmap from the guest allocator;
    direct memory as offsets into a pool, each mapped to fresh guest
    memory (aliasing one range twice is fatal). File-backed mmap is
    fatal.
  - pthreads: FreeBSD semantics (error-checking mutexes by default,
    static initialisers 0, 1 recursive, 2 adaptive); a normal mutex
    locked twice by its owner is fatal (a self-deadlock) instead of
    hanging. Every object is a 64-byte guest block (a handle) with the
    host state kept beside it. Priorities and affinity are stored for the
    getters only.
  - Semaphores and event flags: first come, first served, whatever the
    FIFO/priority attribute; timeouts are in/out microseconds.
  - Files: /app0 and /hostapp are the game directory (read-only: EROFS);
    /temp0, /download0 and /savedataN are below the per-user data
    directory. Descriptors are the host's, limited to those the guest
    opened (1 and 2 write to the host's stdout and stderr). struct stat is
    FreeBSD 9's.
- recomp_orbis_libc_internal: libSceLibcInternal, the firmware's libc
  that system modules games ship import (a game's own libc is lifted).
- recomp_orbis_system: libSceSystemService (fixed settings: English (US),
  YYYY/MM/DD, 24-hour, UTC, no DST, parental control off, cross to
  confirm), libSceUserService (one user, id 1, "Player"),
  libSceSysmodule (system modules are HLE: loading one records it),
  libSceScreenShot (the system takes none).
- The loader (orbis/process.cpp) provides __tls_get_addr.

## Not measured on hardware yet
- sceKernelGetDirectMemorySize: 5 GiB, the commonly documented game
  budget.
- The default thread stack (2 MiB, FreeBSD's) and priority (700).
- sceKernelGetTscFrequency: 3 GHz, the rate of the core's rdtsc model
  (lifted.h kTscHz); the console's own TSC runs at its CPU clock.
- The signed-in user's id and name.
