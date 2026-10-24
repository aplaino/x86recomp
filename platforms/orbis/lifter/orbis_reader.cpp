// recomp_lift's reader for Orbis ELFs (orbis/elf_image.h): function
// entries from .eh_frame, the entry point, DT_INIT/DT_FINI, exports and
// PLT stubs (named by library and NID); noreturn imports by NID.

#include <algorithm>

#include "orbis/elf_image.h"
#include "orbis/nid.h"
#include "program.h"

namespace lifter {
namespace {

bool Match(const std::string& path, bool* x64) {
  *x64 = true;
  return recomp::orbis::ElfImage::IsElfFile(path);
}

bool Load(Program& p, Module& m, const std::string& path, uint64_t base, std::string* err) {
  recomp::orbis::ElfImage img;
  if (!img.Load(path, p.mem, err, base)) return false;
  m.base = img.image_base + img.link_base;
  m.preferred_base = img.link_base;
  // The executable segment also holds .rodata and .eh_frame, after the
  // code: .text ends with the last FDE's function or PLT stub.
  for (const auto& s : img.segments) {
    if (!(s.flags & 1)) {
      m.data.push_back({s.va, s.va + s.size});
    } else if (m.text_hi) {
      *err = "more than one executable segment";
      return false;
    } else {
      m.text_lo = s.va;
      for (const auto& fn : img.functions)
        if (fn.begin >= s.va && fn.end <= s.va + s.size) m.text_hi = std::max(m.text_hi, fn.end);
      for (const auto& imp : img.imports)
        if (imp.plt) m.text_hi = std::max(m.text_hi, imp.plt + 16);
      m.rodata.push_back({m.text_hi, s.va + s.size});
    }
  }
  auto in_text = [&](uint64_t va) { return va >= m.text_lo && va < m.text_hi; };
  for (const auto& fn : img.functions)
    if (in_text(fn.begin)) p.entries.insert(fn.begin);
  for (const auto& e : img.exports)
    if (e.type == 2 && in_text(e.va)) p.entries.insert(e.va);
  const std::pair<uint64_t, const char*> named[] = {
      {img.entry, "_start"}, {img.init, "_init"}, {img.fini, "_fini"}};
  for (const auto& [va, name] : named)
    if (va && in_text(va)) {
      p.entries.insert(va);
      p.names.emplace(va, name);
    }
  for (const auto& imp : img.imports)
    if (imp.plt) {
      p.entries.insert(imp.plt);
      p.names.emplace(imp.plt, imp.library + " " + imp.nid);
    }

  // Imports that never return: the C and POSIX exits and longjmps, the
  // stack protector's failure handler, the C++ runtime's throws (the
  // Itanium ABI's and the library's std::_X* helpers) and terminate, and
  // the Orbis pthread_exit.
  static const char* const kNoreturn[] = {
      "exit", "_exit", "_Exit", "abort", "quick_exit", "longjmp", "_longjmp", "siglongjmp",
      "pthread_exit", "scePthreadExit", "__stack_chk_fail", "__cxa_throw", "__cxa_rethrow",
      "__cxa_bad_cast", "__cxa_bad_typeid", "__cxa_pure_virtual", "_Unwind_Resume",
      "_ZSt9terminatev", "_ZSt11_Xbad_allocv", "_ZSt14_Xlength_errorPKc",
      "_ZSt14_Xout_of_rangePKc", "_ZSt15_Xruntime_errorPKc", "_ZSt16_Xoverflow_errorPKc",
      "_ZSt18_Xinvalid_argumentPKc", "_ZSt19_Xbad_function_callv",
      "_ZSt13_Xregex_errorNSt15regex_constants10error_typeE"};
  for (const auto& imp : img.imports)
    for (const char* name : kNoreturn)
      if (imp.nid == recomp::orbis::Nid(name)) {
        p.noreturn_iat.insert(imp.slots.begin(), imp.slots.end());
        if (imp.plt) p.noreturn.insert(imp.plt);
      }
  return true;
}

const bool kRegistered = (RegisterImageReader({Match, Load}), true);

}  // namespace
}  // namespace lifter
