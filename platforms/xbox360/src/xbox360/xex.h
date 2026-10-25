#pragma once
// XEX2, the Xbox 360's executable format: the headers, the image (stored
// plain, "basic" compressed as runs of data and zeros, or "normal"
// compressed with LZX) and the PE image inside it (sections, .pdata, the
// import libraries and their thunks). Only unencrypted XEX files are read:
// tools/xex_decrypt turns an encrypted one into one with the owner's key,
// which never comes into this tree.

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::xbox360 {

struct XexSection {
  std::string name;
  uint32_t va = 0, size = 0;  // guest VA and virtual size
  bool code = false;
};

// One entry of .pdata: a function's start and its length in bytes.
struct XexFunction {
  uint32_t va = 0, size = 0;
};

// One record of an import library. Each import has a variable record (its
// slot, which the loader fills) and functions also a thunk record (the
// code the title calls).
struct XexImport {
  std::string module;  // e.g. "xboxkrnl.exe"
  uint32_t ordinal = 0;
  uint32_t va = 0;     // the slot or the thunk
  bool thunk = false;
};

// Static thread-local storage (__declspec(thread)): each thread gets
// `data_size` bytes, the first `raw_size` copied from `raw_data` (a VA),
// and `slots` dynamic slots (KeTlsAlloc).
struct XexTls {
  uint32_t slots = 0, raw_data = 0, data_size = 0, raw_size = 0;
};

struct XexModule {
  uint32_t base = 0;        // the image's load address
  uint32_t entry = 0;
  uint32_t title_id = 0;    // 0 for system modules
  uint32_t module_flags = 0;
  uint32_t stack_size = 0;  // the main thread's, 0 if the XEX does not say
  XexTls tls;
  std::string original_name;
  std::vector<uint8_t> image;  // the loaded image, `base` at offset 0
  std::vector<XexSection> sections;
  std::vector<XexFunction> functions;
  std::vector<XexImport> imports;
  // Exports by ordinal (system modules): the table the XEX security info
  // points at. Function and variable exports alike; 0 where none.
  std::vector<uint32_t> exports;
  uint32_t export_base = 0;  // the ordinal of exports[0]
};

// TextHash() of the code sections in address order: what recomp_lift_ppc
// stores as kLiftedTextHash for this module.
uint64_t TextHash(const XexModule& m);

// True if the file starts with the XEX2 magic.
bool IsXex(const std::string& path);
bool LoadXex(const std::string& path, XexModule& m, std::string& err);
// The same over a XEX already in memory.
bool ParseXex(const std::vector<uint8_t>& file, XexModule& m, std::string& err);

}  // namespace recomp::xbox360
