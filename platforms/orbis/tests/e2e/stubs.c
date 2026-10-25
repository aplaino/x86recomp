// Link-time stand-ins that give the test programs' imports a library
// (tools/orbis_sce_elf.py --lib): compiled once per library, with
// -DLIB_TEST or -DLIB_KERNEL. Never run.

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
#endif
