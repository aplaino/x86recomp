// The runtime test title (title/), started as a game project starts its
// title: lifted, then run on the runtime with the kernel's HLE module.
//   xbox360_title <title.xex>

#include <cstdio>

#include "xbox360/process.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: xbox360_title title.xex\n");
    return 2;
  }
  recomp::xbox360::RunProcess({"XBOX360_TITLE", argv[1]});
}
