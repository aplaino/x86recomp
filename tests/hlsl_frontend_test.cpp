// The HLSL translator's front end (hlsl::Compile) on edge cases: sources
// it must translate (vector<T, n> and matrix<T, r, c>, a struct cast to
// itself, (S)0) and sources it must refuse with an error rather than a
// crash, a hang or a wrong translation (an unterminated attribute,
// semantic and sampler indices past 32 bits, (S)1, nesting deep enough
// to exhaust the stack, #if dividing INT64_MIN by -1).

#include <cstdio>
#include <string>

#include "runtime/hlsl/hlsl.h"

namespace {

int failures = 0;

bool Translate(const std::string& src, std::string* glsl, std::string* err) {
  recomp::hlsl::Program p;
  bool ok = recomp::hlsl::Compile(src, {}, "main", "ps_3_0", 0, &p, err);
  *glsl = p.glsl;
  return ok;
}

void Good(const char* what, const std::string& src) {
  std::string glsl, err;
  if (!Translate(src, &glsl, &err)) {
    std::printf("FAIL %s: %s\n", what, err.c_str());
    failures++;
  }
}

void Bad(const char* what, const std::string& src) {
  std::string glsl, err;
  if (Translate(src, &glsl, &err)) {
    std::printf("FAIL %s: translated\n", what);
    failures++;
  } else {
    std::printf("ok %s: %s\n", what, err.c_str());
  }
}

}  // namespace

int main() {
  Good("vector and matrix templates",
       "float4 main(float4 p : TEXCOORD0) : COLOR {\n"
       "  vector<float, 2> v = p.xy;\n"
       "  matrix<float, 2, 2> m = float2x2(1, 0, 0, 1);\n"
       "  return float4(mul(v, m), 0, 1);\n"
       "}\n");
  const std::string with_struct =
      "struct S { float4 a; };\n"
      "float4 main(float4 p : TEXCOORD0) : COLOR {\n"
      "  S s;\n"
      "  s.a = p;\n";
  std::string cast, plain, err;
  if (!Translate(with_struct + "  S t = (S)s;\n  return t.a;\n}\n", &cast, &err) ||
      !Translate(with_struct + "  S t = s;\n  return t.a;\n}\n", &plain, &err) || cast != plain) {
    std::printf("FAIL a struct cast to itself is not the struct (%s)\n", err.c_str());
    failures++;
  }
  Good("(S)0", with_struct + "  S t = (S)0;\n  return t.a;\n}\n");
  Bad("(S)1", with_struct + "  S t = (S)1;\n  return t.a;\n}\n");
  Bad("unterminated attribute", "float4 main() : COLOR {\n  [unroll(4\n");
  Bad("semantic index past 32 bits",
      "float4 main(float4 p : TEXCOORD99999999999999999999) : COLOR { return p; }\n");
  Bad("sampler index past 32 bits",
      "sampler s[2];\n"
      "float4 main(float2 t : TEXCOORD0) : COLOR { return tex2D(s[99999999999999999999], t); }\n");
  const int deep = 100000;
  Bad("parentheses nested deeply", "float4 main() : COLOR { return " + std::string(deep, '(') +
                                       "1" + std::string(deep, ')') + "; }\n");
  Bad("negations nested deeply",
      "float4 main() : COLOR { return " + std::string(deep, '-') + "1; }\n");
  Bad("blocks nested deeply", "float4 main() : COLOR { " + std::string(deep, '{') +
                                  std::string(deep, '}') + " return 0; }\n");
  Bad("#if nested deeply",
      "#if " + std::string(deep, '(') + "1" + std::string(deep, ')') + "\n#endif\n");
  Good("#if INT64_MIN / -1",
       "#if (-9223372036854775807 - 1) / -1 && (-9223372036854775807 - 1) % -1 == 0\n"
       "#endif\n"
       "float4 main() : COLOR { return 0; }\n");
  if (failures) return 1;
  std::printf("ok\n");
  return 0;
}
