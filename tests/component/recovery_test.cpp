#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "domain/artifact.hpp"
#include "domain/safety.hpp"
#include "infrastructure/sqlite_storage.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/types.hpp"

namespace {

std::filesystem::path recoveryPath() {
  if (const auto* configured = std::getenv("EDGEFLEET_RECOVERY_FIXTURE_ROOT"); configured != nullptr && *configured != '\0') return configured;
  return std::filesystem::temp_directory_path() / ("edgefleet-recovery-fixture-" + edgefleet::shared::Uuid::generate().str());
}

struct ArtifactFixture {
  std::string id;
  std::string keyId;
  std::string digest;
};

ArtifactFixture createArtifact(edgefleet::infrastructure::SqliteStorage& storage, const std::string& tenantId, const std::filesystem::path& root,
                               const std::string& name, const std::string& version, const std::string& bytes) {
  std::filesystem::create_directories(root);
  const auto path = root / (name + "-" + version + ".bin");
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  REQUIRE(static_cast<bool>(output));

  const auto key = edgefleet::domain::ArtifactSigner::generateKeyPair();
  REQUIRE(key.ok());
  ArtifactFixture result{edgefleet::shared::Uuid::generate().str(), edgefleet::shared::Uuid::generate().str(), edgefleet::shared::DigestService::sha256File(path.string())};
  REQUIRE(storage.execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519',?,?,?,datetime('now'),datetime('now'))",
                          {result.keyId, tenantId, name + " key", key.value->publicKeyPem, key.value->fingerprintSha256, "recovery-fixture"}));
  const auto manifest = edgefleet::shared::Json{{"artifact", name}, {"version", version}, {"fixture", "recovery"}};
  const auto signedPayload = edgefleet::shared::CanonicalJson::serialize({{"digest", result.digest}, {"size_bytes", bytes.size()}, {"name", name}, {"version", version},
                                                                            {"hardware_model", "m1"}, {"architecture", "x86_64"}, {"manifest", manifest}});
  const auto signature = edgefleet::domain::ArtifactSigner::sign(signedPayload, key.value->privateKeyPem);
  REQUIRE(signature.ok());
  REQUIRE(storage.execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready',?,?,?,?,?,?,?, ?,datetime('now'),datetime('now'))",
                          {result.id, tenantId, name, version, "m1", "x86_64", path.filename().string(), path.string(), std::to_string(bytes.size()), result.digest,
                           edgefleet::shared::CanonicalJson::serialize(manifest), *signature.value, result.keyId, "recovery-fixture"}));
  return result;
}

}  // namespace

TEST_CASE("recovery fixture contains a completed release, verified artifacts, and a replay to recover", "[component][recovery]") {
  const auto root = recoveryPath();
  const auto databasePath = root / "edgefleet.db";
  const auto artifactPath = root / "artifacts";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  bool keepFixture = std::getenv("EDGEFLEET_RECOVERY_FIXTURE_ROOT") != nullptr;
  {
    edgefleet::infrastructure::SqliteStorage storage(databasePath.string());
    REQUIRE(storage.open());
    REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
    const auto tenant = storage.createTenant("Recovery Tenant", "Recovery Ltd", "Recovery", "UTC", "recovery-prefix", "recovery-hash");
    REQUIRE(tenant.has_value());
    const auto tenantId = tenant->at("id").get<std::string>();
    const auto fleet = storage.createFleet(tenantId, "recovery-fleet", "Recovery Fleet", "production");
    REQUIRE(fleet.has_value());
    const auto device = storage.createDevice(tenantId, fleet->at("id").get<std::string>(), {{"stable_key", "recovery-device"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}}, "device-secret-hash");
    REQUIRE(device.has_value());
    const auto target = createArtifact(storage, tenantId, artifactPath, "target", "1", "target-recovery-bytes");
    const auto rollback = createArtifact(storage, tenantId, artifactPath, "rollback", "0", "rollback-recovery-bytes");
    const auto policy = storage.createPolicy(tenantId, { {"name", "Recovery Policy"}, {"stage_plan", {100}}, {"rollback_requirement", "required"}, {"two_person_approval", false} });
    REQUIRE(policy.has_value());
    const auto policyId = policy->at("id").get<std::string>();
    REQUIRE(storage.execute("UPDATE rollout_policies SET status='active' WHERE tenant_id=? AND id=?", {tenantId, policyId}));
    const auto release = storage.createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", target.id}, {"rollback_artifact_id", rollback.id}, {"policy_id", policyId}, {"name", "Recovery Release"}});
    REQUIRE(release.has_value());
    const auto releaseId = release->at("id").get<std::string>();
    const auto stageId = edgefleet::shared::Uuid::generate().str();
    REQUIRE(storage.execute("UPDATE releases SET status='completed',version=2,frozen_policy_json=?,frozen_manifest_json=?,frozen_rollback_json=?,membership_digest='recovery-membership',eligible_device_count=1,current_stage_ordinal=1,started_at=datetime('now'),ended_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=?",
                            {edgefleet::shared::CanonicalJson::serialize({{"stage_plan", {100}}, {"min_observation_sec", 1}}), "{}", edgefleet::shared::CanonicalJson::serialize({{"artifact_id", rollback.id}, {"digest", rollback.digest}}), tenantId, releaseId}));
    REQUIRE(storage.execute("INSERT INTO release_stages(id,tenant_id,release_id,ordinal,target_percentage,status,eligible_count,assigned_count,started_at,ended_at,created_at,updated_at) VALUES(?,?,?,1,100,'passed',1,1,datetime('now'),datetime('now'),datetime('now'),datetime('now'))",
                            {stageId, tenantId, releaseId}));
    const auto assignmentId = edgefleet::shared::Uuid::generate().str();
    REQUIRE(storage.execute("INSERT INTO release_assignments(id,tenant_id,release_id,stage_id,device_id,desired_artifact_id,desired_generation,state,updated_at) VALUES(?,?,?,?,?,?,1,'converged',datetime('now'))",
                            {assignmentId, tenantId, releaseId, stageId, device->at("id").get<std::string>(), target.id}));
    const auto gateDigest = edgefleet::shared::DigestService::sha256Hex("recovery-gate");
    REQUIRE(storage.execute("INSERT INTO health_gate_evaluations(id,tenant_id,release_id,stage_id,decision,sample_window_start,sample_window_end,sample_count,eligible_device_count,fresh_device_count,metrics_json,failed_gates_json,evidence_digest,evaluated_at) VALUES(?,?,?,?,'pass',datetime('now','-1 minute'),datetime('now'),1,1,1,?,'[]',?,datetime('now'))",
                            {edgefleet::shared::Uuid::generate().str(), tenantId, releaseId, stageId, edgefleet::shared::CanonicalJson::serialize({{"assigned", 1}, {"fresh", 1}, {"converged", 1}}), gateDigest}));
    REQUIRE(storage.appendEvidence(tenantId, "release.gate.evaluated", "release", releaseId, {{"decision", "pass"}, {"evidence_digest", gateDigest}}, "job", "recovery-fixture").has_value());
    REQUIRE(storage.appendEvidence(tenantId, "release.completed", "release", releaseId, {{"decision", "pass"}, {"membership_digest", "recovery-membership"}}, "job", "recovery-fixture").has_value());

    const auto input = edgefleet::shared::Json{{"schema_version", "v1"}, {"device_count", 8}, {"duration_seconds", 60}, {"failure_probability", 0.0}};
    const auto simulation = edgefleet::domain::Simulator::run(input, 42);
    REQUIRE(simulation.ok());
    const auto simulationId = edgefleet::shared::Uuid::generate().str();
    const auto inputJson = edgefleet::shared::CanonicalJson::serialize(input);
    REQUIRE(storage.execute("INSERT INTO simulation_runs(id,tenant_id,scenario_name,scenario_version,status,seed,input_json,input_digest,simulator_version,result_json,result_digest,requested_by_actor_id,completed_at,created_at) VALUES(?,?,?,'v1','completed',?,?,?,?,?,?,? ,datetime('now'),datetime('now'))",
                            {simulationId, tenantId, "recovery", "42", inputJson, edgefleet::shared::DigestService::sha256Hex(inputJson), "pcg64-v1", simulation.value->metrics.dump(), simulation.value->resultDigest, "recovery-fixture"}));
    REQUIRE(storage.execute("INSERT INTO replay_runs(id,tenant_id,simulation_run_id,source_kind,status,expected_decision_digest,created_at) VALUES(?,?,?,'simulation','queued',?,datetime('now'))",
                            {edgefleet::shared::Uuid::generate().str(), tenantId, simulationId, simulation.value->resultDigest}));
    REQUIRE(storage.verifyEvidence(tenantId).at("valid").get<bool>());
  }
  if (keepFixture) {
    REQUIRE(std::filesystem::exists(databasePath));
    REQUIRE(std::filesystem::exists(artifactPath / "target-1.bin"));
  } else {
    std::filesystem::remove_all(root);
  }
}
