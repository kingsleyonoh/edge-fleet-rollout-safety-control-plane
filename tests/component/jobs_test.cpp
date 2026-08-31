#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "application/jobs.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/types.hpp"
#include "infrastructure/sqlite_storage.hpp"

TEST_CASE("job leases are exclusive and recoverable", "[component]") {
  const auto db = std::filesystem::temp_directory_path() / "edgefleet-jobs-test.db";
  std::filesystem::remove(db);
  edgefleet::infrastructure::SqliteStorage storage(db.string());
  REQUIRE(storage.open());
  REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto tenant = storage.createTenant("Jobs", "Jobs Ltd", "Jobs", "UTC", "jobs-prefix", "jobs-hash");
  REQUIRE(tenant.has_value());
  const auto tenantId = tenant->at("id").get<std::string>();
  REQUIRE(edgefleet::application::JobCoordinator::acquire(storage, tenantId, "simulator_worker", "0", "worker-a", 60));
  REQUIRE_FALSE(edgefleet::application::JobCoordinator::acquire(storage, tenantId, "simulator_worker", "0", "worker-b", 60));
  REQUIRE(edgefleet::application::JobCoordinator::release(storage, tenantId, "simulator_worker", "0", "worker-a"));
  REQUIRE(edgefleet::application::JobCoordinator::acquire(storage, tenantId, "simulator_worker", "0", "worker-b", 60));
  REQUIRE(storage.execute("UPDATE job_leases SET lease_expires_at=datetime('now','-1 second') WHERE tenant_id=? AND job_name=? AND shard_key=?", {tenantId, "simulator_worker", "0"}));
  REQUIRE(edgefleet::application::JobCoordinator::acquire(storage, tenantId, "simulator_worker", "0", "worker-c", 60));
}

TEST_CASE("worker restart resumes a due release without duplicate commands or evidence", "[component][recovery][release]") {
  const auto root = std::filesystem::temp_directory_path() / ("edgefleet-worker-restart-" + edgefleet::shared::Uuid::generate().str());
  const auto db = root / "edgefleet.db";
  std::filesystem::create_directories(root);
  {
    edgefleet::infrastructure::SqliteStorage storage(db.string());
    REQUIRE(storage.open());
    REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
    const auto tenant = storage.createTenant("Worker Restart", "Worker Restart Ltd", "Worker Restart", "UTC", "worker-restart-prefix", "worker-restart-hash");
    REQUIRE(tenant.has_value());
    const auto tenantId = tenant->at("id").get<std::string>();
    const auto fleet = storage.createFleet(tenantId, "worker-restart-fleet", "Worker Restart Fleet", "production");
    REQUIRE(fleet.has_value());
    const auto device = storage.createDevice(tenantId, fleet->at("id").get<std::string>(),
                                             { {"stable_key", "worker-restart-device"}, {"hardware_model", "m1"}, {"architecture", "x86_64"} },
                                             edgefleet::shared::DigestService::sha256Hex("worker-restart-secret"));
    REQUIRE(device.has_value());

    const auto keyId = edgefleet::shared::Uuid::generate().str();
    const auto artifactId = edgefleet::shared::Uuid::generate().str();
    const auto digest = std::string(64, 'a');
    REQUIRE(storage.execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,status,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519',?,?, 'active','test',datetime('now'),datetime('now'))",
                            {keyId, tenantId, "worker restart key", "test-public-key", digest}));
    REQUIRE(storage.execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,?,?, 'ready',?,?,?,?,?,?,?,'test',datetime('now'),datetime('now'))",
                            {artifactId, tenantId, "worker-restart-artifact", "1", "m1", "x86_64", "artifact.bin", "missing-artifact.bin", "1", digest, "{}", "test-signature", keyId}));
    const auto policy = storage.createPolicy(tenantId, { {"name", "Worker Restart Policy"}, {"stage_plan", {100}}, {"min_observation_sec", 1}, {"two_person_approval", false}, {"rollback_requirement", "allow_first_install"} });
    REQUIRE(policy.has_value());
    const auto policyId = policy->at("id").get<std::string>();
    REQUIRE(storage.execute("UPDATE rollout_policies SET status='active' WHERE tenant_id=? AND id=?", {tenantId, policyId}));
    const auto release = storage.createRelease(tenantId, { {"fleet_id", fleet->at("id")}, {"artifact_id", artifactId}, {"policy_id", policyId}, {"name", "Worker Restart Release"} });
    REQUIRE(release.has_value());
    const auto releaseId = release->at("id").get<std::string>();
    const auto frozenPolicy = edgefleet::shared::CanonicalJson::serialize({ {"stage_plan", {100}}, {"min_observation_sec", 1}, {"two_person_approval", false} });
    REQUIRE(storage.execute("UPDATE releases SET status='scheduled',frozen_policy_json=?,frozen_manifest_json='{}',membership_digest='worker-restart-membership',eligible_device_count=1,current_stage_ordinal=0,scheduled_for=datetime('now','-1 second'),version=1 WHERE tenant_id=? AND id=?",
                            {frozenPolicy, tenantId, releaseId}));
    const auto stageId = edgefleet::shared::Uuid::generate().str();
    REQUIRE(storage.execute("INSERT INTO release_stages(id,tenant_id,release_id,ordinal,target_percentage,status,eligible_count,created_at,updated_at) VALUES(?,?,?,1,100,'pending',1,datetime('now'),datetime('now'))",
                            {stageId, tenantId, releaseId}));
    REQUIRE(storage.execute("INSERT INTO release_memberships(id,tenant_id,release_id,device_id,cohort_hash_hex,cohort_ordinal,frozen_labels_json,included_at) VALUES(?,?,?,?,?,0,'{}',datetime('now'))",
                            {edgefleet::shared::Uuid::generate().str(), tenantId, releaseId, device->at("id").get<std::string>(), "worker-restart-cohort"}));

    const auto tracePath = root / "traces";
    const auto exportPath = root / "exports";
    const auto tempPath = root / "tmp";
    const auto first = edgefleet::application::WorkerCoordinator::run(storage, tenantId, tracePath, exportPath, tempPath, "worker-before-restart");
    REQUIRE(first.scheduledReleases == 1);
    REQUIRE(storage.query("SELECT id FROM release_assignments WHERE tenant_id=? AND release_id=?", {tenantId, releaseId}).size() == 1);
    REQUIRE(storage.query("SELECT id FROM rollout_commands WHERE tenant_id=? AND release_id=? AND command_type='install'", {tenantId, releaseId}).size() == 1);

    const auto second = edgefleet::application::WorkerCoordinator::run(storage, tenantId, tracePath, exportPath, tempPath, "worker-after-restart");
    REQUIRE(second.scheduledReleases == 0);
    REQUIRE(storage.query("SELECT id FROM release_assignments WHERE tenant_id=? AND release_id=?", {tenantId, releaseId}).size() == 1);
    REQUIRE(storage.query("SELECT id FROM rollout_commands WHERE tenant_id=? AND release_id=? AND command_type='install'", {tenantId, releaseId}).size() == 1);
    REQUIRE(storage.query("SELECT id FROM evidence_events WHERE tenant_id=? AND event_type='release.scheduled_started'", {tenantId}).size() == 1);
    REQUIRE(storage.verifyEvidence(tenantId).at("valid").get<bool>());
  }
  std::filesystem::remove_all(root);
}
