#pragma once
// The NID an Orbis symbol is imported and exported by: the first 8 bytes
// of SHA-1(name + a fixed 16-byte suffix), byte-reversed, in base64 with
// '-' for '/', 11 characters ("printf" is "hcuQgD53UxM").

#include <string>
#include <string_view>

namespace recomp::orbis {

std::string Nid(std::string_view name);

}  // namespace recomp::orbis
