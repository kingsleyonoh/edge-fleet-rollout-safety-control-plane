#include <catch2/catch_test_macros.hpp>

#include "shared/canonical_json.hpp"
#include "shared/config.hpp"
#include "shared/digest_service.hpp"
#include "shared/types.hpp"

using edgefleet::shared::CanonicalJson;
using edgefleet::shared::Config;
using edgefleet::shared::DigestService;

TEST_CASE("canonical JSON sorts object keys and preserves array order", "[unit]") {
  const auto first = nlohmann::json{{"b", 2}, {"a", 1}, {"items", nlohmann::json::array({3, 1})}};
  const auto second = nlohmann::json{{"items", nlohmann::json::array({3, 1})}, {"a", 1}, {"b", 2}};

  REQUIRE(CanonicalJson::serialize(first) == CanonicalJson::serialize(second));
  REQUIRE(CanonicalJson::serialize(first) == R"({"a":1,"b":2,"items":[3,1]})");
}

TEST_CASE("digest service matches the SHA-256 and HMAC vectors", "[unit]") {
  REQUIRE(DigestService::sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  REQUIRE(DigestService::hmacSha256Hex("key", "The quick brown fox jumps over the lazy dog") ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
}

TEST_CASE("credential encryption authenticates only the original secret", "[unit][security]") {
  const auto ciphertext = DigestService::encryptSecret("test-encryption-key", "device-secret");
  REQUIRE(ciphertext.starts_with("v1:"));
  REQUIRE(DigestService::decryptSecret("test-encryption-key", ciphertext).value() == "device-secret");
  REQUIRE_FALSE(DigestService::decryptSecret("wrong-key", ciphertext).has_value());
  REQUIRE_FALSE(DigestService::decryptSecret("test-encryption-key", ciphertext.substr(0, ciphertext.size() - 2) + "00").has_value());
}

TEST_CASE("configuration rejects unsafe production secrets and invalid enums", "[unit]") {
  auto env = Config::defaultsForTests();
  env.environment = "production";
  env.sessionEncryptionKey.clear();

  const auto error = Config::validate(env);
  REQUIRE(error.has_value());
  REQUIRE(error->code == "PRODUCTION_SECRET_REQUIRED");

  env.environment = "development";
  env.storageBackend = "oracle";
  REQUIRE(Config::validate(env)->code == "INVALID_STORAGE_BACKEND");

  env.storageBackend = "sqlite";
  env.iotUrl = "http://172.31.255.254:3000/v1";
  REQUIRE_FALSE(Config::validate(env).has_value());
  env.iotUrl = "http://172.32.0.1:3000";
  REQUIRE(Config::validate(env)->code == "INVALID_ADAPTER_URL");
  REQUIRE(edgefleet::shared::isSafePrivateHttpUrl("http://[::1]:3000"));
}

TEST_CASE("UUIDs are opaque, non-empty, and distinct", "[unit]") {
  const auto first = edgefleet::shared::Uuid::generate();
  const auto second = edgefleet::shared::Uuid::generate();

  REQUIRE(first.isValid());
  REQUIRE(second.isValid());
  REQUIRE(first != second);
}
