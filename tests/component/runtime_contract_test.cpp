#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "application/evidence_export.hpp"
#include "application/jobs.hpp"
#include "infrastructure/integrations.hpp"
#include "infrastructure/sqlite_storage.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/types.hpp"

namespace {

std::filesystem::path temporaryPath(const std::string& prefix, const std::string& extension = ".db") {
  return std::filesystem::temp_directory_path() / (prefix + edgefleet::shared::Uuid::generate().str() + extension);
}

struct TestStorage {
  std::filesystem::path databasePath = temporaryPath("edgefleet-runtime-");
  edgefleet::infrastructure::SqliteStorage storage{databasePath.string()};
  std::string tenantId;

  TestStorage() {
    REQUIRE(storage.open());
    REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
    const auto tenant = storage.createTenant("Runtime", "Runtime Ltd", "Runtime", "UTC", "runtime-prefix", "runtime-hash");
    REQUIRE(tenant.has_value());
    tenantId = tenant->at("id").get<std::string>();
  }

  ~TestStorage() { storage.close(); std::filesystem::remove(databasePath); }
};

}  // namespace

TEST_CASE("adapter contracts normalize fixtures and fail closed", "[component][integration]") {
  const auto readings = edgefleet::infrastructure::IotFixtureParser::parse({{"freshness_seconds", 120}, {"readings", {{{"device_id", "device-1"}, {"metric_name", "availability"}, {"value", 1.0}}}}});
  REQUIRE(readings.ok());
  REQUIRE(readings.value->size() == 1);
  REQUIRE(readings.value->front().fresh);
  REQUIRE(readings.value->front().sourceEventId.size() == 64);

  const auto stale = edgefleet::infrastructure::IotFixtureParser::parse({{"readings", {{{"device_id", "device-1"}, {"metric_name", "availability"}, {"value", 1.0}, {"fresh", false}}}}});
  REQUIRE(stale.ok());
  REQUIRE_FALSE(stale.value->front().fresh);
  const auto future = edgefleet::infrastructure::IotFixtureParser::parse({{"readings", {{{"device_id", "device-1"}, {"metric_name", "availability"}, {"value", 1.0}, {"observed_at", "2099-01-01T00:00:00Z"}}}}});
  REQUIRE(future.ok());
  REQUIRE_FALSE(future.value->front().fresh);
  REQUIRE_FALSE(edgefleet::infrastructure::IotFixtureParser::parse({{"readings", {{{"device_id", "device-1"}, {"metric_name", "availability"}, {"value", "bad"}}}}}).ok());

  const auto retry = edgefleet::infrastructure::AdapterContract::fixtureDelivery("notification_hub_v1", {{"fixture_mode", true}, {"fixture_status", 429}}, "outbox-1", 0);
  REQUIRE(retry.disposition == edgefleet::infrastructure::DeliveryDisposition::retryable_failure);
  const auto dead = edgefleet::infrastructure::AdapterContract::fixtureDelivery("workflow_manual_v1", {{"fixture_mode", true}, {"fixture_behavior", "timeout_after_write"}}, "outbox-2", 0);
  REQUIRE(dead.disposition == edgefleet::infrastructure::DeliveryDisposition::ambiguous_delivery);
  REQUIRE(edgefleet::infrastructure::isSupportedAdapterType("iot_rest_v1"));
  REQUIRE_FALSE(edgefleet::infrastructure::isSupportedAdapterType("unknown"));
}

namespace {

struct AssignmentFixture {
  std::string fleetId;
  std::string deviceId;
  std::string releaseId;
  std::string stageId;
  std::string assignmentId;
  std::string artifactId;
};

AssignmentFixture seedAssignment(TestStorage& fixture) {
  AssignmentFixture result;
  const auto fleet = fixture.storage.createFleet(fixture.tenantId, "adapter-fleet", "Adapter Fleet", "production");
  REQUIRE(fleet.has_value());
  result.fleetId = fleet->at("id").get<std::string>();
  const auto device = fixture.storage.createDevice(fixture.tenantId, result.fleetId, {{"stable_key", "adapter-device"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}}, "device-hash");
  REQUIRE(device.has_value());
  result.deviceId = device->at("id").get<std::string>();
  const auto signingKeyId = edgefleet::shared::Uuid::generate().str();
  result.artifactId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(fixture.storage.execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519','pem',?,?,datetime('now'),datetime('now'))", {signingKeyId, fixture.tenantId, "adapter-key", "fingerprint", "test"}));
  REQUIRE(fixture.storage.execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready','fixture','fixture',1,?, '{}','signature',?,?,datetime('now'),datetime('now'))", {result.artifactId, fixture.tenantId, "adapter-artifact", "1", "m1", "x86_64", "digest", signingKeyId, "test"}));
  const auto policy = fixture.storage.createPolicy(fixture.tenantId, {{"name", "Adapter Policy"}, {"stage_plan", {100}}, {"rollback_requirement", "allow_first_install"}});
  REQUIRE(policy.has_value());
  const auto release = fixture.storage.createRelease(fixture.tenantId, {{"fleet_id", result.fleetId}, {"artifact_id", result.artifactId}, {"policy_id", policy->at("id")}, {"name", "Adapter Release"}});
  REQUIRE(release.has_value());
  result.releaseId = release->at("id").get<std::string>();
  result.stageId = edgefleet::shared::Uuid::generate().str();
  result.assignmentId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(fixture.storage.execute("INSERT INTO release_stages(id,tenant_id,release_id,ordinal,target_percentage,status,eligible_count,created_at,updated_at) VALUES(?,?,?,1,100,'active',1,datetime('now'),datetime('now'))", {result.stageId, fixture.tenantId, result.releaseId}));
  REQUIRE(fixture.storage.execute("INSERT INTO release_assignments(id,tenant_id,release_id,stage_id,device_id,desired_artifact_id,desired_generation,state,updated_at) VALUES(?,?,?,?,?,?,1,'commanded',datetime('now'))", {result.assignmentId, fixture.tenantId, result.releaseId, result.stageId, result.deviceId, result.artifactId}));
  return result;
}

}  // namespace

TEST_CASE("adapter jobs persist IoT samples and publish frozen outbox rows", "[component][integration][jobs]") {
  TestStorage fixture;
  const auto assignment = seedAssignment(fixture);
  const auto settings = edgefleet::shared::CanonicalJson::serialize({{"fixture_mode", true}, {"freshness_seconds", 120}, {"readings", {{{"device_id", assignment.deviceId}, {"metric_name", "availability"}, {"value", 1.0}, {"source_event_id", "iot-event-1"}}}}});
  REQUIRE(fixture.storage.execute("INSERT INTO integration_configs(id,tenant_id,adapter_type,enabled,required_for_promotion,endpoint_base_url,secret_ref,settings_json,poll_cursor_json,health_status,created_at,updated_at) VALUES(?,?, 'iot_rest_v1',1,0,'http://fixture','IOT_KEY',?,'{}','disabled',datetime('now'),datetime('now'))", {edgefleet::shared::Uuid::generate().str(), fixture.tenantId, settings}));
  REQUIRE(edgefleet::application::IotHealthPoller::run(fixture.storage, fixture.tenantId) == 1);
  REQUIRE(edgefleet::application::IotHealthPoller::run(fixture.storage, fixture.tenantId) == 0);
  REQUIRE(fixture.storage.query("SELECT id FROM health_samples WHERE tenant_id=? AND source='iot_rest_v1'", {fixture.tenantId}).size() == 1);

  const auto event = fixture.storage.appendEvidence(fixture.tenantId, "release.paused", "release", assignment.releaseId, {{"reason", "adapter job test"}});
  REQUIRE(event.has_value());
  const auto notificationSettings = edgefleet::shared::CanonicalJson::serialize({{"fixture_mode", true}, {"fixture_status", 202}});
  REQUIRE(fixture.storage.execute("INSERT INTO integration_configs(id,tenant_id,adapter_type,enabled,required_for_promotion,endpoint_base_url,secret_ref,settings_json,poll_cursor_json,health_status,created_at,updated_at) VALUES(?,?, 'notification_hub_v1',1,0,'http://fixture','HUB_KEY',?,'{}','disabled',datetime('now'),datetime('now'))", {edgefleet::shared::Uuid::generate().str(), fixture.tenantId, notificationSettings}));
  const auto outboxId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(fixture.storage.execute("INSERT INTO outbox_deliveries(id,tenant_id,evidence_event_id,adapter_type,status,payload_json,idempotency_key,next_attempt_at,created_at,updated_at) VALUES(?,?,?,'notification_hub_v1','pending',?,?,datetime('now'),datetime('now'),datetime('now'))", {outboxId, fixture.tenantId, event->at("id").get<std::string>(), "{}", "adapter-outbox-1"}));
  REQUIRE(edgefleet::application::OutboxPublisher::run(fixture.storage, fixture.tenantId, "adapter-worker").first == 1);
  REQUIRE(fixture.storage.query("SELECT id FROM outbox_deliveries WHERE tenant_id=? AND id=? AND status='published'", {fixture.tenantId, outboxId}).size() == 1);
}

TEST_CASE("workflow observer records one terminal result and warning", "[component][integration][jobs]") {
  TestStorage fixture;
  const auto event = fixture.storage.appendEvidence(fixture.tenantId, "release.paused", "release", "release-1", {{"reason", "workflow job test"}});
  REQUIRE(event.has_value());
  const auto settings = edgefleet::shared::CanonicalJson::serialize({{"fixture_mode", true}, {"fixture_observed_status", "failed"}});
  REQUIRE(fixture.storage.execute("INSERT INTO integration_configs(id,tenant_id,adapter_type,enabled,required_for_promotion,endpoint_base_url,secret_ref,settings_json,poll_cursor_json,health_status,created_at,updated_at) VALUES(?,?, 'workflow_manual_v1',1,0,'http://fixture','WORKFLOW_KEY',?,'{}','healthy',datetime('now'),datetime('now'))", {edgefleet::shared::Uuid::generate().str(), fixture.tenantId, settings}));
  const auto outboxId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(fixture.storage.execute("INSERT INTO outbox_deliveries(id,tenant_id,evidence_event_id,adapter_type,status,payload_json,idempotency_key,external_reference,external_status,next_attempt_at,created_at,updated_at) VALUES(?,?,?,'workflow_manual_v1','published',?,?,?,'running',datetime('now'),datetime('now'),datetime('now'))", {outboxId, fixture.tenantId, event->at("id").get<std::string>(), "{}", "workflow-outbox-1", "execution-1"}));
  REQUIRE(edgefleet::application::WorkflowExecutionObserver::run(fixture.storage, fixture.tenantId) == 1);
  REQUIRE(fixture.storage.query("SELECT id FROM outbox_deliveries WHERE tenant_id=? AND id=? AND external_status='failed'", {fixture.tenantId, outboxId}).size() == 1);
  REQUIRE(fixture.storage.query("SELECT id FROM operator_notices WHERE tenant_id=? AND title='Workflow execution failed'", {fixture.tenantId}).size() == 1);
}

TEST_CASE("queued simulation and replay workers persist one frozen source", "[component][jobs]") {
  TestStorage fixture;
  const auto simulationId = edgefleet::shared::Uuid::generate().str();
  const auto input = edgefleet::shared::CanonicalJson::serialize({{"schema_version", "v1"}, {"device_count", 32}, {"duration_seconds", 600}, {"failure_probability", 0.0}});
  REQUIRE(fixture.storage.execute("INSERT INTO simulation_runs(id,tenant_id,scenario_name,scenario_version,status,seed,input_json,input_digest,simulator_version,requested_by_actor_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,datetime('now'))", {simulationId, fixture.tenantId, "contract", "v1", "queued", "42", input, edgefleet::shared::DigestService::sha256Hex(input), "pcg64-v1", "test"}));
  const auto tracePath = temporaryPath("edgefleet-traces-", "");
  REQUIRE(edgefleet::application::SimulationJobRunner::run(fixture.storage, fixture.tenantId, tracePath, "worker") == 1);
  const auto simulation = fixture.storage.query("SELECT status,result_digest,trace_storage_key FROM simulation_runs WHERE tenant_id=? AND id=?", {fixture.tenantId, simulationId});
  REQUIRE(simulation.front().at("status") == "completed");
  REQUIRE(std::filesystem::exists(simulation.front().at("trace_storage_key").get<std::string>()));

  const auto replayId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(fixture.storage.execute("INSERT INTO replay_runs(id,tenant_id,simulation_run_id,source_kind,status,expected_decision_digest,created_at) VALUES(?,?,?,'simulation','queued',?,datetime('now'))", {replayId, fixture.tenantId, simulationId, simulation.front().at("result_digest").get<std::string>()}));
  REQUIRE(edgefleet::application::ReplayJobRunner::run(fixture.storage, fixture.tenantId, "worker") == 1);
  const auto replay = fixture.storage.query("SELECT status,actual_decision_digest FROM replay_runs WHERE tenant_id=? AND id=?", {fixture.tenantId, replayId});
  REQUIRE(replay.front().at("status") == "reproduced");
  REQUIRE(replay.front().at("actual_decision_digest") == simulation.front().at("result_digest"));
  std::filesystem::remove_all(tracePath);
}

TEST_CASE("benchmark worker commits exactly 108 cells and a DuckDB bundle", "[component][benchmark]") {
  TestStorage fixture;
  const auto benchmarkId = edgefleet::shared::Uuid::generate().str();
  const auto manifest = edgefleet::shared::CanonicalJson::serialize({{"schema_version", "v1"}, {"corpus_version", "v1"}, {"simulator_version", "pcg64-v1"}, {"expected_case_count", 108}});
  REQUIRE(fixture.storage.execute("INSERT INTO benchmark_runs(id,tenant_id,corpus_version,corpus_manifest_json,corpus_manifest_digest,status,expected_case_count,requested_by_actor_id,created_at) VALUES(?,?,?,?,?,'queued',108,?,datetime('now'))", {benchmarkId, fixture.tenantId, "v1", manifest, edgefleet::shared::DigestService::sha256Hex(manifest), "test"}));
  const auto outputPath = temporaryPath("edgefleet-benchmark-", "");
  REQUIRE(edgefleet::application::BenchmarkJobRunner::run(fixture.storage, fixture.tenantId, outputPath, "worker") == 1);
  const auto run = fixture.storage.query("SELECT status,completed_case_count,duckdb_storage_key,json_report_storage_key,markdown_report_storage_key,report_bundle_sha256 FROM benchmark_runs WHERE tenant_id=? AND id=?", {fixture.tenantId, benchmarkId});
  REQUIRE(run.front().at("status") == "completed");
  REQUIRE(run.front().at("completed_case_count") == 108);
  REQUIRE(std::filesystem::exists(run.front().at("duckdb_storage_key").get<std::string>()));
  REQUIRE(std::filesystem::exists(run.front().at("json_report_storage_key").get<std::string>()));
  REQUIRE(std::filesystem::exists(run.front().at("markdown_report_storage_key").get<std::string>()));
  REQUIRE(run.front().at("report_bundle_sha256").get<std::string>().size() == 64);
  REQUIRE(std::filesystem::file_size(run.front().at("duckdb_storage_key").get<std::string>()) > 0);
  std::filesystem::remove_all(outputPath);
}

TEST_CASE("evidence export preserves its frozen chain range", "[component][evidence]") {
  TestStorage fixture;
  REQUIRE(fixture.storage.appendEvidence(fixture.tenantId, "first", "tenant", fixture.tenantId, {{"value", 1}}).has_value());
  const auto event = fixture.storage.query("SELECT sequence_no,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no DESC LIMIT 1", {fixture.tenantId}).front();
  const auto tenant = fixture.storage.getTenant(fixture.tenantId);
  REQUIRE(tenant.has_value());
  const auto exportId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(fixture.storage.execute("INSERT INTO evidence_exports(id,tenant_id,status,source_event_from,source_event_to,source_chain_head_hash,aggregate_filters_json,tenant_snapshot_json,requested_by_actor_id,created_at) VALUES(?,?, 'queued',?,?,?,?,?,?,datetime('now'))", {exportId, fixture.tenantId, "1", std::to_string(event.at("sequence_no").get<long long>()), event.at("event_hash").get<std::string>(), "{}", edgefleet::shared::CanonicalJson::serialize(*tenant), "test"}));
  const auto outputPath = temporaryPath("edgefleet-export-", "");
  REQUIRE(edgefleet::application::EvidenceExportJobRunner::run(fixture.storage, fixture.tenantId, outputPath, "worker") == 1);
  const auto result = fixture.storage.query("SELECT status,output_storage_key,output_sha256 FROM evidence_exports WHERE tenant_id=? AND id=?", {fixture.tenantId, exportId});
  REQUIRE(result.front().at("status") == "completed");
  REQUIRE(std::filesystem::exists(result.front().at("output_storage_key").get<std::string>()));
  REQUIRE(result.front().at("output_sha256").get<std::string>().size() == 64);
  std::filesystem::remove_all(outputPath);
}

TEST_CASE("evidence export retry rejects a changed frozen chain head", "[component][evidence][recovery]") {
  TestStorage fixture;
  REQUIRE(fixture.storage.appendEvidence(fixture.tenantId, "first", "tenant", fixture.tenantId, {{"value", 1}}).has_value());
  REQUIRE(fixture.storage.appendEvidence(fixture.tenantId, "second", "tenant", fixture.tenantId, {{"value", 2}}).has_value());
  const auto events = fixture.storage.query("SELECT sequence_no,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no", {fixture.tenantId});
  REQUIRE(events.size() == 2);
  const auto tenant = fixture.storage.getTenant(fixture.tenantId);
  REQUIRE(tenant.has_value());
  const auto exportId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(fixture.storage.execute("INSERT INTO evidence_exports(id,tenant_id,status,source_event_from,source_event_to,source_chain_head_hash,aggregate_filters_json,tenant_snapshot_json,requested_by_actor_id,attempt_count,lease_owner,lease_expires_at,started_at,created_at) VALUES(?,?, 'running',1,1,?,?,?, ?,1,?,datetime('now','-1 second'),datetime('now','-2 seconds'),datetime('now'))",
                                  {exportId, fixture.tenantId, events.at(1).at("event_hash").get<std::string>(), "{}", edgefleet::shared::CanonicalJson::serialize(*tenant), "test", "previous-worker"}));
  const auto outputPath = temporaryPath("edgefleet-export-retry-", "");
  REQUIRE(edgefleet::application::EvidenceExportJobRunner::run(fixture.storage, fixture.tenantId, outputPath, "restarted-worker") == 0);
  const auto result = fixture.storage.query("SELECT status,failure_message,attempt_count FROM evidence_exports WHERE tenant_id=? AND id=?", {fixture.tenantId, exportId});
  REQUIRE(result.size() == 1);
  REQUIRE(result.front().at("status") == "failed");
  REQUIRE(result.front().at("failure_message") == "CHAIN_HEAD_CHANGED");
  REQUIRE(result.front().at("attempt_count") == 2);
  REQUIRE_FALSE(std::filesystem::exists(outputPath / (exportId + ".ndjson")));
  std::filesystem::remove_all(outputPath);
}
