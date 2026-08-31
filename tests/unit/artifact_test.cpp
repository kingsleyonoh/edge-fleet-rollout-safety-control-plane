#include <catch2/catch_test_macros.hpp>

#include "domain/artifact.hpp"

TEST_CASE("Ed25519 artifact signatures verify and reject changed payloads", "[unit]") {
  const auto key = edgefleet::domain::ArtifactSigner::generateKeyPair();
  REQUIRE(key.ok());
  const auto signature = edgefleet::domain::ArtifactSigner::sign("manifest-v1", key.value->privateKeyPem);
  REQUIRE(signature.ok());
  REQUIRE(edgefleet::domain::ArtifactSigner::verify("manifest-v1", *signature.value, key.value->publicKeyPem));
  REQUIRE_FALSE(edgefleet::domain::ArtifactSigner::verify("manifest-v2", *signature.value, key.value->publicKeyPem));
  REQUIRE(key.value->fingerprintSha256.size() == 64);
}
