// LeTissier licence verification — C++17.
// Used by Datamosh (FFGL plugin).
//
// Crypto backend is chosen at compile time so a distributed plugin does not
// have to drag OpenSSL along:
//
//   -DLICENCE_BACKEND_SODIUM    libsodium (recommended for shipping plugins;
//                               statically linkable, no runtime dependency)
//   -DLICENCE_BACKEND_OPENSSL   OpenSSL EVP (default; convenient where it is
//                               already present)
//   -DLICENCE_BACKEND_MONOCYPHER  Monocypher's Ed25519 plus a SHA-256 in this
//                               file. Added in ffgl-datamosh, which compiles
//                               it from source on all three platforms; see
//                               external/monocypher/README.md for why.
//
// Both call the same 32-byte-key Ed25519 verify. Swapping backends is a
// compile flag, not a code change.

#pragma once

#include <cstdint>
#include <string>

namespace letissier {

// The studio's licence signing key. Public: safe to ship in a binary.
// The live key from https://letissier.ie/integrate. ffgl-datamosh can override
// it at configure time (DATAMOSH_LICENCE_PUBLIC_KEY), so this default is what
// every normal build ships.
inline constexpr const char* kPublicKeyHex =
    "1fca6c21f2eb7963fd646272a731a41a191d3a4cda839e295c5cda67978fcc85";

enum class Status {
  Active,           // good to run
  UpdateRequired,   // licence fine, but this build is newer than the entitlement
  CheckInRequired,  // lease lapsed; check in to renew
  Expired,          // a trial that has run out
  WrongMachine,     // issued for a different machine
  Invalid,          // signature failed, malformed, or wrong version
};

// Stable string, matching the other language SDKs.
const char* to_string(Status status);

struct Claims {
  int version = 0;
  std::string key;
  std::string product;
  std::string edition;
  std::string customer;
  std::string name;
  int seats = 0;
  std::int64_t maint_until = 0;  // entitled to builds released at or before this
  std::int64_t exp = 0;          // check-in deadline for this lease
  std::string machine;
  std::string mode;
  std::int64_t iat = 0;
  std::string jti;
};

struct Verdict {
  Status status = Status::Invalid;
  bool has_claims = false;
  Claims claims;
  std::int64_t check_in_in = 0;  // seconds until check-in is due; negative once overdue
};

// Must match the server exactly: sha256 of the trimmed fingerprint, first 32
// hex characters.
std::string machine_hash(const std::string& fingerprint);

// Verify the signature and parse the claims. No clock or machine checks.
bool verify(const std::string& token, const std::string& public_key_hex, Claims& out);

// The whole decision, offline.
//
// build_date is when THIS build was released (unix seconds), baked in at
// compile time. An older build stays entitled forever; a newer one asks for a
// renewal — that is what makes "a year of updates, yours to keep" work with no
// server involved.
Verdict check(const std::string& token,
              const std::string& fingerprint,
              std::int64_t build_date,
              std::int64_t now,
              const std::string& public_key_hex = kPublicKeyHex);

}  // namespace letissier
