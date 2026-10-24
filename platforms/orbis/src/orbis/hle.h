#pragma once
// HLE handlers for Orbis system libraries. A handler is registered under
// its library and symbol name, as a Windows one is under its DLL and
// export name (runtime/hle/hle.h); imports name it by NID, which the
// loader (orbis/process.h) maps back to the registered name, so traces
// and fatal errors show names. Handlers are x64 code's and take System V
// arguments (Arg: the i-th integer one, ArgD/ArgF: the i-th float one).

#include <string>

#include "runtime/hle/hle.h"

namespace recomp::orbis {

struct NidName {
  NidName(const char* library, const char* name);
};

// The name registered for library!nid, or null.
const std::string* NameOfNid(const std::string& library, const std::string& nid);

}  // namespace recomp::orbis

// ORBIS_HLE(library, name, nargs) { ... return rax; }  for C-identifier
// names; nargs is the number of integer arguments traces show.
#define ORBIS_HLE(lib, name, nargs) ORBIS_HLE_NAMED(lib, #name, name, nargs)
// ORBIS_HLE_NAMED(library, "_ZN...", ident, nargs) for mangled names.
#define ORBIS_HLE_NAMED(lib, sym, ident, nargs)                            \
  static ::recomp::orbis::NidName RECOMP_HLE_CAT(nid_, ident){lib, sym}; \
  HLE_NAMED_X(lib, sym, ident, nargs, true)
// ORBIS_DATA(library, "name", ident, size, init): a data export of `size`
// bytes, set up by init(gaddr) (or zero) when first imported.
#define ORBIS_DATA(lib, sym, ident, size, init)                            \
  static ::recomp::orbis::NidName RECOMP_HLE_CAT(nid_, ident){lib, sym}; \
  static ::recomp::hle::DataExport RECOMP_HLE_CAT(data_, ident){lib, sym, size, init}
