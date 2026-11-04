#pragma once
// A game's application directory (/app0) as the Orbis loader and the
// system read it: the modules the game starts with, and its param.sfo.
// The lifter's lift script (tools/lift.sh here) and the loader take the
// modules and their bases from the same place, so they agree.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runtime/guest_memory.h"

namespace recomp::orbis {

// Where the first PRX goes; each next one at the next multiple of
// kPrxAlign past the end of the one before.
inline constexpr gaddr kPrxBase = 0x800000000, kPrxAlign = 0x10000000;

struct AppModule {
  std::string path;  // host path
  std::string name;  // its module name ("eboot" for the eboot)
  gaddr base;        // 0 for the eboot (its own address)
};

// The eboot (app0/eboot.bin), then the PRXs the game ships in
// app0/sce_module that it needs at start (DT_SCE_NEEDED_MODULE, directly
// or through each other), in initialisation order: each after the modules
// it needs, else in the order they are first named. Needed modules the
// game does not ship are system libraries (the HLE). `shipped` (if
// given) gets every PRX in sce_module by module name, those not started
// too.
bool StartModules(const std::string& app0, std::vector<AppModule>* out, std::string* err,
                  std::map<std::string, std::string>* shipped = nullptr);

// param.sfo: the application's parameters (TITLE_ID, APP_VER, ...), as
// UTF-8 strings and integers.
struct ParamSfo {
  std::map<std::string, std::string> strings;
  std::map<std::string, uint32_t> ints;
  std::string Str(const std::string& key) const {
    auto it = strings.find(key);
    return it != strings.end() ? it->second : std::string();
  }
};

bool ReadParamSfo(const std::string& path, ParamSfo* out, std::string* err);

}  // namespace recomp::orbis
