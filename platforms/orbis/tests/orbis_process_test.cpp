// orbis_process_test: tests/e2e's eboot and PRX (C built for FreeBSD
// x86-64, made SCE ELFs by tools/orbis_sce_elf.py) lifted and run through
// the Orbis loader: imports bound by NID to the HLE and across modules,
// data imports both ways, static TLS and the TCB through fs, general-
// dynamic TLS (__tls_get_addr), the PRX's _init before _start, _start's
// arguments, a switch table and System V argument order; then libkernel's
// HLE (e2e/kernel.c) and other libraries' (e2e/system.c). Here is libTest,
// the HLE the eboot imports.
//
// usage: orbis_process_test eboot.elf prx.elf

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "orbis/hle.h"
#include "orbis/process.h"

namespace {

int g_checks = 0, g_failed = 0;

bool Report(int id, bool ok) {
  g_checks++;
  if (!ok) {
    std::printf("FAIL: check %d\n", id);
    g_failed++;
  }
  return !ok;
}

void InitTestValue(::recomp::gaddr a) { ::recomp::Wr32(a, 1234); }

}  // namespace

ORBIS_HLE("libTest", test_check, 3) {
  using ::recomp::hle::Arg;
  const int64_t got = int64_t(Arg(c, 1)), want = int64_t(Arg(c, 2));
  if (got != want)
    std::printf("check %d: got %lld, want %lld\n", int(Arg(c, 0)), (long long)got,
                (long long)want);
  return Report(int(Arg(c, 0)), got == want);
}

// test_double(int id, double d, long n): id and n are the first and
// second integer arguments, d the first float one.
ORBIS_HLE("libTest", test_double, 3) {
  using ::recomp::hle::Arg;
  return Report(int(Arg(c, 0)), ::recomp::hle::ArgD(c, 0) == 2.5 && Arg(c, 1) == 3);
}

ORBIS_DATA("libTest", "test_value", test_value, 4, InitTestValue);

int main(int argc, char** argv) {
  if (argc != 3) {
    std::printf("usage: orbis_process_test eboot.elf prx.elf\n");
    return 2;
  }
  recomp::GameConfig game;
  game.id = "orbis_test";
  recomp::orbis::Config config;
  config.eboot = argv[1];
  config.prxs = {{argv[2], 0x800000000}};
  config.args = {"eboot", "hello"};
  // The game directory (/app0): a file, and a directory of two.
  const auto dir = std::filesystem::temp_directory_path() / "orbis_process_test";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "data.txt") << "hello\n";
  std::filesystem::create_directories(dir / "sub");
  std::ofstream(dir / "sub" / "b.txt") << "b";
  std::ofstream(dir / "sub" / "a.txt") << "a";
  // The per-user data (save data) in a directory of its own.
  setenv("XDG_DATA_HOME", (dir / "data").c_str(), 1);
  const int r = recomp::orbis::RunProcess(game, dir.string(), config);
  std::filesystem::remove_all(dir);
  constexpr int kChecks = 23 + 23 + 27 + 18 + 7 + 6 + 7 + 32 + 3;
  std::printf("%d checks, %d failed, _start returned %d\n", g_checks, g_failed, r);
  const bool ok = r == 0 && g_failed == 0 && g_checks == kChecks;
  std::printf("%s\n", ok ? "orbis_process: ok" : "orbis_process: FAILED");
  return ok ? 0 : 1;
}
