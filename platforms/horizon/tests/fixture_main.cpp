// The lifted test fixture: horizon_fixture <fixture.nro>.
#include <cstdio>

#include "horizon/process.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: horizon_fixture <fixture.nro>\n");
    return 2;
  }
  return recomp::horizon::RunProcess({"NXFIXTURE", argv[1]});
}
