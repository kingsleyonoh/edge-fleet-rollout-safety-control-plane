#include "domain/artifact.hpp"

#include <memory>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "shared/digest_service.hpp"

namespace edgefleet::domain {
namespace {

template <typename T, void (*Free)(T*)>
using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;

struct BioDeleter {
  void operator()(BIO* value) const { if (value != nullptr) BIO_free(value); }
};

std::string bioText(BIO* bio) {
  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio, &memory);
  return memory == nullptr ? std::string{} : std::string(memory->data, memory->length);
}

std::string base64Encode(const unsigned char* bytes, int length) {
  std::string output(static_cast<std::size_t>(4 * ((length + 2) / 3)), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), bytes, length);
  return output;
}

std::vector<unsigned char> base64Decode(std::string_view input) {
  std::vector<unsigned char> output((input.size() * 3) / 4 + 3);
  const auto length = EVP_DecodeBlock(output.data(), reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
  if (length < 0) return {};
  output.resize(static_cast<std::size_t>(length));
  while (!output.empty() && input.back() == '=') { output.pop_back(); input.remove_suffix(1); }
  return output;
}

EVP_PKEY* readPrivate(std::string_view pem) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) return nullptr;
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return key;
}

EVP_PKEY* readPublic(std::string_view pem) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) return nullptr;
  EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return key;
}

}  // namespace

shared::Result<SigningKeyPair> ArtifactSigner::generateKeyPair() {
  OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1) return shared::Result<SigningKeyPair>::failure({"KEY_GENERATION_FAILED", "Ed25519 key generation could not start.", 500});
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw) != 1) return shared::Result<SigningKeyPair>::failure({"KEY_GENERATION_FAILED", "Ed25519 key generation failed.", 500});
  OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(raw, EVP_PKEY_free);
  std::unique_ptr<BIO, BioDeleter> publicBio(BIO_new(BIO_s_mem()));
  std::unique_ptr<BIO, BioDeleter> privateBio(BIO_new(BIO_s_mem()));
  if (!publicBio || !privateBio || PEM_write_bio_PUBKEY(publicBio.get(), key.get()) != 1 || PEM_write_bio_PrivateKey(privateBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) return shared::Result<SigningKeyPair>::failure({"KEY_SERIALIZATION_FAILED", "Ed25519 key serialization failed.", 500});
  SigningKeyPair result{bioText(privateBio.get()), bioText(publicBio.get()), {}};
  result.fingerprintSha256 = fingerprint(result.publicKeyPem);
  return shared::Result<SigningKeyPair>::success(std::move(result));
}

shared::Result<std::string> ArtifactSigner::sign(std::string_view payload, std::string_view privateKeyPem) {
  OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(readPrivate(privateKeyPem), EVP_PKEY_free);
  OpenSslPtr<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!key || !context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) return shared::Result<std::string>::failure({"SIGNATURE_FAILED", "Artifact signing failed.", 422});
  std::size_t length = 0;
  if (EVP_DigestSign(context.get(), nullptr, &length, reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) != 1) return shared::Result<std::string>::failure({"SIGNATURE_FAILED", "Artifact signing failed.", 422});
  std::vector<unsigned char> signature(length);
  if (EVP_DigestSign(context.get(), signature.data(), &length, reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) != 1) return shared::Result<std::string>::failure({"SIGNATURE_FAILED", "Artifact signing failed.", 422});
  return shared::Result<std::string>::success(base64Encode(signature.data(), static_cast<int>(length)));
}

bool ArtifactSigner::verify(std::string_view payload, std::string_view signatureBase64, std::string_view publicKeyPem) {
  OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(readPublic(publicKeyPem), EVP_PKEY_free);
  OpenSslPtr<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  const auto signature = base64Decode(signatureBase64);
  return key && context && !signature.empty() && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 && EVP_DigestVerify(context.get(), signature.data(), signature.size(), reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) == 1;
}

std::string ArtifactSigner::fingerprint(std::string_view publicKeyPem) {
  OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(readPublic(publicKeyPem), EVP_PKEY_free);
  if (!key) return {};
  const auto length = i2d_PUBKEY(key.get(), nullptr);
  if (length <= 0) return {};
  std::vector<unsigned char> encoded(static_cast<std::size_t>(length));
  auto* cursor = encoded.data();
  if (i2d_PUBKEY(key.get(), &cursor) != length) return {};
  return shared::DigestService::sha256Hex(std::string(reinterpret_cast<char*>(encoded.data()), encoded.size()));
}

}  // namespace edgefleet::domain
