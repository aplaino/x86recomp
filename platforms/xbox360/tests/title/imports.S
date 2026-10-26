// The test title's imports, as a title's are: a slot per import (the
// loader writes the export's address into it) and, for a function, the
// thunk the code calls. xbox360_elf2xex finds the records by their
// __xex_import symbols; library 0 is xboxkrnl.exe, 1 xam.xex.
  .macro FUNCTION ordinal, name, library=0
  .section .data.imports, "aw"
  .p2align 2
  .globl __imp_\name
__imp_\name:
__xex_import_slot_\name:
  .long (\library << 16) | \ordinal
  .text
  .p2align 4
  .globl \name
\name:
__xex_import_thunk_\name:
  .long 0x01000000 | (\library << 16) | \ordinal
  .long 0, 0, 0
  .endm

  .macro VARIABLE ordinal, name
  .section .data.imports, "aw"
  .p2align 2
  .globl __imp_\name
__imp_\name:
__xex_import_slot_\name:
  .long \ordinal
  .endm

  FUNCTION 1, DbgBreakPoint
  FUNCTION 3, DbgPrint
  FUNCTION 9, ExAllocatePool
  FUNCTION 13, ExCreateThread
  FUNCTION 15, ExFreePool
  FUNCTION 16, ExGetXConfigSetting
  FUNCTION 21, ExRegisterTitleTerminateNotification
  FUNCTION 25, ExTerminateThread
  FUNCTION 33, FscSetCacheElementCount
  FUNCTION 34, HalGetCurrentAVPack
  FUNCTION 43, InterlockedFlushSList
  FUNCTION 44, InterlockedPopEntrySList
  FUNCTION 90, KeDelayExecutionThread
  FUNCTION 93, KeEnableFpuExceptions
  FUNCTION 102, KeGetCurrentProcessType
  FUNCTION 116, KeInitializeSemaphore
  FUNCTION 131, KeQueryPerformanceFrequency
  FUNCTION 132, KeQuerySystemTime
  FUNCTION 136, KeReleaseSemaphore
  FUNCTION 143, KeResetEvent
  FUNCTION 157, KeSetEvent
  FUNCTION 176, KeWaitForSingleObject
  FUNCTION 177, KfAcquireSpinLock
  FUNCTION 180, KfReleaseSpinLock
  FUNCTION 186, MmAllocatePhysicalMemoryEx
  FUNCTION 189, MmFreePhysicalMemory
  FUNCTION 190, MmGetPhysicalAddress
  FUNCTION 196, MmQueryAddressProtect
  FUNCTION 198, MmQueryStatistics
  FUNCTION 204, NtAllocateVirtualMemory
  FUNCTION 205, NtCancelTimer
  FUNCTION 207, NtClose
  FUNCTION 210, NtCreateFile
  FUNCTION 219, NtFlushBuffersFile
  FUNCTION 223, NtOpenFile
  FUNCTION 228, NtQueryDirectoryFile
  FUNCTION 231, NtQueryFullAttributesFile
  FUNCTION 232, NtQueryInformationFile
  FUNCTION 239, NtQueryVolumeInformationFile
  FUNCTION 240, NtReadFile
  FUNCTION 247, NtSetInformationFile
  FUNCTION 255, NtWriteFile
  FUNCTION 259, ObCreateSymbolicLink
  FUNCTION 260, ObDeleteSymbolicLink
  FUNCTION 209, NtCreateEvent
  FUNCTION 212, NtCreateMutant
  FUNCTION 215, NtCreateTimer
  FUNCTION 220, NtFreeVirtualMemory
  FUNCTION 238, NtQueryVirtualMemory
  FUNCTION 242, NtReleaseMutant
  FUNCTION 245, NtResumeThread
  FUNCTION 246, NtSetEvent
  FUNCTION 250, NtSetTimerEx
  FUNCTION 253, NtWaitForSingleObjectEx
  FUNCTION 254, NtWaitForMultipleObjectsEx
  FUNCTION 272, ObReferenceObjectByHandle
  FUNCTION 261, ObDereferenceObject
  FUNCTION 299, RtlImageXexHeaderField
  FUNCTION 293, RtlEnterCriticalSection
  FUNCTION 294, RtlFillMemoryUlong
  FUNCTION 295, RtlFreeAnsiString
  FUNCTION 300, RtlInitAnsiString
  FUNCTION 302, RtlInitializeCriticalSection
  FUNCTION 304, RtlLeaveCriticalSection
  FUNCTION 307, RtlMultiByteToUnicodeN
  FUNCTION 309, RtlNtStatusToDosError
  FUNCTION 315, sprintf
  FUNCTION 321, RtlTryEnterCriticalSection
  FUNCTION 322, RtlUnicodeStringToAnsiString
  FUNCTION 323, RtlUnicodeToMultiByteN
  FUNCTION 333, _vsnprintf
  FUNCTION 338, KeTlsAlloc
  FUNCTION 340, KeTlsGetValue
  FUNCTION 341, KeTlsSetValue
  FUNCTION 404, XexCheckExecutablePrivilege
  FUNCTION 405, XexGetModuleHandle
  FUNCTION 406, XexGetModuleSection
  FUNCTION 407, XexGetProcedureAddress
  FUNCTION 438, VdEnableRingBufferRPtrWriteBack
  FUNCTION 451, VdInitializeRingBuffer
  FUNCTION 469, VdSetGraphicsInterruptCallback
  FUNCTION 499, XAudioRegisterRenderDriverClient
  FUNCTION 500, XAudioUnregisterRenderDriverClient
  FUNCTION 501, XAudioSubmitRenderDriverFrame
  FUNCTION 503, XAudioGetVoiceCategoryVolumeChangeMask
  FUNCTION 504, XAudioGetVoiceCategoryVolume
  FUNCTION 603, VdSwap
  FUNCTION 458, VdQueryVideoMode
  FUNCTION 971, XGetAVPack, 1
  FUNCTION 972, XGetGameRegion, 1
  FUNCTION 977, XGetVideoMode, 1
  FUNCTION 1, NetDll_WSAStartup, 1
  FUNCTION 51, NetDll_XNetStartup, 1
  FUNCTION 423, XamLoaderGetLaunchDataSize, 1
  FUNCTION 424, XamLoaderGetLaunchData, 1
  FUNCTION 490, XamAlloc, 1
  FUNCTION 492, XamFree, 1
  FUNCTION 500, XMsgInProcessCall, 1
  FUNCTION 503, XMsgStartIORequest, 1
  FUNCTION 508, XMsgStartIORequestEx, 1
  FUNCTION 642, XamGetSystemVersion, 1
  FUNCTION 522, XamUserGetXUID, 1
  FUNCTION 526, XamUserGetName, 1
  FUNCTION 528, XamUserGetSigninState, 1
  FUNCTION 537, XamUserReadProfileSettings, 1
  FUNCTION 650, XamNotifyCreateListener, 1
  FUNCTION 651, XNotifyGetNext, 1
  FUNCTION 652, XNotifyPositionUI, 1
  VARIABLE 27, ExThreadObjectType
  VARIABLE 173, KeTimeStampBundle
  VARIABLE 344, XboxKrnlVersion
  VARIABLE 403, XexExecutableModuleHandle
  VARIABLE 430, ExLoadedCommandLine
  VARIABLE 446, VdGlobalDevice
  VARIABLE 448, VdGpuClockInMHz
  VARIABLE 449, VdHSIOCalibrationLock
