// The test title's imports, as a title's are: a slot per import (the
// loader writes the export's address into it) and, for a function, the
// thunk the code calls. xbox360_elf2xex finds the records by their
// __xex_import symbols; library 0 is xboxkrnl.exe.
  .macro FUNCTION ordinal, name
  .section .data.imports, "aw"
  .p2align 2
  .globl __imp_\name
__imp_\name:
__xex_import_slot_\name:
  .long \ordinal
  .text
  .p2align 4
  .globl \name
\name:
__xex_import_thunk_\name:
  .long 0x01000000 | \ordinal
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
  FUNCTION 25, ExTerminateThread
  FUNCTION 43, InterlockedFlushSList
  FUNCTION 44, InterlockedPopEntrySList
  FUNCTION 90, KeDelayExecutionThread
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
  FUNCTION 204, NtAllocateVirtualMemory
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
  FUNCTION 220, NtFreeVirtualMemory
  FUNCTION 238, NtQueryVirtualMemory
  FUNCTION 242, NtReleaseMutant
  FUNCTION 245, NtResumeThread
  FUNCTION 246, NtSetEvent
  FUNCTION 253, NtWaitForSingleObjectEx
  FUNCTION 254, NtWaitForMultipleObjectsEx
  FUNCTION 272, ObReferenceObjectByHandle
  FUNCTION 261, ObDereferenceObject
  FUNCTION 299, RtlImageXexHeaderField
  FUNCTION 293, RtlEnterCriticalSection
  FUNCTION 294, RtlFillMemoryUlong
  FUNCTION 300, RtlInitAnsiString
  FUNCTION 302, RtlInitializeCriticalSection
  FUNCTION 304, RtlLeaveCriticalSection
  FUNCTION 315, sprintf
  FUNCTION 321, RtlTryEnterCriticalSection
  FUNCTION 333, _vsnprintf
  FUNCTION 338, KeTlsAlloc
  FUNCTION 340, KeTlsGetValue
  FUNCTION 341, KeTlsSetValue
  FUNCTION 404, XexCheckExecutablePrivilege
  FUNCTION 405, XexGetModuleHandle
  FUNCTION 406, XexGetModuleSection
  FUNCTION 407, XexGetProcedureAddress
  VARIABLE 27, ExThreadObjectType
  VARIABLE 173, KeTimeStampBundle
  VARIABLE 344, XboxKrnlVersion
  VARIABLE 403, XexExecutableModuleHandle
