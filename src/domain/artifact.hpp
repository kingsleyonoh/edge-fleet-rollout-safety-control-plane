#pragma once

#include <string>

#include "shared/types.hpp"

namespace edgefleet::domain {

struct SigningKeyPair {
  std::string privateKeyPem;
  std::string publicKeyPem;
  std::string fingerprintSha256;
};

class ArtifactSigner {
 public:
  static shared::Result<SigningKeyPair> generateKeyPair();
  static shared::Result<std::string> sign(std::string_view payload, std::string_view privateKeyPem);
  static bool verify(std::string_view payload, std::string_view signatureBase64, std::string_view publicKeyPem);
  static std::string fingerprint(std::string_view publicKeyPem);
};

}  // namespace edgefleet::domain
