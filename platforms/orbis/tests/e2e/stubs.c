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
STUB(sceKernelLoadStartModule) STUB(sceKernelDlsym)
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

#ifdef LIB_PAD
#define STUB(name) \
  void name(void) {}
STUB(scePadInit) STUB(scePadOpen) STUB(scePadRead)
#endif

#ifdef LIB_COMMONDIALOG
#define STUB(name) \
  void name(void) {}
STUB(sceCommonDialogInitialize)
#endif

#ifdef LIB_SAVEDATADIALOG
#define STUB(name) \
  void name(void) {}
STUB(sceSaveDataDialogInitialize) STUB(sceSaveDataDialogOpen) STUB(sceSaveDataDialogUpdateStatus)
STUB(sceSaveDataDialogIsReadyToDisplay) STUB(sceSaveDataDialogClose)
STUB(sceSaveDataDialogGetResult) STUB(sceSaveDataDialogTerminate)
#endif

#ifdef LIB_TROPHY
#define STUB(name) \
  void name(void) {}
STUB(sceNpTrophyCreateContext) STUB(sceNpTrophyCreateHandle) STUB(sceNpTrophyRegisterContext)
STUB(sceNpTrophyUnlockTrophy)
#endif

#ifdef LIB_GNM
#define STUB(name) \
  void name(void) {}
STUB(sceGnmSetVsShader) STUB(sceGnmInsertPushMarker) STUB(sceGnmDrawInitDefaultHardwareState200)
STUB(sceGnmSubmitCommandBuffers) STUB(sceGnmAddEqEvent)
#endif

#ifdef LIB_LIBC
#define STUB(name) \
  void name(void) {}
STUB(strlen) STUB(strcmp) STUB(memmove) STUB(vsnprintf) STUB(memcpy_s) STUB(rand) STUB(srand)
STUB(gmtime_s) STUB(mktime) STUB(__cxa_guard_acquire) STUB(__cxa_guard_release) STUB(__cxa_atexit)
STUB(__cxa_finalize) STUB(_Dtest) STUB(_FExp) STUB(_FSin) STUB(_Znwm) STUB(_ZdlPv) STUB(powf)
STUB(ldexp)
float _FInf[4];
#endif

#ifdef LIB_RTC
#define STUB(name) \
  void name(void) {}
STUB(sceRtcGetTick) STUB(sceRtcSetTick) STUB(sceRtcCheckValid) STUB(sceRtcParseRFC3339)
STUB(sceRtcParseDateTime) STUB(sceRtcFormatRFC3339) STUB(sceRtcFormatRFC2822)
STUB(sceRtcGetDayOfWeek) STUB(sceRtcTickAddMonths) STUB(sceRtcGetDosTime)
#endif

#ifdef LIB_MSGDIALOG
#define STUB(name) \
  void name(void) {}
STUB(sceMsgDialogInitialize) STUB(sceMsgDialogOpen) STUB(sceMsgDialogUpdateStatus)
STUB(sceMsgDialogGetStatus) STUB(sceMsgDialogGetResult) STUB(sceMsgDialogClose)
STUB(sceMsgDialogTerminate) STUB(sceMsgDialogProgressBarSetValue) STUB(sceMsgDialogProgressBarInc)
#endif

#ifdef LIB_APPCONTENT
#define STUB(name) \
  void name(void) {}
STUB(sceAppContentInitialize) STUB(sceAppContentAppParamGetInt) STUB(sceAppContentGetAddcontInfoList)
STUB(sceAppContentTemporaryDataMount2)
#endif

#ifdef LIB_NGS2
#define STUB(name) \
  void name(void) {}
STUB(sceNgs2SystemCreateWithAllocator) STUB(sceNgs2SystemDestroy) STUB(sceNgs2RackQueryBufferSize)
STUB(sceNgs2RackCreate) STUB(sceNgs2RackCreateWithAllocator) STUB(sceNgs2RackDestroy)
STUB(sceNgs2RackGetVoiceHandle) STUB(sceNgs2VoiceControl) STUB(sceNgs2VoiceGetState)
STUB(sceNgs2SystemRender)
#endif
