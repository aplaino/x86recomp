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
  Event queues: small integers, as the kqueue descriptors they are; an
  event is returned once per trigger (EV_CLEAR) with its last data.
  Directories: getdents/getdirentries return FreeBSD 9's struct dirent
  records, sorted by name.
- recomp_orbis_libc_internal: also the memory functions (memcpy,
  memset, malloc and free on a heap of its own).
- recomp_orbis_fiber: libSceFiber, fibers as host coroutines on the
  game's context memory. The SceFiber block gets 11.00's magic words and
  idle state only.
- recomp_orbis_videoout: libSceVideoOut's main port (handle 1). Flips
  queue and run at the vblank the flip rate allows, on the host thread
  (orbis/host.h); the flip event's ident is 6 << 48 | handle << 40 and
  its data the flip argument << 16 | 1 << 12 (one flip reported). Linear
  A8R8G8B8 and A8B8G8R8 buffers are shown; tiled ones are shown black
  until the GPU draws (only it writes them).
- recomp_orbis_gnm: libSceGnmDriver. Its packet builders write what
  11.00's do (the same dwords; a padding NOP's body is left alone, the
  NOP-wrapped context registers of the Update*Shader functions included,
  which the command processor applies as SET_CONTEXT_REG). Submissions
  run on the command processor (orbis/gpu/cp.h); submit-and-flip
  rewrites the game's flip packet as 11.00 does and flips once the
  submission is done. Only the graphics end-of-pipe event (0x40) exists;
  its data is the id (a timestamp of 0). Mip statistics are accepted and
  never written.
- recomp_orbis_system also: libScePlayGo (the game is installed in full:
  every chunk local, nothing to do, ETAs 0) and libSceDiscMap (a disc
  game's file locations from its sce_discmap.plt: MD5 of the path below
  /app0/, 24-bit block, location and length; every block is on the HDD).
- recomp_orbis_audio: libSceAudioOut. Ports numbered and checked as
  11.00 does; a port holds one buffer and Output returns once the system
  has taken the previous one (every `length` samples at 48 kHz on the
  host's clock, so a run without an audio device keeps time); samples go
  to an SDL audio stream per port, scaled by the port's volumes.
- recomp_orbis_savedata: libSceSaveData on DataDir()/savedata/<user>/
  (each directory with a .meta of its blocks and SceSaveDataParam, and
  its icon .png); a mount maps /savedataN onto it through libkernel's
  files. The library's argument checks and codes are 11.00's, the save
  data service's answers the SDK's error codes. Save data memory is
  memory.dat beside them.
- recomp_orbis_net: libSceNet (init, pools, errno, byte order),
  libSceNetCtl, libSceSsl and libSceHttp contexts, for a console with no
  network connection: NetCtl's state is disconnected and never changes.
- recomp_orbis_np: libSceNpManager and libSceNpWebApi for a user not
  signed in to PSN: the state is signed out, what the PSN service would
  answer fails with SCE_NP_ERROR_SIGNED_OUT.
- recomp_orbis_font: libSceFont and libSceFontFt on the host's FreeType:
  libSceFont's structures, checks, codes and the objects it allocates
  through the game's memory interface are 11.00's; glyphs are
  FreeType's (unhinted, anti-aliased), not libSceFontFt's own
  rasteriser, so pixels differ, and FreeType's allocations through the
  game's memory are not made.
- recomp_orbis_pad: libScePad, the user's controller from the first SDL
  gamepad or the keyboard, always connected; at rest in background
  runs; one state per read; the motion sensors report it lying still.
- libSceNpTrophy (recomp_orbis_np): contexts and handles from 1,
  unlocks kept below DataDir()/trophies; what needs the trophy pack
  (outside the game's files) is not implemented.
- libSceCommonDialog (in recomp_orbis_system): initialisation.
- libkernel also: signal masks (kept per thread; nothing delivers
  signals to the guest) and libScePosix's clock_gettime.
- The loader (orbis/process.cpp) provides __tls_get_addr.

## Not measured on hardware yet
- sceKernelGetDirectMemorySize: 5 GiB, the commonly documented game
  budget.
- The default thread stack (2 MiB, FreeBSD's) and priority (700).
- sceKernelGetTscFrequency: 3 GHz, the rate of the core's rdtsc model
  (lifted.h kTscHz); the console's own TSC runs at its CPU clock.
- The signed-in user's id and name.
- The display's refresh (60 Hz) and the flip event's data below bit 16.
- The GPU's clocks for timestamps (the 100 MHz reference clock, the
  800 MHz core clock).
- libScePad's touch pad (44.86 dots/mm, 1920 x 943) and stick dead
  zones (13).
- PlayGo's chunk count (every id below 1000 is accepted) and its
  default install speed (full).
