#pragma once
// Generators for tests/lift_{x87,int,sse}_test.cpp; see
// x87test.cpp.

#include <string>

#include "program.h"

namespace lifter {
class Decoder;
int WriteX87Test(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue);
int WriteIntTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue);
int WriteSseTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue);
// The same forms from a list of encodings instead of an image (one
// instruction per line as hex bytes, '#' to the end of the line a
// comment) or, with `code`, from flat machine code (a fixture linked to a
// raw binary: instructions only, decoded from the first byte to the last).
enum class FormKind { X87, Int, Sse };
int WriteFormListTest(const std::string& path, bool code, bool x64, const Decoder& dec,
                      const std::string& out, const char* prologue, FormKind kind);
}  // namespace lifter
