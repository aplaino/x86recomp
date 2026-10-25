// Link-time stand-ins that give the test programs' imports a library
// (tools/orbis_sce_elf.py --lib): compiled once per library, with
// -DLIB_<library> (build.py). Never run.

#ifdef LIB_TEST
int test_check(int id, long got, long want) { return 0; }
int test_double(int id, double d, long n) { return 0; }
int test_value;
#endif

#ifdef LIB_KERNEL
void* __tls_get_addr(void* ti) { return 0; }
#define STUB(name) \
  void name(void) {}
STUB(scePthreadCreate) STUB(scePthreadJoin) STUB(scePthreadMutexattrInit)
STUB(scePthreadMutexattrSettype) STUB(scePthreadMutexattrDestroy) STUB(scePthreadMutexInit)
STUB(scePthreadMutexLock) STUB(scePthreadMutexTrylock) STUB(scePthreadMutexUnlock)
STUB(scePthreadMutexDestroy) STUB(scePthreadCondInit) STUB(scePthreadCondWait)
STUB(scePthreadCondSignal) STUB(scePthreadCondBroadcast) STUB(scePthreadKeyCreate) STUB(scePthreadSetspecific)
STUB(scePthreadGetspecific) STUB(scePthreadOnce) STUB(sceKernelCreateSema)
STUB(sceKernelWaitSema) STUB(sceKernelSignalSema) STUB(sceKernelCreateEventFlag)
STUB(sceKernelSetEventFlag) STUB(sceKernelWaitEventFlag) STUB(__error) STUB(open)
STUB(sceKernelOpen) STUB(sceKernelRead) STUB(sceKernelClose) STUB(sceKernelStat) STUB(mmap)
STUB(munmap) STUB(sceKernelUsleep) STUB(sceKernelGetProcessTime) STUB(clock_gettime)
STUB(sceKernelGetdents) STUB(sceKernelCreateEqueue) STUB(sceKernelWaitEqueue) STUB(sceKernelWrite)
#endif

#ifdef LIB_FIBER
#define STUB(name) \
  void name(void) {}
STUB(_sceFiberInitializeImpl) STUB(sceFiberRun) STUB(sceFiberSwitch) STUB(sceFiberReturnToThread)
STUB(sceFiberGetSelf) STUB(sceFiberFinalize)
#endif

#ifdef LIB_SAVEDATA
#define STUB(name) \
  void name(void) {}
STUB(sceSaveDataInitialize) STUB(sceSaveDataMount) STUB(sceSaveDataUmount) STUB(sceSaveDataDelete)
STUB(sceSaveDataDirNameSearch) STUB(sceSaveDataSetParam)
#endif

#ifdef LIB_AUDIOOUT
#define STUB(name) \
  void name(void) {}
STUB(sceAudioOutInit) STUB(sceAudioOutOpen) STUB(sceAudioOutOutput) STUB(sceAudioOutClose)
#endif

#ifdef LIB_GNM
#define STUB(name) \
  void name(void) {}
STUB(sceGnmSetVsShader) STUB(sceGnmInsertPushMarker) STUB(sceGnmDrawInitDefaultHardwareState200)
STUB(sceGnmSubmitCommandBuffers) STUB(sceGnmAddEqEvent)
#endif
