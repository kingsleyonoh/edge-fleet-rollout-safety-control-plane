#pragma once

#include <string>
#include <string_view>
#include <optional>

namespace edgefleet::shared {

class DigestService {
 public:
  static std::string sha256Hex(std::string_view input);
  static std::string sha256File(const std::string& path);
  static std::string hmacSha256Hex(std::string_view key, std::string_view input);
  static bool constantTimeEqual(std::string_view left, std::string_view right);
  static std::string randomToken(std::size_t bytes = 24);
  static std::string argon2idHash(std::string_view secret);
  static bool argon2idVerify(std::string_view secret, std::string_view encodedHash);
  static std::string encryptSecret(std::string_view keyMaterial, std::string_view plaintext);
  static std::optional<std::string> decryptSecret(std::string_view keyMaterial, std::string_view ciphertext);
};

}  // namespace edgefleet::shared
