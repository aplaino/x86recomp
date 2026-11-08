// recomp::orbis::Main (orbis/process.h): a game executable's main().

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "orbis/process.h"

namespace recomp::orbis {

int Main(const GameConfig& game, int argc, char** argv) {
  std::string game_dir, sfo;
  std::vector<std::string> game_args;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--game-dir") && i + 1 < argc) {
      game_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--param-sfo") && i + 1 < argc) {
      sfo = argv[++i];
    } else if (!std::strcmp(argv[i], "--")) {
      game_args.assign(argv + i + 1, argv + argc);
      break;
    } else {
      game_dir.clear();
      break;
    }
  }
  if (game_dir.empty()) {
    std::fprintf(stderr, "usage: %s --game-dir APP0 [--param-sfo FILE] [-- GAME ARGS...]\n",
                 argv[0]);
    return 2;
  }
  while (game_dir.size() > 1 && game_dir.back() == '/') game_dir.pop_back();

  Config config;
  std::vector<AppModule> modules;
  std::string err;
  if (!AppModules(game_dir, &modules, &err)) {
    std::fprintf(stderr, "%s: %s\n", argv[0], err.c_str());
    return 2;
  }
  config.eboot = modules[0].path;
  for (size_t i = 1; i < modules.size(); i++)
    (modules[i].start ? config.prxs : config.loadable).push_back({modules[i].path, modules[i].base});
  struct stat st;
  if (sfo.empty() && stat((game_dir + "/sce_sys/param.sfo").c_str(), &st) == 0)
    sfo = game_dir + "/sce_sys/param.sfo";
  if (!sfo.empty() && !ReadParamSfo(sfo, &config.param, &err)) {
    std::fprintf(stderr, "%s: %s\n", argv[0], err.c_str());
    return 2;
  }
  config.args = {"/app0/eboot.bin"};
  config.args.insert(config.args.end(), game_args.begin(), game_args.end());
  return RunProcess(game, game_dir, config);
}

}  // namespace recomp::orbis
