// Copied from the licence service's C++ SDK (letissier.ie, clients/cpp) and
// kept as close to it as possible, so a diff against upstream shows only the
// Monocypher backend below. The product check the SDK leaves to its caller is
// in ../Licence.cpp.

#include "licence.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(LICENCE_BACKEND_SODIUM)
#include <sodium.h>
#elif defined(LICENCE_BACKEND_MONOCYPHER)
#include <monocypher-ed25519.h>
#else
#include <openssl/evp.h>
#endif

namespace letissier {
namespace {

std::vector<unsigned char> from_hex(const std::string& hex) {
  std::vector<unsigned char> out;

  if (hex.size() % 2 != 0) {
    return out;
  }

  out.reserve(hex.size() / 2);

  for (std::size_t index = 0; index < hex.size(); index += 2) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };

    const int high = nibble(hex[index]);
    const int low = nibble(hex[index + 1]);

    if (high < 0 || low < 0) {
      return {};
    }

    out.push_back(static_cast<unsigned char>((high << 4) | low));
  }

  return out;
}

std::string to_hex(const unsigned char* data, std::size_t length) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);

  for (std::size_t index = 0; index < length; ++index) {
    out.push_back(digits[data[index] >> 4]);
    out.push_back(digits[data[index] & 0x0F]);
  }

  return out;
}

// base64url, no padding.
std::vector<unsigned char> b64url_decode(const std::string& input) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
  };

  std::vector<unsigned char> out;
  int buffer = 0;
  int bits = 0;

  for (const char c : input) {
    const int digit = value(c);

    if (digit < 0) {
      return {};
    }

    buffer = (buffer << 6) | digit;
    bits += 6;

    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
    }
  }

  return out;
}

bool ed25519_verify(const unsigned char* message,
                    std::size_t message_length,
                    const std::vector<unsigned char>& signature,
                    const std::vector<unsigned char>& key) {
  if (signature.size() != 64 || key.size() != 32) {
    return false;
  }

#if defined(LICENCE_BACKEND_SODIUM)
  return crypto_sign_verify_detached(signature.data(), message, message_length, key.data()) == 0;
#elif defined(LICENCE_BACKEND_MONOCYPHER)
  return crypto_ed25519_check(signature.data(), key.data(), message, message_length) == 0;
#else
  EVP_PKEY* pkey =
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size());

  if (pkey == nullptr) {
    return false;
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool ok = false;

  if (ctx != nullptr && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
    ok = EVP_DigestVerify(ctx, signature.data(), signature.size(), message, message_length) == 1;
  }

  if (ctx != nullptr) {
    EVP_MD_CTX_free(ctx);
  }

  EVP_PKEY_free(pkey);
  return ok;
#endif
}

#if defined(LICENCE_BACKEND_MONOCYPHER)
// FIPS 180-4 SHA-256. Monocypher has SHA-512 (Ed25519 needs it) but not this,
// and machine_hash is defined as SHA-256. Checked against the NIST examples and
// against the vectors' own fingerprint/machineHash pair in the test suite.
void sha256_monocypher_backend(const unsigned char* data, std::size_t length, unsigned char out[32]) {
  static const std::uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

  // Message plus 0x80, zero padding, and the 64-bit big-endian bit length.
  std::vector<unsigned char> message(data, data + length);
  message.push_back(0x80);
  while (message.size() % 64 != 56) {
    message.push_back(0x00);
  }
  const std::uint64_t bits = static_cast<std::uint64_t>(length) * 8;
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<unsigned char>((bits >> shift) & 0xFF));
  }

  for (std::size_t block = 0; block < message.size(); block += 64) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const unsigned char* p = &message[block + static_cast<std::size_t>(i) * 4];
      w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
             (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  for (int i = 0; i < 8; ++i) {
    out[i * 4 + 0] = static_cast<unsigned char>(h[i] >> 24);
    out[i * 4 + 1] = static_cast<unsigned char>(h[i] >> 16);
    out[i * 4 + 2] = static_cast<unsigned char>(h[i] >> 8);
    out[i * 4 + 3] = static_cast<unsigned char>(h[i]);
  }
}
#endif

void sha256(const std::string& input, unsigned char out[32]) {
#if defined(LICENCE_BACKEND_SODIUM)
  crypto_hash_sha256(out, reinterpret_cast<const unsigned char*>(input.data()), input.size());
#elif defined(LICENCE_BACKEND_MONOCYPHER)
  sha256_monocypher_backend(reinterpret_cast<const unsigned char*>(input.data()), input.size(), out);
#else
  unsigned int length = 0;
  EVP_Digest(input.data(), input.size(), out, &length, EVP_sha256(), nullptr);
#endif
}

std::string trim(const std::string& input) {
  const auto begin = std::find_if_not(input.begin(), input.end(),
                                      [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(input.rbegin(), input.rend(),
                                    [](unsigned char c) { return std::isspace(c); })
                       .base();

  return begin < end ? std::string(begin, end) : std::string();
}

// Minimal JSON reading for the flat claims object. The payload is signed
// before it is parsed, so this only ever sees data the studio produced.
std::string json_string(const std::string& json, const std::string& field) {
  const std::string needle = "\"" + field + "\"";
  const auto at = json.find(needle);

  if (at == std::string::npos) {
    return {};
  }

  auto cursor = json.find(':', at + needle.size());

  if (cursor == std::string::npos) {
    return {};
  }

  cursor = json.find('"', cursor);

  if (cursor == std::string::npos) {
    return {};
  }

  const auto end = json.find('"', cursor + 1);

  return end == std::string::npos ? std::string() : json.substr(cursor + 1, end - cursor - 1);
}

std::int64_t json_number(const std::string& json, const std::string& field) {
  const std::string needle = "\"" + field + "\"";
  const auto at = json.find(needle);

  if (at == std::string::npos) {
    return 0;
  }

  auto cursor = json.find(':', at + needle.size());

  if (cursor == std::string::npos) {
    return 0;
  }

  ++cursor;

  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) {
    ++cursor;
  }

  return std::strtoll(json.c_str() + cursor, nullptr, 10);
}

}  // namespace

const char* to_string(Status status) {
  switch (status) {
    case Status::Active: return "active";
    case Status::UpdateRequired: return "update_required";
    case Status::CheckInRequired: return "check_in_required";
    case Status::Expired: return "expired";
    case Status::WrongMachine: return "wrong_machine";
    case Status::Invalid: return "invalid";
  }

  return "invalid";
}

std::string machine_hash(const std::string& fingerprint) {
  unsigned char digest[32];
  sha256(trim(fingerprint), digest);
  return to_hex(digest, sizeof(digest)).substr(0, 32);
}

bool verify(const std::string& token, const std::string& public_key_hex, Claims& out) {
  const auto dot = token.find('.');

  if (dot == std::string::npos || token.find('.', dot + 1) != std::string::npos) {
    return false;
  }

  const std::string payload = token.substr(0, dot);
  const std::string signature = token.substr(dot + 1);

  if (!ed25519_verify(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
                      b64url_decode(signature), from_hex(public_key_hex))) {
    return false;
  }

  const auto decoded = b64url_decode(payload);
  const std::string json(decoded.begin(), decoded.end());

  out.version = static_cast<int>(json_number(json, "v"));

  if (out.version != 1) {
    return false;
  }

  out.key = json_string(json, "key");
  out.product = json_string(json, "product");
  out.edition = json_string(json, "edition");
  out.customer = json_string(json, "customer");
  out.name = json_string(json, "name");
  out.seats = static_cast<int>(json_number(json, "seats"));
  out.maint_until = json_number(json, "maintUntil");
  out.exp = json_number(json, "exp");
  out.machine = json_string(json, "machine");
  out.mode = json_string(json, "mode");
  out.iat = json_number(json, "iat");
  out.jti = json_string(json, "jti");

  return true;
}

Verdict check(const std::string& token,
              const std::string& fingerprint,
              std::int64_t build_date,
              std::int64_t now,
              const std::string& public_key_hex) {
  Verdict verdict;

  if (!verify(token, public_key_hex, verdict.claims)) {
    verdict.status = Status::Invalid;
    return verdict;
  }

  verdict.has_claims = true;

  if (verdict.claims.machine != machine_hash(fingerprint)) {
    verdict.status = Status::WrongMachine;
    return verdict;
  }

  verdict.check_in_in = verdict.claims.exp - now;

  if (verdict.check_in_in <= 0) {
    // A trial's lease is its lifetime, so a lapsed trial is simply over.
    verdict.status = verdict.claims.edition == "trial" ? Status::Expired : Status::CheckInRequired;
    return verdict;
  }

  verdict.status =
      build_date > verdict.claims.maint_until ? Status::UpdateRequired : Status::Active;

  return verdict;
}

}  // namespace letissier
