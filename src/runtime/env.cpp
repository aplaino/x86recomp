// runtime/env.h. In the logging library, which everything links.

#include "runtime/env.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace recomp {
namespace {
std::string g_prefix;  // "<ID>_"
}  // namespace

void SetEnvId(const std::string& id) {
  g_prefix.clear();
  for (char ch : id) g_prefix += char(std::toupper(uint8_t(ch)));
  g_prefix += '_';
}

const char* Env(const char* name) {
  if (g_prefix.empty()) return nullptr;
  return std::getenv((g_prefix + name).c_str());
}

}  // namespace recomp
