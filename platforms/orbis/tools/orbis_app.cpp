// orbis_app: what the platform makes of a game's app0, without running it.
//
//   orbis_app modules APP0
//     The game's modules (orbis/app.h): those it starts with, in
//     initialisation order, then those it may load. One per line: base
//     (hex, 0 for the eboot), module name, path, and "start" or "load"
//     (tools/lift.sh lifts them all at these bases).
//   orbis_app imports APP0 [--nid-names FILE] [--all]
//     Every import of those modules and where the loader binds it: to a
//     module's export, to an HLE handler, or to nothing (fatal when
//     called). Per library: imports, bound to modules, handled, missing
//     (and the module exporting it, if only one the game loads exports it:
//     the modules it starts with are bound before, so not to it);
//     then each missing one (named from FILE, symbol names one per line,
//     else by NID) and the modules importing it. --all lists the handled
//     ones too. Exit code 1 if any is missing.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "orbis/app.h"
#include "orbis/elf_image.h"
#include "orbis/hle.h"
#include "orbis/nid.h"

namespace {

using namespace recomp::orbis;

int Usage() {
  std::fprintf(stderr,
               "usage: orbis_app modules APP0\n"
               "       orbis_app imports APP0 [--nid-names FILE] [--all]\n");
  return 2;
}

int Imports(const std::vector<AppModule>& modules, const std::string& names_file, bool all) {
  std::map<std::string, std::string> names;  // NID -> name
  if (!names_file.empty()) {
    std::ifstream f(names_file);
    if (!f) {
      std::fprintf(stderr, "orbis_app: cannot open %s\n", names_file.c_str());
      return 2;
    }
    for (std::string n; std::getline(f, n);) {
      while (!n.empty() && (n.back() == '\r' || n.back() == ' ')) n.pop_back();
      if (!n.empty()) names.emplace(Nid(n), n);
    }
  }

  std::vector<ElfImage> images(modules.size());
  // (library, NID) exported by the modules started, and by any. The
  // loader binds the started modules' imports before the game loads
  // others, and never again.
  std::set<std::pair<std::string, std::string>> exported_start, exported;
  // Libraries only modules the game loads export.
  std::map<std::string, std::string> later;  // library -> module
  std::set<std::string> now;
  std::string err;
  for (size_t i = 0; i < modules.size(); i++) {
    if (!images[i].Read(modules[i].path, &err, modules[i].base)) {
      std::fprintf(stderr, "orbis_app: %s: %s\n", modules[i].path.c_str(), err.c_str());
      return 2;
    }
    for (const ElfExport& e : images[i].exports) {
      exported.insert({e.library, e.nid});
      if (modules[i].start) {
        exported_start.insert({e.library, e.nid});
        now.insert(e.library);
      } else {
        later.emplace(e.library, modules[i].name);
      }
    }
  }
  for (const std::string& l : now) later.erase(l);

  struct Lib {
    int imports = 0, modules = 0, handled = 0, missing = 0;
  };
  std::map<std::string, Lib> libs;
  // (library, NID) -> importing modules; and whether data.
  std::map<std::pair<std::string, std::string>, std::set<std::string>> missing, handled;
  std::set<std::pair<std::string, std::string>> data;
  for (size_t i = 0; i < modules.size(); i++)
    for (const ElfImport& imp : images[i].imports) {
      const auto key = std::make_pair(imp.library, imp.nid);
      Lib& l = libs[imp.library];
      l.imports++;
      if ((modules[i].start ? exported_start : exported).count(key)) {
        l.modules++;
      } else if (NameOfNid(imp.library, imp.nid)) {
        l.handled++;
        handled[key].insert(modules[i].name);
      } else {
        l.missing++;
        missing[key].insert(modules[i].name);
        if (imp.type == 1) data.insert(key);
      }
    }

  int total[4] = {};
  std::printf("%-32s %7s %7s %7s %7s\n", "library", "imports", "modules", "handled", "missing");
  for (const auto& [name, l] : libs) {
    std::printf("%-32s %7d %7d %7d %7d%s\n", name.c_str(), l.imports, l.modules, l.handled,
                l.missing, later.count(name) ? ("  (exported by " + later[name] + ", loaded by the game)").c_str() : "");
    total[0] += l.imports, total[1] += l.modules, total[2] += l.handled, total[3] += l.missing;
  }
  std::printf("%-32s %7d %7d %7d %7d\n", "total", total[0], total[1], total[2], total[3]);

  auto list = [&](const char* title, const auto& set) {
    if (set.empty()) return;
    std::printf("\n%s:\n", title);
    for (const auto& [key, importers] : set) {
      auto n = names.find(key.second);
      const std::string* reg = NameOfNid(key.first, key.second);
      std::string by;
      for (const std::string& m : importers) by += (by.empty() ? "" : ", ") + m;
      std::printf("  %s %s%s%s  (%s)\n", key.first.c_str(),
                  reg ? reg->c_str() : n != names.end() ? n->second.c_str() : key.second.c_str(),
                  reg || n == names.end() ? "" : (" [" + key.second + "]").c_str(),
                  data.count(key) ? " (data)" : "", by.c_str());
    }
  };
  list("missing", missing);
  if (all) list("handled", handled);
  return total[3] ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string cmd = argv[1], app0 = argv[2];
  std::string names;
  bool all = false;
  for (int i = 3; i < argc; i++) {
    if (cmd == "imports" && !std::strcmp(argv[i], "--nid-names") && i + 1 < argc)
      names = argv[++i];
    else if (cmd == "imports" && !std::strcmp(argv[i], "--all"))
      all = true;
    else
      return Usage();
  }
  std::vector<AppModule> modules;
  std::string err;
  if (!AppModules(app0, &modules, &err)) {
    std::fprintf(stderr, "orbis_app: %s\n", err.c_str());
    return 2;
  }
  if (cmd == "modules") {
    for (const AppModule& m : modules)
      std::printf("%llx\t%s\t%s\t%s\n", (unsigned long long)m.base, m.name.c_str(), m.path.c_str(),
                  m.start ? "start" : "load");
    return 0;
  }
  if (cmd == "imports") return Imports(modules, names, all);
  return Usage();
}
