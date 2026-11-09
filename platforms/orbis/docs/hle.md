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
    memory (one range mapped twice at once, aliasing, is fatal; a range
    unmapped and mapped again keeps its contents). munmap frees every
    mapping in its range as FreeBSD's does, parts of mappings included,
    so a fixed mapping (16 KiB pages, without overwriting too) fits
    there afterwards. File-backed mmap is fatal. The mappings these make are recorded for
    sceKernelVirtualQuery (an mprotect or a range name splits a record,
    as FreeBSD's map entries split); not measured on hardware: whether
    the kernel merges neighbouring entries, the memory type of flexible
    memory (0 here) and the name of an unnamed mapping (empty here).
    Memory the process maps itself (images, stacks), FIND_NEXT and an
    address no mapping holds are fatal.
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
    FreeBSD 9's. The root (/) is a directory; a top-level name the
    sandbox's root does not have (11.00's, listed over FTP) is absent
    (ENOENT); those it has but nothing here maps are fatal.
- recomp_orbis_libc_internal: libSceLibcInternal, the firmware's libc
  that system modules games ship import (a game's own libc is lifted).
- recomp_orbis_system: libSceSystemService (fixed settings: English (US),
  YYYY/MM/DD, 24-hour, UTC, no DST, parental control off, cross to
  confirm), libSceUserService (one user, id 1, "Player"),
  libSceSysmodule (system modules are HLE: loading one records it),
  libSceScreenShot (the system takes none). The system's events: one
  pending at the start, type 0x10000003 without data (measured on 11.00;
  what raises it is not known); the status's flags all clear, a game in
  the foreground.
  Event queues: small integers, as the kqueue descriptors they are; an
  event is returned once per trigger (EV_CLEAR) with its last data.
  User events (EVFILT_USER) are per queue, level unless added as edge
  (EV_CLEAR); a trigger sets their udata, as 11.00's kevent does.
  Directories: getdents/getdirentries return FreeBSD 9's struct dirent
  records, sorted by name.
- recomp_orbis_libc_internal: also what the system's middleware PRXs
  import, as 11.00 has it: the memory and string functions (malloc,
  free, new and delete on a heap of its own; memcpy_s's checks and codes),
  printf's family to the host's stdout and stderr (the library's
  conversions: an unknown one printed as itself, %n; vsnprintf's C99
  truncation; puts returns 0), rand's generator and global seed, time
  and struct tm (nine ints; local time is UTC), __cxa_atexit and
  __cxa_finalize (one list, newest first, by module), the static
  initialisation guards, and Dinkumware's internals (_Dtest, _FExp, _FLog,
  _FSin, _FInf, _FNan, _Stderr). The math functions are the host's libm,
  so results may differ from the console's in the last bit; the
  floating-point flags the internals raise are not modelled. Not
  implemented (fatal): C++ exceptions (bad_alloc, the _X* throws), wide
  characters and positional arguments in printf, long double, streams
  other than stdout and stderr, std::_Syserror_map's texts (the
  library's own), and the standard streams (ios_base::Init and _Winit
  only count). qsort has no handler yet: its comparator calls must come
  in the library's (Dinkumware's introsort) order.
- recomp_orbis_fiber: libSceFiber, fibers as host coroutines on the
  game's context memory, or, without one, on the stack of the thread
  that first runs them (below its call, as 11.00 does). The SceFiber
  block gets 11.00's magic words and idle state only.
- recomp_orbis_videoout: libSceVideoOut's main port (handle 1). Flips
  queue and run in order on the host thread (orbis/host.h): flip mode 1
  at the vblank the flip rate allows, mode 2 as soon as it is ready,
  between vblanks (measured on a base console with 11.00: mode 2's flips
  20-140 us after the submission's end of pipe, anywhere in the frame, up
  to three per vblank; mode 2 at a flip rate other than 0 is not
  measured and fatal). The flip event's ident is 6 << 48 | handle << 40
  and its data the flip argument << 16 | 1 << 12 (one flip reported). Linear
  A8R8G8B8 and A8B8G8R8 buffers are shown; tiled ones are the GPU's
  colour target at that address (black before it has drawn one). A flip
  submitted from the CPU waits until the GPU has finished what was
  submitted before it, as the console's GPU has by the vblank. Colour
  adjustment with a plain settings size sends the display nothing, as
  11.00's library does; settings that would reach it are not
  implemented.
- recomp_orbis_gnm: libSceGnmDriver. Its packet builders write what
  11.00's do (the same dwords; a padding NOP's body is left alone, the
  NOP-wrapped context registers of the Update*Shader functions included,
  which the command processor applies as SET_CONTEXT_REG; the draw and
  dispatch builders, direct, indirect, multi and count-multi, with their
  argument checks; sceGnmDrawIndexMultiInstanced writes only a NOP, as
  11.00's does). Submissions run on the command processor (orbis/gpu/cp.h,
  the library recomp_orbis_gpu); submit-and-flip, and
  sceGnmRequestFlipAndSubmitDone (a flip packet written into its buffer
  and submitted alone), rewrite the flip packet as 11.00 does and flip
  once the submission is done. The command processor's indirect draws
  write the first (or base) vertex and first instance to the SH registers
  the packet names and draw from vertex 0, as AMD documents the packets
  (the vertex shader adds the register); their arguments are read from
  SET_BASE's base 1 (other bases are fatal), a compute queue's indirect
  dispatches from the packet's address. Draws of the stream-out buffer's
  filled size (USE_OPAQUE) are fatal. End-of-pipe interrupts raise the
  graphics event 0x40, or on a compute ring event 0 to 6 by its pipe;
  their data is the id (a timestamp of 0). Other ids are fatal. Mip statistics are accepted and
  never written. GDS (64 KiB, shared by every ring) is host memory that
  DMA_DATA writes and reads; a compute shader with GDS instructions (of
  them only ds_append is translated; others are fatal) runs in waves
  and binds it read-write, written back before its dispatch returns, so
  that it never runs behind the command processor. A ds_append past M0's
  size or the GDS is fatal (the dispatch's fault word).
- recomp_orbis_gpu draws on gfx::Device (orbis/gpu/renderer.h): its
  shaders translated to GLSL (orbis/gpu/shader.h), colour and depth
  targets as device images found by their guest address (never written
  back to guest memory; as large as their padded pitch and slice or
  DB_DEPTH_SIZE, and sampled through a copy of their top-left corner when
  a T# describes less of them; a T# at a depth target's
  DB_STENCIL_READ_BASE samples its stencil, resolved into an R8 (UNORM or UINT) image;
  a depth target drawn one DB_DEPTH_VIEW slice at a time, or sampled
  through an array T#, becomes a 2D array of as many slices as used; a
  2D T# of several levels over colour targets takes each level from the
  target at its address, else from memory), textures read from guest
  memory once a submission when they change: every level from BASE_LEVEL
  to LAST_LEVEL and every slice (a 2D array's from 0 to LAST_ARRAY, a
  cube's six faces, a 3D texture's depth), laid out as the firmware's
  (11.00) GL library lays out a T#'s levels and slices (orbis/gpu/tiling.h:
  the levels after one another, each with its slices; past level 0 a
  level as wide as the level-0 pitch halved; POW2_PAD; a 2D level smaller
  than a macro tile in its micro tiles' 1D mode; an array slice's banks
  rotated). The T#'s MIN_LOD clamps the sampler's.
  A colour target of four components holds them in the shader's order
  and a T# of it reads them through its COMP_SWAP; one of one or two holds
  them in memory order (the pixel shader exports what COMP_SWAP puts
  there, blending such an alpha-only target uses the colour equation); a
  swap of three components is not modelled.
  Compute image stores write such a target at the image's address (2D,
  level 0), so draws sample what compute stored. A compute group's waves
  take its invocations 64 at a time in order: an invocation's lane (what
  ADD_TID_ENABLE adds to a buffer index) is its place in the group modulo
  64. A compute stage that reads other lanes than its own (v_readlane,
  v_writelane, v_readfirstlane, ds_swizzle across quads) runs in waves:
  each host invocation runs two lanes of a wave (l and l + 32), a host
  subgroup of 32 invocations a wave, so comparisons and carries are
  ballots of the wave, branches on VCC or EXEC test the whole mask, and
  cross-lane reads are subgroup shuffles (v_readfirstlane reads the first
  lane EXEC holds, lane 0 when it is empty); a host whose subgroups are
  not those invocations in order is fatal once the dispatch has run (the
  fault word below). Other stages run one lane per invocation, where
  v_readfirstlane reads the invocation's own lane and a lane mask is
  that lane (bit 0): s_wqm_b64 in a pixel stage sets it when any lane of
  the invocation's quad has it (in another such stage it is fatal), so
  loops that clear a mask a lane at a time end. v_movrels and
  v_movreld index VGPRs by M0; past the stage's VGPRs a source reads
  VGPR0 and a destination is not written (Sea Islands ISA, "Out-of-Range
  GPRs"). A compute stage can load scalars and descriptors at a run-time
  offset: an index a v_readfirstlane reads, scaled and added to a
  pointer the stage also reads at known offsets. Those loads read the
  range the known offsets cover (from the pointer to the end of the last
  one), the descriptors the range holds at the offsets the index can
  give are bound, and the shader takes the one whose words it loaded; a
  load outside the range or a descriptor not among them is fatal once
  the dispatch has run (the stage writes the instruction's address to
  a fault word). Occlusion counts: a draw DB_COUNT_CONTROL counts
  (ZPASS_INCREMENT_DISABLE clear, ZPASS_ENABLE set) also writes 1 for each
  pixel it draws to a count target after its colour targets, blended
  additively, and the counts are summed on the GPU into one 64-bit
  counter, exact whatever PERFECT_ZPASS_COUNTS asks; a counted draw into
  four colour targets is fatal. PIXEL_PIPE_STAT_CONTROL (which counter
  the dumps take, their stride, the render backends) writes nothing but
  is kept; a dump (PIXEL_PIPE_STAT_DUMP) writes, once the draws before it
  are counted, 8 render backends' 64-bit counters 16 bytes apart at its
  address, each with bit 63 set (measured on 11.00 under the control
  word 0x7FC00, the only one handled: a query's begin at +0, its end at
  +8). Backend 0 carries the whole count and the rest stay at 0, so the
  begin-to-end differences add up as the console's do. Buffer loads at a 64-bit address (ADDR64, a V# of base
  0 and no range) whose address is a lane's offset added to a pointer the
  stage reads at known offsets read that pointer's range the same way,
  checked the same way (compute stages; stores and other stages are
  fatal). CLEAR_STATE loads the Sea
  Islands clear state's scissors, masks and depth ranges (AMD's published
  values), not the console's driver's, which are not measured. A draw with
  an ES, a GS and a copy shader runs the ES and the GS as compute passes
  (in waves of 64 invocations, so swizzled ring accesses address by lane)
  on the guest's ESGS and GSVS rings, the ES once per index of the draw
  (shared vertices again for each primitive), each part of the draw as
  many primitives as the rings hold; the copy shader then draws the
  vertices the GS emitted on stream 0, its strips as lists. V# base
  addresses are 40 bits (the ring V#s set bits above them). Fans and
  polygons are drawn as triangles (0, i, i + 1), quads as (0 1 2, 0 2 3),
  quad strips as (2i 2i+1 2i+3, 2i 2i+3 2i+2), line loops as lines;
  rectangle lists as two triangles a rectangle (corners 0 1 2, 1 3 2), the
  fourth corner's position and parameters the second's and third's less
  the first's, as AMD describes the hardware's. Cube maps sample the
  direction the face coordinates and face (the v_cube* instructions'
  results, 1 to 2 across a face) point to. Not drawn yet, each fatal:
  tessellation stages, on-chip and instanced GS, GS streams 1-3, indexed
  rectangle lists, adjacency primitive types, cross-lane instructions
  outside compute stages other than ds_swizzle within quads (its quad
  mode and its bit-mask mode within quads are subgroup quad broadcasts),
  lane counts of a mask (s_bcnt1 and the like), descriptors loaded at run-time
  offsets outside compute stages, 1D and multisampled textures, cube arrays, arrays
  from a slice other than 0, colour targets sampled as other than 2D or
  from a level past 0, depth targets sampled with levels, samples with a
  level clamp, offsets into a texture of several levels at a level not
  known (implicit, explicit, derivatives), cube image loads and cube
  samples with derivatives or offsets, draws into several slices at once,
  textures in rotated and PRT tile modes, textures other than 3D ones in
  thick modes (a 3D texture in 1D thick is tiled in 8x8x4 micro tiles,
  its depth padded to 4, as the GL library does; a compressed one takes
  1D thin), 2D thick modes, compressed textures in linear modes, CB_COLOR_CONTROL modes resolving and decompressing,
  targets with FMASK compression, gathers from a texture of several
  levels at a level not known, and fast clears of colour targets in
  formats other than 16_16_16_16 float and 8_8_8_8 UNORM or with a
  component swap. An unmapped texture reads as black. The
  tile mode table (GB_TILE_MODE0-31, GB_MACROTILE_MODE0-15) is
  the console's, read on firmware 11.00; its GB_ADDR_CONFIG read back 0,
  and the pipe interleave (256 bytes), pipes (8) and row size (1 KiB) are
  the firmware's own addressing's (its GL library). Stencil REPLACE_OP
  writes the test value, not DB_STENCILREFMASK's op value. A depth
  target's HTILE (one that tracks depth alone) written in full as
  cleared, every tile's ZMASK 0, by a dispatch or DMA_DATA, clears the
  target to DB_DEPTH_CLEAR at its next draw; other HTILE writes are
  fatal. A colour target with CMASK fast clears (CB_COLORn_INFO's
  FAST_CLEAR) is the same: its CMASK written in full as cleared (every
  tile 0), or found so the first time it is drawn into, fills the
  target with CB_COLORn_CLEAR_WORD0-1 at its next draw. Clears are
  applied at once, so a draw eliminating fast clears (CB_COLOR_CONTROL
  mode 2) draws nothing; one with another clear colour than the one
  applied is fatal. The depth clear of DB_RENDER_CONTROL is modelled.
- recomp_orbis_system also: libScePlayGo (the game is installed in full:
  every chunk local, nothing to do, ETAs 0, prefetching succeeds; the
  language mask is the service's answer for a console set to English,
  0x4000000000000000, measured on 11.00) and libSceDiscMap (a disc
  game's file locations from its sce_discmap.plt: MD5 of the path below
  /app0/, 24-bit block, location and length; every block is on the HDD).
- recomp_orbis_audio: libSceAudioOut. Ports numbered and checked as
  11.00 does; a port holds one buffer and Output returns once the system
  has taken the previous one (every `length` samples at 48 kHz on the
  host's clock, so a run without an audio device keeps time). Samples
  go to the software mixer (runtime/audio), a queue-fed voice per port,
  scaled by the port's volumes; 8 channels are folded to stereo, the
  standard order's swapped side and back pairs alike.
- recomp_orbis_ngs2: libSceNgs2, rendering on the caller's thread into
  the game's buffers (the game gives them to libSceAudioOut). What
  follows 11.00: the allocator protocol (the context-buffer info, the
  free callback on failure and destroy), handles at 11.00's offsets in
  the game's buffers (system +0x68, rack +0x80, voices after the rack
  object; a rack handle as a patch destination is its voice 0), every
  option and parameter layout with its clamps, checks and codes, the
  voice events, fades (64 samples) and state flags, states as of the
  last render (exactly 4 bytes: the flags; fewer than the rack's state:
  nothing), controls taking effect at the next render (setup without
  play kills a voice in use, games built with SDK 1.75 or later), and the
  render order (sources, then the other voices depth-first from each
  mastering voice; buffers no voice wrote are zeroed). The sampler reads
  PCM in place from the game's memory through its block ring (repeats,
  exit loop, stream continuations, termination), and ATRAC9 through
  the decoder as 11.00 walks it: units of a superframe plus the frame
  margin, the superframe at the frame offset, a block's skip dropped from
  its first frames, the decoder restarted at a block that skips (blocks
  without a skip play on gaplessly), the setup's checks (decoders of the
  kind flag 1 picks, the 0xFE config byte, its channel count). Ours: the
  decoder is libSceAjm's (LibAtrac9, as below); it decodes a whole
  superframe at once, where 11.00 decodes a frame at a time (the decoder
  state differs only when a block ends inside a superframe and the next
  does not skip); a superframe that does not decode or a config LibAtrac9
  refuses (an odd superframe index) is fatal. Both resample through a 4-tap
  Hann-windowed sinc resampler (512 phases, 3 samples of start-up
  history); envelopes, RBJ biquads, the compressor/limiter, distortion
  and peak meters follow 11.00's formulas; the mastering rack's matrix,
  LFE low-pass (4th-order Chebyshev, 0.1 dB), limiter and gain feed the
  buffer its output id names. Ours rather than 11.00's: the voices' work
  memory is on the host, so a rack's buffer is only 0x1058 + 0xc0 per
  voice bytes; the default channel matrices (standard fold-downs: C and
  surrounds at -3/-6 dB, no LFE); the I3DL2 reverb's delay lengths,
  reflection patterns and comb routing (the structure, the property
  mapping and clamps are 11.00's); the curve and coefficient tables
  (computed). Not implemented yet, each fatal: VAG and
  oscillator waveforms, voice callbacks, user effects, custom racks and
  reverb parameters, the alternative default matrices, and reading
  parameters back (the 0x8000 ids).
- recomp_orbis_ajm: libSceAjm, the library and the kernel driver behind
  it as 11.00 has them: the library's checks and the words its job
  builders write (the bits they keep of what was there included), the
  codecs a game may register, instance ids (slot | codec << 14, slots
  from a FIFO starting at 1, the pseudo-instance 0x80000), batch ids (the
  games' FIFO 0-190), priorities 10-90 and the priority-violation rule,
  the batch's two-pass validation with the error block (the job, the
  command's offset and the job's return address; the error code
  returned as malformed batch), busy instances until their batch is
  waited for, cancelling, the SDK 5.00 zeroing of the error block,
  sceAjmDecAt9ParseConfigData and sceAjmDecMp3ParseFrame (Xing/Info with
  LAME's delay and padding, VBRI, the 0xB4 tag and its CRC). A batch's
  inputs are read at its start and its jobs run when it is waited for.
  The jobs run on the audio coprocessor in 11.00, whose firmware decides
  what they do. Measured on 11.00 (a console run's AJM log, ATRAC9 with
  STREAM and MULTIPLE_FRAMES into S16): an ATRAC9 instance no
  initialisation job configured takes its stream from a RIFF/WAVE header
  at the start of a run's input (WAVE_FORMAT_EXTENSIBLE with ATRAC9's
  subformat: the config from fmt, the samples and the delay from fact,
  the header's bytes consumed up to the data chunk), skips the delay,
  stops output at the samples, and takes a new header once the data is
  used up; a frame consumes its own length (byte-aligned) and the last
  frame of a superframe the superframe's rest; a run decodes as many
  frames as the output holds and the input has; the result is 0x10 when
  the output has no room for the next frame, else 0x8 when the input is
  used up; a 24-byte sideband buffer gets the result and STREAM records
  only, with no error. The decoder is LibAtrac9 (MIT, NOTICE), whose
  frame lengths match 11.00's for every run measured; its S16 samples
  match or differ by 1 (90.5% equal: the coprocessor computes with other
  arithmetic). The rest is ours, MP3 on ffmpeg's decoder: initialisation
  takes ATRAC9's 4 config bytes (MP3: no parameters); a run decodes a
  frame, or with MULTIPLE_FRAMES as many as the input and output hold,
  into the instance's format (flags bits 7-9: S16 rounded to nearest,
  S32, float), the output buffers filled one after the other. The
  sideband records, in order: the result (MP3 and a single frame: 0; the
  internal result 0), STREAM (bytes consumed, bytes written, samples
  output since the stream's start), MULTIPLE_FRAMES (frames, when the
  buffer has room), FORMAT (channels, WAVE speaker mask, rate, the
  instance's format, bitrate), GAPLESS (samples to output, to skip,
  skipped). Gapless decoding skips the first samples and stops output
  after the total (frames are still consumed; a RIFF stream's delay and
  samples are its skip and total); RESET restarts the stream and the
  skip.
  Contexts are small numbers of the HLE's own (11.00's are the
  descriptors of /dev/ajm); memory registration keeps 11.00's limits but
  takes any memory (11.00 wants direct memory); sceAjmStrError's texts
  are the codes' SDK names; sceAjmBatchErrorDump logs the error block.
  Not implemented yet, each fatal: codecs other than ATRAC9 and MP3,
  multi-codec instances (Extend, Switch), jobs on 0x80000, FLUSH and
  jobs with other than one CONTROL or RUN command, codec-specific
  sideband records, codec info, resampling, a run that decodes no frame,
  a sideband buffer too small for its records, an ATRAC9 frame that does
  not end in a run's input, a RIFF header that is not ATRAC9's, lacks
  fmt, fact or data in the input, or whose fact delays differ, a RIFF
  header inside a stream's data or on an initialised instance's stream,
  input past a RIFF stream's data, and anything the decoder rejects (the
  coprocessor's error results are not measured).
- recomp_orbis_avplayer: libSceAvPlayer on ffmpeg, reading through the
  game's file callbacks on a guest service thread; video as NV12 in the
  game's texture memory (pitch: the width rounded up to 64), released
  by the player's clock; audio as 16-bit chunks of 1024 frames; ready,
  play and stop events. Its structures are 11.00's; the pixels are
  ffmpeg's decoders'.
- recomp_orbis_videodec: libSceVideodec2 (AVC) on ffmpeg's H.264
  decoder. The checks, codes and structures are 11.00's libSceVideodec2
  (with libSceVdecCore's compute queue handle: the memory + 0x290500);
  the memory sizes and the pipeline's delay per configuration are
  measured (below), others are fatal, as are HEVC, the other decoder
  kinds and the 56-byte output info. A decoder takes in delay - 1 access
  units, then hands out one picture per access unit in output order,
  written into the frame buffer passed with it; a flush hands out the
  rest one per call (a decode during one: 0x811D0100). The handle is the
  CPU memory; the memory the console's decoder uses is never touched.
  Picture info comes from the stream's SPS; the VUI is only known for
  the measured streams' shape (timing, bitstream restrictions,
  max_dec_frame_buffering = max_num_ref_frames), others are fatal, as
  are field coding, scaling matrices, POC type 1 and pictures smaller
  than the decoder's maximum (their pitch). The console's own record in
  the frame buffer's last KiB is not written. Pointers other than null
  are taken as mapped; memory types are checked from libkernel's
  records (memory the process maps itself: fatal).
- recomp_orbis_savedata: libSceSaveData on DataDir()/savedata/<user>/
  (each directory with a .meta of its blocks and SceSaveDataParam, and
  its icon .png); a mount maps /savedataN onto it through libkernel's
  files. The library's argument checks and codes are 11.00's, the save
  data service's answers the SDK's error codes. Save data memory is
  memory.dat beside them. A title id naming the game's own (param.sfo's
  TITLE_ID) is its own save data; other titles' are not implemented yet.
- recomp_orbis_net: libSceNet (init, pools, errno, byte order),
  libSceNetCtl, libSceSsl and libSceHttp contexts, for a console with no
  network connection: NetCtl's state is disconnected and never changes.
- recomp_orbis_np: libSceNpManager and libSceNpWebApi for a user not
  signed in to PSN: the state is signed out, what the PSN service would
  answer fails with SCE_NP_ERROR_SIGNED_OUT. libSceNpTus (initialized
  by its own module_start in 11.00): no title context exists; the calls
  that create one are not implemented.
- recomp_orbis_font: libSceFont and libSceFontFt on the host's FreeType:
  libSceFont's structures, checks, codes and the objects it allocates
  through the game's memory interface are 11.00's. Glyphs follow
  libSceFontFt's use of FreeType: outlines loaded in font units,
  unhinted, scaled in its 16.16 fixed point (each edge rounded on its
  own), placed at the pen truncated to 1/64 pixel and rasterised by
  FreeType's anti-aliasing rasteriser into the scissor, each pixel
  keeping the larger coverage; for SDK versions 0.93 to 2.50 a row lower,
  as there. Kerning comes from the 'kern' table, else from GPOS pair
  adjustments (the 'latn' script's default language, feature 'kern').
  Pixels can differ from the console's where the host FreeType's
  rasteriser or glyph loaders differ from the firmware's; the effects
  (weight, slant), vertical layout and system fonts are not implemented,
  and FreeType's allocations through the game's memory are not made.
- recomp_orbis_pad: libScePad, the user's controller from the first SDL
  gamepad or the keyboard, always connected; in background runs at rest
  or pressed as <ID>_TEST_PAD's file says (timed presses and stick
  positions, for tests; read as it grows, so a driver can append to it);
  one state per read; the motion sensors report it lying still. While a
  system dialog is open the game reads it intercepted
  (SCE_PAD_BUTTON_INTERCEPTED alone, sticks centred). libSceMouse: no
  mouse is attached; a read gives one zeroed sample, as 11.00's library
  does with no input; the driver's first handle as measured (0x160500),
  a second open not modelled.
- libSceNpTrophy (recomp_orbis_np): contexts and handles from 1,
  unlocks kept below DataDir()/trophies; the unlock state's trophy count
  from the pack's table of contents (app0's sce_sys/trophy/trophyNN.trp,
  NN the label's entry in sce_sys/npbind.dat: its TROPnnn.PNG icons,
  which matched a console's count for a 54-trophy pack); what needs the
  pack's encrypted contents (names, grades, the platinum trophy) is not
  implemented.
- libSceCommonDialog (in recomp_orbis_system): initialisation.
- recomp_orbis_system also: libSceRemoteplay's initialisation (11.00's
  checks; nobody streams from the console).
- recomp_orbis_system also: libSceRtc as 11.00 has it (ticks in
  microseconds since 0001-01-01, its checks in their order and codes, the
  RFC 3339 and RFC 2822 parsers and formatters with their quirks, the
  checks that depend on the SDK version). Local time is UTC; the network
  clocks read the host's clock; setting a clock is not implemented. Of
  the time zone names the RFC 2822 parser knows, RFC 2822's own (GMT and
  the North American ones) are implemented; the others are fatal.
- recomp_orbis_system also: libSceAppContent. The library's checks (and
  their SDK versions) are 11.00's; the system service it asks is
  modelled as a game installed in full: not a trial, the full SKU (3),
  param.sfo's USER_DEFINED_PARAM_1-4, no add-on content (its info: not
  found), temporary data at /temp0 (cleared when mounted with the format
  option). Mounting add-on content, entitlement keys, download progress,
  free space with download data and a USER_DEFINED_PARAM_2-4 param.sfo
  does not have are not implemented. With DOWNLOAD_DATA_SIZE 0 the free
  space query answers 0x80D90004 (measured). Without USER_DEFINED_PARAM_1, AppParamGetInt(1)
  returns 0 and the value 0 (measured on 11.00).
- recomp_orbis_savedata_dialog: libSceSaveDataDialog. The library's side
  is 11.00's: the states, the checks of the parameter and everything it
  points to (the common dialog base's size and magic included; some
  checks depend on the game's SDK version), the codes (libSceSaveData's
  that it turns into its own included) and the result's layout. The
  system's side is the runtime's own: a dialog drawn over the game's
  frames (runtime/ui, in the host's sans-serif font) while it has the
  controller. The list for loading shows the listed directories that
  exist, in the game's order, each with its icon, title, subtitle, time
  (the newest file's, also given as the param's mtime) and size, and the
  focused one's details; up and down move, cross picks, circle backs
  out. The list for saving shows the game's new item (its title and PNG
  icon) first, then the listed directories that exist; picking the new
  item answers OK with no directory name (games name the new directory
  themselves); the focus positions of save data fall on the new item
  when there is none. The library's check of the new item is 11.00's
  (title: under 128 bytes, no line break, UTF-8 as its own decoder reads
  it; an icon of 1 to 0x1C800 bytes, or none and size 0; 32 zero bytes),
  for the save type only. The progress message shows the first listed directory that
  exists and the work going on until the game closes it. The progress
  bar (mode 5) shows the first listed directory that exists (else the
  save list's new item), the game's message or the work going on, and
  the bar the game sets; its parameter's checks are 11.00's (bar type 0,
  a system message type 0-2, from SDK 2.00 a message or a system message
  type, a UTF-8 message, zero bytes), and the setter's (not initialized
  before Initialize, finished after a dialog has finished, target 0, the
  progress bar mode, the value capped at 100). The restore message, the
  setter before the first dialog, other modes, messages and list types
  are not implemented.
- recomp_orbis_savedata_dialog also: libSceMsgDialog. The library's side
  is 11.00's: the checks of SceMsgDialogParam and what it points to (in
  its order: every parameter before the library's state), the states
  (FINISHED seen only through UpdateStatus; a finished dialog opens
  again), the codes, the result and its fix-up for SDK 3.00 on (backing
  out of an OK dialog reads as OK, of a yes/no one as no), and the
  progress bar's value (Inc stops at 100, SetValue does not). The system's
  side is the runtime's own: the message and its buttons (or bar) in a
  box over the game's frames; left and right move, cross chooses, circle
  backs out where the dialog has a cancel. System messages and button
  type 4 are not implemented.
- libkernel also: signal masks (kept per thread; nothing delivers
  signals to the guest) and libScePosix's clock_gettime.
- The loader (orbis/process.cpp) provides __tls_get_addr,
  sceKernelLoadStartModule and sceKernelDlsym (ARCHITECTURE.md).

## Measured on hardware
Firmware 11.00, a base (not Neo) console, in a game's process unless
noted:
- sceKernelGetDirectMemorySize: 4.5 GiB (0x120000000). A payload's
  process reads 768 MiB, its own budget.
- The default thread stack (64 KiB) and priority (700), for an
  initialised attribute and for a thread created without one. A
  payload's process gets 2 MiB.
- The signed-in user's id: 0x10000000. The HLE keeps that user's save
  data in the host folder savedata/1.
- The display: 1920 x 1080 at 59.94 Hz. The flip event's ident is
  6 << 48 (no handle bits) and its data the flip's argument from bit 16
  and 1 in bits 12-15.
- libScePad's touch pad (44.86 dots/mm, 1920 x 942) and stick dead zones
  (13), a DualShock 4 connected locally.
- The GPU's core clock: 800 MHz (in a payload's process).
- libSceVideodec2 for AVC High 4.2, 1920 x 1088, DPB -1, pipeline depth
  4: CPU memory 0x334CC00, GPU 0x48EAE00, CPU+GPU 0x304B000, frame
  buffer 0x331400 aligned to 0x100; the first picture with the 8th
  access unit, in display order; pictures as NV12 at a pitch of 1920,
  chroma after the 1088 coded rows, byte for byte ffmpeg's (three
  pictures compared). The picture info of a stream with timing and
  bitstream restrictions only in its VUI.
- sceKernelGetTscFrequency: the console reports 1,593,756,095 Hz (its
  CPU clock; rdtsc against the process clock agrees). The HLE reports
  3 GHz instead, the rate of the core's rdtsc model (lifted.h kTscHz),
  so the two stay consistent with each other.

## Not measured on hardware yet
- The user's name.
- What the save data dialog's result holds when the game closes the
  dialog (the HLE: result 0, button 0, no directory) and the button of a
  list's answer (0); whether the list keeps the game's order.
- The system message types' meaning: 5 is taken to be the progress
  message (the library's checks fit the SDK's numbering: 1 no data, 3
  overwrite, 4 and 8 no space, saving only).
- That a system dialog's game pad reads carry only the intercepted bit.
- The message dialog's answers: a choice's result and button id (OK and
  yes 1, no and the second button 2, cancel: user canceled with none), a
  dialog the game closes (user canceled, none), and whether the dialog
  libraries refuse each other while one is initialised
  (sceCommonDialogIsUsed: not modelled).
- What libSceAjm's jobs do on the audio coprocessor beyond the measured
  ATRAC9 streams (recomp_orbis_ajm above): the sideband records' values
  for other flags (the layouts follow the SDK's record sizes; field
  meanings are the community's), the results of single-frame runs and
  of MP3, the frame count's record, the speaker masks, gapless
  decoding's details, what MP3 initialisation takes, and the
  coprocessor's error results.
- The Remote Play service's answer to sceRemoteplayInitialize (the HLE:
  success).
- The add-on content service's answers (the SKU, the boot param's attr,
  an empty list, "not found" for an add-on's info).
- The flip event's data below bit 12 (it varies on the console; the HLE
  gives 0).
- The handles' values (the console's video out and pad handles are not
  small numbers; the HLE's are 1).
- The GPU's 100 MHz reference clock for timestamps.
- Which GPU event a compute ring's end-of-pipe interrupt raises: the HLE
  raises its pipe's (0 to 6), as games register ids 0 to 6 beside 0x40,
  one per compute pipe.
- sceVideoOutGetResolutionStatus's 12 reserved bytes (zero on the
  console, but not known to be written).
- Which vertex of a fan's, quad's or polygon's triangle is the provoking
  one (flat-shaded parameters): the HLE's triangles keep the first vertex
  first. Where the v_cube* instructions put a direction exactly between
  two faces (the HLE: z, then y, then x, as AMD's instruction description
  reads), and how a cube's level is chosen near a face's edge (the
  host's).
- A buffer format store of 16-bit floats (16_16, 16_16_16_16): the HLE
  writes the components the instruction stores and leaves the format's
  others as they are, and converts to nearest even (infinity past the
  range, half denormals kept, NaNs quieted, their payload's top bits
  kept).
- PlayGo's chunk count (every id below 1000 is accepted; a package's
  count is in its playgo-chunk.dat, package entry 0x1001, which a copy of
  app0 does not hold), what an id past it returns, and the default
  install speed (full).
