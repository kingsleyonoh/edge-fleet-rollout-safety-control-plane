#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "application/control_plane.hpp"
#include "application/jobs.hpp"
#include "domain/artifact.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/types.hpp"

namespace {

using edgefleet::shared::Json;
using edgefleet::web::HttpRequest;
using edgefleet::web::HttpResponse;

HttpRequest operatorRequest(std::string method, std::string target, std::string body, const std::string& apiKey, const std::string& idempotency = {}) {
  HttpRequest request{std::move(method), std::move(target), {}, std::move(body)};
  request.headers["authorization"] = "Bearer " + apiKey;
  if (!idempotency.empty()) request.headers["idempotency-key"] = idempotency;
  return request;
}

HttpRequest deviceReportRequest(const std::string& deviceId, const std::string& secret, int sequence, const Json& body) {
  const auto serialized = body.dump();
  HttpRequest request{"POST", "/api/agent/v1/reports", {}, serialized};
  request.headers["x-device-id"] = deviceId;
  request.headers["x-device-key-version"] = "1";
  request.headers["x-device-secret"] = secret;
  request.headers["x-device-sequence"] = std::to_string(sequence);
  request.headers["x-device-signature"] = edgefleet::shared::DigestService::hmacSha256Hex(secret, "POST /api/agent/v1/reports " + std::to_string(sequence) + " " + edgefleet::shared::DigestService::sha256Hex(serialized));
  return request;
}

Json createSignedArtifact(edgefleet::application::ControlPlane& app, const std::string& apiKey, const Json& key, const std::string& name,
                          const std::string& version, const std::string& payloadName) {
  const Json manifest{{"artifact", payloadName}, {"bytes", "standalone-journey"}};
  const auto manifestJson = edgefleet::shared::CanonicalJson::serialize(manifest);
  const auto digest = edgefleet::shared::DigestService::sha256Hex(manifestJson);
  const Json signedPayload{{"digest", digest}, {"size_bytes", manifestJson.size()}, {"name", name}, {"version", version},
                           {"hardware_model", "m1"}, {"architecture", "x86_64"}, {"manifest", manifest}};
  const auto signature = edgefleet::domain::ArtifactSigner::sign(edgefleet::shared::CanonicalJson::serialize(signedPayload), key.at("private_key_pem").get<std::string>());
  REQUIRE(signature.ok());
  auto request = operatorRequest("POST", "/api/artifacts",
                                 Json{{"name", name}, {"version", version}, {"hardware_model", "m1"}, {"architecture", "x86_64"},
                                      {"manifest", manifest}, {"signature", *signature.value}, {"signing_key_id", key.at("id")}}.dump(), apiKey,
                                 "standalone-artifact-" + name);
  const auto response = app.handle(request);
  REQUIRE(response.status == 201);
  return Json::parse(response.body);
}

}  // namespace

TEST_CASE("standalone SQLite journey completes a one hundred device five stage release", "[integration][standalone][release]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-standalone-journey-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));

  const auto setup = app.handle(HttpRequest{"POST", "/api/tenants/register", {}, R"({"name":"Standalone Journey"})"});
  REQUIRE(setup.status == 201);
  const auto setupBody = Json::parse(setup.body);
  const auto apiKey = setupBody.at("api_key").get<std::string>();
  const auto tenantId = setupBody.at("tenant").at("id").get<std::string>();

  auto fleetRequest = operatorRequest("POST", "/api/fleets", R"({"name":"Journey Fleet","slug":"journey-fleet","environment":"production"})", apiKey, "journey-fleet");
  const auto fleetResponse = app.handle(fleetRequest);
  REQUIRE(fleetResponse.status == 201);
  const auto fleetId = Json::parse(fleetResponse.body).at("id").get<std::string>();

  std::vector<std::string> deviceIds;
  deviceIds.reserve(100);
  for (int index = 0; index < 100; ++index) {
    const auto device = app.storage()->createDevice(tenantId, fleetId,
                                                     Json{{"stable_key", "journey-device-" + std::to_string(index)}, {"hardware_model", "m1"}, {"architecture", "x86_64"}},
                                                     edgefleet::shared::DigestService::sha256Hex("journey-device-secret"));
    REQUIRE(device.has_value());
    deviceIds.push_back(device->at("id").get<std::string>());
  }

  auto keyResponse = app.handle(operatorRequest("POST", "/api/artifact-signing-keys", "{}", apiKey, "journey-target-key"));
  REQUIRE(keyResponse.status == 201);
  const auto targetKey = Json::parse(keyResponse.body);
  const auto target = createSignedArtifact(app, apiKey, targetKey, "journey-target", "1.0.0", "target");
  auto rollbackKeyResponse = app.handle(operatorRequest("POST", "/api/artifact-signing-keys", "{}", apiKey, "journey-rollback-key"));
  REQUIRE(rollbackKeyResponse.status == 201);
  const auto rollbackKey = Json::parse(rollbackKeyResponse.body);
  const auto rollback = createSignedArtifact(app, apiKey, rollbackKey, "journey-rollback", "0.9.0", "rollback");

  const Json policyBody{{"name", "Journey fixed waves"}, {"stage_plan", Json::array({1, 5, 20, 50, 100})},
                        {"health_gates", {{"fresh_device_coverage", 0.80}, {"install_failure_rate", 0.01}, {"convergence_rate", 0.98}}},
                        {"max_offline_fraction", 0.20}, {"telemetry_freshness_sec", 120}, {"min_observation_sec", 1},
                        {"two_person_approval", true}, {"require_iot_evidence", false}, {"rollback_requirement", "required"}};
  const auto policyResponse = app.handle(operatorRequest("POST", "/api/policies", policyBody.dump(), apiKey, "journey-policy"));
  REQUIRE(policyResponse.status == 201);
  const auto policyId = Json::parse(policyResponse.body).at("id").get<std::string>();
  REQUIRE(app.handle(operatorRequest("POST", "/api/policies/" + policyId + "/activate", R"({"reason":"activate standalone journey policy"})", apiKey, "journey-policy-activate")).status == 200);

  const Json releaseBody{{"name", "Standalone 100 device release"}, {"fleet_id", fleetId}, {"artifact_id", target.at("id")},
                         {"rollback_artifact_id", rollback.at("id")}, {"policy_id", policyId}};
  const auto releaseResponse = app.handle(operatorRequest("POST", "/api/releases", releaseBody.dump(), apiKey, "journey-release"));
  REQUIRE(releaseResponse.status == 201);
  const auto releaseId = Json::parse(releaseResponse.body).at("id").get<std::string>();

  auto release = Json::parse(app.handle(operatorRequest("GET", "/api/releases/" + releaseId, {}, apiKey)).body);
  auto validate = app.handle(operatorRequest("POST", "/api/releases/" + releaseId + "/validate", Json{{"expected_version", release.at("version")}}.dump(), apiKey, "journey-validate"));
  REQUIRE(validate.status == 200);
  release = Json::parse(app.handle(operatorRequest("GET", "/api/releases/" + releaseId, {}, apiKey)).body);
  auto submit = app.handle(operatorRequest("POST", "/api/releases/" + releaseId + "/submit", Json{{"expected_version", release.at("version")}, {"reason", "request two-person start review"}}.dump(), apiKey, "journey-submit"));
  REQUIRE(submit.status == 200);
  release = Json::parse(submit.body);
  const auto approvalRows = Json::parse(app.handle(operatorRequest("GET", "/api/approvals", {}, apiKey)).body).at("items");
  REQUIRE(approvalRows.size() == 1);
  auto approverResponse = app.handle(operatorRequest("POST", "/api/credentials", R"({"label":"journey-approver","role":"approver"})", apiKey, "journey-approver"));
  REQUIRE(approverResponse.status == 201);
  const auto approverKey = Json::parse(approverResponse.body).at("api_key").get<std::string>();
  REQUIRE(app.handle(operatorRequest("POST", "/api/approvals/" + approvalRows.at(0).at("id").get<std::string>() + "/approve", R"({"reason":"independent journey approval"})", approverKey, "journey-approval")).status == 200);

  release = Json::parse(app.handle(operatorRequest("GET", "/api/releases/" + releaseId, {}, apiKey)).body);
  const auto started = app.handle(operatorRequest("POST", "/api/releases/" + releaseId + "/start", Json{{"expected_version", release.at("version")}, {"reason", "start five stage healthy rollout"}}.dump(), apiKey, "journey-start"));
  REQUIRE(started.status == 200);

  const auto targetDigest = target.at("sha256_digest").get<std::string>();
  for (int stage = 1; stage <= 5; ++stage) {
    const auto detail = Json::parse(app.handle(operatorRequest("GET", "/api/releases/" + releaseId, {}, apiKey)).body);
    const auto stages = detail.at("stages");
    const auto active = std::find_if(stages.begin(), stages.end(), [](const auto& row) { return row.value("status", "") == "active"; });
    REQUIRE(active != stages.end());
    REQUIRE(active->at("ordinal") == stage);
    const auto assignments = Json::parse(app.handle(operatorRequest("GET", "/api/releases/" + releaseId + "/assignments", {}, apiKey)).body).at("items");
    int stageAssignments = 0;
    int cumulativeAssignments = 0;
    for (const auto& assignment : assignments) {
      const auto assignmentStage = std::find_if(stages.begin(), stages.end(), [&](const auto& row) { return row.at("id") == assignment.at("stage_id"); });
      REQUIRE(assignmentStage != stages.end());
      if (assignmentStage->at("ordinal") > stage) continue;
      ++cumulativeAssignments;
      if (assignment.at("stage_id") == active->at("id")) ++stageAssignments;
      const auto deviceId = assignment.at("device_id").get<std::string>();
      const auto report = Json{{"device_id", deviceId}, {"release_id", releaseId}, {"report_id", "journey-report-" + std::to_string(stage) + "-" + deviceId}, {"report_sequence", stage},
                               {"report_type", "install_result"}, {"observed_generation", assignment.at("desired_generation")}, {"observed_artifact_digest", targetDigest}, {"health", Json::object()}};
      REQUIRE(app.handle(deviceReportRequest(deviceId, "journey-device-secret", stage, report)).status == 202);
    }
    static const std::vector<int> newAssignments{1, 4, 15, 30, 50};
    static const std::vector<int> cumulativeAssignmentCounts{1, 5, 20, 50, 100};
    REQUIRE(stageAssignments == newAssignments.at(static_cast<std::size_t>(stage - 1)));
    REQUIRE(cumulativeAssignments == cumulativeAssignmentCounts.at(static_cast<std::size_t>(stage - 1)));
    REQUIRE(app.storage()->execute("UPDATE release_stages SET observation_ends_at=datetime('now','-1 second') WHERE tenant_id=? AND release_id=? AND id=?", {detail.at("tenant_id").get<std::string>(), releaseId, active->at("id").get<std::string>()}));
    REQUIRE(edgefleet::application::StageGateEvaluatorJob::run(*app.storage(), detail.at("tenant_id").get<std::string>(), "standalone-journey") == 1);
    const auto gate = app.storage()->query("SELECT decision,metrics_json FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? ORDER BY evaluated_at DESC LIMIT 1", {tenantId, releaseId});
    REQUIRE_FALSE(gate.empty());
    CAPTURE(stage, gate.front().value("decision", ""), gate.front().value("metrics_json", "{}"));
    REQUIRE(gate.front().value("decision", "") == "pass");
  }

  const auto completed = Json::parse(app.handle(operatorRequest("GET", "/api/releases/" + releaseId, {}, apiKey)).body);
  REQUIRE(completed.at("status") == "completed");
  REQUIRE(completed.at("current_stage_ordinal") == 5);
  REQUIRE(completed.at("stages").size() == 5);
  REQUIRE(std::all_of(completed.at("stages").begin(), completed.at("stages").end(), [](const auto& row) { return row.value("status", "") == "passed"; }));
  REQUIRE(app.storage()->query("SELECT id FROM release_assignments WHERE tenant_id=? AND release_id=?", {tenantId, releaseId}).size() == 100);
  REQUIRE(app.storage()->query("SELECT id FROM rollout_commands WHERE tenant_id=? AND release_id=? AND command_type='install'", {tenantId, releaseId}).size() == 100);
  REQUIRE(app.storage()->query("SELECT id FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? AND decision='pass'", {tenantId, releaseId}).size() == 5);
  REQUIRE(app.storage()->verifyEvidence(completed.at("tenant_id").get<std::string>()).value("valid", false));
}
