// xbox360_xex_decrypt: writes an encrypted XEX2's unencrypted copy, for the
// lifter and the loader, which read only those.
//   xbox360_xex_decrypt <in.xex> <out.xex> --key-file <file>
// The key file holds the 16-byte key the console decrypts retail (or
// development) XEX session keys with, as raw bytes or 32 hex digits. It
// is the owner's: it never comes into this tree, and nothing here knows it.
//
// The session key is the security info's AES key decrypted with that key
// (AES-128 ECB); the data after the headers is AES-128 CBC under the
// session key with a zero IV. The copy differs from the input only in that
// data and in the file format info's encryption type (now 0); the headers'
// signatures no longer match it, which nothing here checks.

#include <openssl/evp.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t Be32(const std::vector<uint8_t>& d, size_t off) {
  return uint32_t(d[off]) << 24 | uint32_t(d[off + 1]) << 16 | uint32_t(d[off + 2]) << 8 | d[off + 3];
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool ReadKey(const std::string& path, uint8_t key[16]) {
  std::vector<uint8_t> raw;
  if (!ReadFile(path, raw)) return false;
  if (raw.size() == 16) {
    std::memcpy(key, raw.data(), 16);
    return true;
  }
  std::string hex;
  for (uint8_t c : raw)
    if (std::isxdigit(c)) hex += char(c);
  if (hex.size() != 32) return false;
  for (int k = 0; k < 16; k++) key[k] = uint8_t(std::stoi(hex.substr(2 * k, 2), nullptr, 16));
  return true;
}

bool Aes(const EVP_CIPHER* cipher, const uint8_t* key, const uint8_t* in, size_t n, uint8_t* out) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  uint8_t iv[16] = {};
  int len = 0, fin = 0;
  bool ok = c && EVP_DecryptInit_ex(c, cipher, nullptr, key, iv) == 1 &&
            EVP_CIPHER_CTX_set_padding(c, 0) == 1 &&
            EVP_DecryptUpdate(c, out, &len, in, int(n)) == 1 &&
            EVP_DecryptFinal_ex(c, out + len, &fin) == 1;
  EVP_CIPHER_CTX_free(c);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::string in, out, key_file;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--key-file" && i + 1 < argc) key_file = argv[++i];
    else if (in.empty()) in = a;
    else if (out.empty()) out = a;
  }
  if (in.empty() || out.empty() || key_file.empty()) {
    std::fprintf(stderr, "usage: xbox360_xex_decrypt in.xex out.xex --key-file file\n");
    return 2;
  }
  std::vector<uint8_t> x;
  uint8_t key[16];
  if (!ReadFile(in, x) || x.size() < 24 || std::memcmp(x.data(), "XEX2", 4)) {
    std::fprintf(stderr, "xbox360_xex_decrypt: %s is not a XEX2 file\n", in.c_str());
    return 1;
  }
  if (!ReadKey(key_file, key)) {
    std::fprintf(stderr, "xbox360_xex_decrypt: %s holds no 16-byte key\n", key_file.c_str());
    return 1;
  }
  uint32_t header_size = Be32(x, 8), security = Be32(x, 16), count = Be32(x, 20), ffi = 0;
  for (uint32_t k = 0; k < count; k++)
    if (Be32(x, 24 + 8 * k) == 0x000003FF) ffi = Be32(x, 28 + 8 * k);
  if (!ffi || header_size > x.size() || security + 0x160 > x.size()) {
    std::fprintf(stderr, "xbox360_xex_decrypt: malformed headers\n");
    return 1;
  }
  if ((x[ffi + 4] << 8 | x[ffi + 5]) == 0) {
    std::fprintf(stderr, "xbox360_xex_decrypt: %s is not encrypted\n", in.c_str());
    return 1;
  }
  uint8_t session[16];
  size_t n = (x.size() - header_size) & ~size_t(15);
  std::vector<uint8_t> plain(n);
  if (!Aes(EVP_aes_128_ecb(), key, &x[security + 0x150], 16, session) ||
      !Aes(EVP_aes_128_cbc(), session, &x[header_size], n, plain.data())) {
    std::fprintf(stderr, "xbox360_xex_decrypt: decryption failed\n");
    return 1;
  }
  std::memcpy(&x[header_size], plain.data(), n);
  x[ffi + 4] = x[ffi + 5] = 0;
  std::ofstream f(out, std::ios::binary);
  f.write(reinterpret_cast<const char*>(x.data()), std::streamsize(x.size()));
  if (!f) {
    std::fprintf(stderr, "xbox360_xex_decrypt: cannot write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
