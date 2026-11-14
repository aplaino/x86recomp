// hle_dup_test: a second registration of one import is fatal at load
// (x86/hle.cpp), whichever library registers it; the test passes when
// the runtime says so.

#include <cstdio>

#include "x86/hle.h"

HLE("test.dll", Twice, 0) { return 1; }
HLE_NAMED("test.dll", "Twice", Twice_again, 0) { return 2; }

int main() {
  std::printf("both registrations were accepted\n");
  return 1;
}
