#include "shared/digest_service.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <argon2.h>

namespace edgefleet::shared {
namespace {

std::string hexEncode(const unsigned char* bytes, std::size_t length) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string output(length * 2, '0');
  for (std::size_t index = 0; index < length; ++index) {
    output[index * 2] = digits[(bytes[index] >> 4U) & 0x0fU];
    output[index * 2 + 1] = digits[bytes[index] & 0x0fU];
  }
  return output;
}

std::optional<std::vector<unsigned char>> hexDecode(std::string_view value) {
  if (value.size() % 2 != 0) return std::nullopt;
  std::vector<unsigned char> bytes;
  bytes.reserve(value.size() / 2);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    const auto high = value[index];
    const auto low = value[index + 1];
    const auto nibble = [](char character) -> int {
      if (character >= '0' && character <= '9') return character - '0';
      if (character >= 'a' && character <= 'f') return character - 'a' + 10;
      if (character >= 'A' && character <= 'F') return character - 'A' + 10;
      return -1;
    };
    const auto highValue = nibble(high);
    const auto lowValue = nibble(low);
    if (highValue < 0 || lowValue < 0) return std::nullopt;
    bytes.push_back(static_cast<unsigned char>((highValue << 4) | lowValue));
  }
  return bytes;
}

std::array<unsigned char, 32> deriveSecretKey(std::string_view material) {
  std::array<unsigned char, 32> key{};
  unsigned int length = 0;
  EVP_Digest(material.data(), material.size(), key.data(), &length, EVP_sha256(), nullptr);
  return key;
}

}  // namespace

std::string DigestService::sha256Hex(std::string_view input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  EVP_DigestInit_ex(context, EVP_sha256(), nullptr);
  EVP_DigestUpdate(context, input.data(), input.size());
  EVP_DigestFinal_ex(context, digest.data(), &length);
  EVP_MD_CTX_free(context);
  return hexEncode(digest.data(), length);
}

std::string DigestService::sha256File(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  std::vector<char> buffer(1024 * 1024);
  unsigned int length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    if (context != nullptr) EVP_MD_CTX_free(context);
    return {};
  }
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
      EVP_MD_CTX_free(context);
      return {};
    }
  }
  if (!input.eof() || EVP_DigestFinal_ex(context, digest.data(), &length) != 1) {
    EVP_MD_CTX_free(context);
    return {};
  }
  EVP_MD_CTX_free(context);
  return hexEncode(digest.data(), length);
}

std::string DigestService::hmacSha256Hex(std::string_view key, std::string_view input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data(), &length);
  return hexEncode(digest.data(), length);
}

bool DigestService::constantTimeEqual(std::string_view left, std::string_view right) {
  const auto length = std::max(left.size(), right.size());
  unsigned char difference = static_cast<unsigned char>(left.size() ^ right.size());
  for (std::size_t index = 0; index < length; ++index) {
    const auto a = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
    const auto b = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
    difference = static_cast<unsigned char>(difference | (a ^ b));
  }
  return difference == 0;
}

std::string DigestService::randomToken(std::size_t bytes) {
  std::vector<unsigned char> raw(bytes);
  if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
    std::random_device device;
    for (auto& value : raw) value = static_cast<unsigned char>(device());
  }
  return hexEncode(raw.data(), raw.size());
}

std::string DigestService::argon2idHash(std::string_view secret) {
  const auto salt = randomToken(16);
  std::array<char, 256> encoded{};
  const auto result = argon2id_hash_encoded(3, 65536, 1, secret.data(), secret.size(), salt.data(), salt.size(), 32, encoded.data(), encoded.size());
  return result == ARGON2_OK ? std::string(encoded.data()) : std::string{};
}

bool DigestService::argon2idVerify(std::string_view secret, std::string_view encodedHash) {
  if (encodedHash.empty()) return false;
  return argon2id_verify(encodedHash.data(), secret.data(), secret.size()) == ARGON2_OK;
}

std::string DigestService::encryptSecret(std::string_view keyMaterial, std::string_view plaintext) {
  if (keyMaterial.empty()) return {};
  const auto key = deriveSecretKey(keyMaterial);
  std::array<unsigned char, 12> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) return {};
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (context == nullptr) return {};
  std::vector<unsigned char> encrypted(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
  std::array<unsigned char, 16> tag{};
  int written = 0;
  int finalWritten = 0;
  const bool initialized = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
                           EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) == 1 &&
                           EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
                           EVP_EncryptUpdate(context, encrypted.data(), &written, reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) == 1 &&
                           EVP_EncryptFinal_ex(context, encrypted.data() + written, &finalWritten) == 1 &&
                           EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1;
  EVP_CIPHER_CTX_free(context);
  if (!initialized) return {};
  encrypted.resize(static_cast<std::size_t>(written + finalWritten));
  std::string result = "v1:" + hexEncode(nonce.data(), nonce.size()) + hexEncode(encrypted.data(), encrypted.size()) + hexEncode(tag.data(), tag.size());
  return result;
}

std::optional<std::string> DigestService::decryptSecret(std::string_view keyMaterial, std::string_view ciphertext) {
  if (keyMaterial.empty() || !ciphertext.starts_with("v1:")) return std::nullopt;
  const auto encoded = hexDecode(ciphertext.substr(3));
  if (!encoded.has_value() || encoded->size() < 12 + 16) return std::nullopt;
  const auto& bytes = *encoded;
  const auto key = deriveSecretKey(keyMaterial);
  const auto nonce = bytes.data();
  const auto encrypted = bytes.data() + 12;
  const auto encryptedLength = bytes.size() - 12 - 16;
  const auto tag = bytes.data() + 12 + encryptedLength;
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (context == nullptr) return std::nullopt;
  std::vector<unsigned char> plaintext(encryptedLength + EVP_MAX_BLOCK_LENGTH);
  int written = 0;
  int finalWritten = 0;
  bool valid = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
               EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
               EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce) == 1 &&
               EVP_DecryptUpdate(context, plaintext.data(), &written, encrypted, static_cast<int>(encryptedLength)) == 1 &&
               EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char*>(tag)) == 1 &&
               EVP_DecryptFinal_ex(context, plaintext.data() + written, &finalWritten) == 1;
  EVP_CIPHER_CTX_free(context);
  if (!valid) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(plaintext.data()), static_cast<std::size_t>(written + finalWritten));
}

}  // namespace edgefleet::shared
