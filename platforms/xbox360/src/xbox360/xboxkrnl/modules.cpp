// xboxkrnl: the title's module (Xex*) and its XEX headers.

#include <cstring>

#include "ppc/fatal.h"
#include "xbox360/fs.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";
// Optional headers: the resources (name[8], address, size each, after the
// data's size) and the system flags (privileges, the value itself).
constexpr uint32_t kXexResourceInfo = 0x000002FF, kXexSystemFlags = 0x00030000;

uint32_t ExecutableHeaders() { return Rd32(ExecutableModule() + kLdrXexHeader); }

void InitExecutableModuleHandle(uint32_t va) { Wr32(va, ExecutableModule()); }

}  // namespace

// The title's loader entry, which titles and xam read for its XEX headers.
HLE_VARIABLE(kKrnl, 403, XexExecutableModuleHandle, 4, InitExecutableModuleHandle);

// RtlImageXexHeaderField(headers, key).
HLE(kKrnl, 299, RtlImageXexHeaderField) { return XexHeaderField(Arg(c, 0), Arg(c, 1)); }

// XexCheckExecutablePrivilege(privilege): the privilege's bit in the
// title's system flags.
HLE(kKrnl, 404, XexCheckExecutablePrivilege) {
  uint32_t privilege = Arg(c, 0);
  if (privilege >= 32) ppc::Fatal(&c, "XexCheckExecutablePrivilege(%u): only the first 32 are read", privilege);
  return XexHeaderField(ExecutableHeaders(), kXexSystemFlags) >> privilege & 1;
}

// XexGetModuleHandle(name, *handle): the title's module for no name or the
// name it was started as. An NTSTATUS, which xam's callers test for < 0;
// the status of a module that is not loaded is chosen (docs/xboxkrnl.md).
HLE(kKrnl, 405, XexGetModuleHandle) {
  uint32_t name_ptr = Arg(c, 0), out = Arg(c, 1);
  if (name_ptr) {
    std::string name = ReadString(name_ptr);
    if (SameName(name, "xboxkrnl.exe") || SameName(name, "xam.xex"))
      ppc::Fatal(&c, "XexGetModuleHandle(%s): the system modules have no loader entries yet", name.c_str());
    if (!SameName(name, ExecutableModuleName())) {
      Wr32(out, 0);
      return kStatusDllNotFound;
    }
  }
  Wr32(out, ExecutableModule());
  return kStatusSuccess;
}

// XexGetModuleSection(handle, name, *data, *size): one of the module's
// resources (its XEX's resource info), by its exact name; the statuses of
// failures are Xenia's.
HLE(kKrnl, 406, XexGetModuleSection) {
  if (Arg(c, 0) != ExecutableModule()) return kStatusInvalidHandle;
  std::string name = ReadString(Arg(c, 1));
  uint32_t info = XexHeaderField(ExecutableHeaders(), kXexResourceInfo);
  if (!info) return kStatusNotFound;
  for (uint32_t r = info + 4; r + 16 <= info + Rd32(info); r += 16) {
    const char* res = reinterpret_cast<const char*>(Mem() + r);
    if (name != std::string(res, strnlen(res, 8))) continue;
    Wr32(Arg(c, 2), Rd32(r + 8));
    Wr32(Arg(c, 3), Rd32(r + 12));
    return kStatusSuccess;
  }
  return kStatusNotFound;
}

// XexGetProcedureAddress(handle, ordinal, *address): an export of the
// title's module (handle 0 or its own). XEX exports are by ordinal; a
// value above 0xFFFF names one by a string, which nothing here reads yet.
HLE(kKrnl, 407, XexGetProcedureAddress) {
  uint32_t handle = Arg(c, 0), ordinal = Arg(c, 1), out = Arg(c, 2);
  if (handle && handle != ExecutableModule()) return kStatusInvalidHandle;
  if (ordinal > 0xFFFF)
    ppc::Fatal(&c, "XexGetProcedureAddress(%s): exports by name are not read", ReadString(ordinal).c_str());
  uint32_t va = ExecutableExport(ordinal);
  Wr32(out, va);
  return va ? kStatusSuccess : kStatusDriverEntrypointNotFound;
}

}  // namespace recomp::xbox360
