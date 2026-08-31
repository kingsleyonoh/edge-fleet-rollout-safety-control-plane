#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "application/control_plane.hpp"
#include "shared/types.hpp"

namespace {

edgefleet::web::HttpRequest request(std::string method, std::string target, std::string body = {}, std::string key = {}) {
  edgefleet::web::HttpRequest result{std::move(method), std::move(target), {}, std::move(body)};
  if (!key.empty()) result.headers["authorization"] = "Bearer " + key;
  return result;
}

}  // namespace

TEST_CASE("two tenants cannot read each other's resources, jobs, evidence, or integrations", "[integration][security][tenant]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-tenant-isolation-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));

  auto registerTenant = [&](const std::string& name, const std::string& peer) {
    auto registration = request("POST", "/api/tenants/register", "{\"name\":\"" + name + "\"}");
    registration.headers["x-peer-address"] = peer;
    const auto response = app.handle(registration);
    REQUIRE(response.status == 201);
    return nlohmann::json::parse(response.body);
  };
  const auto tenantA = registerTenant("Isolation A", "isolation-a");
  const auto tenantB = registerTenant("Isolation B", "isolation-b");
  const auto keyA = tenantA.at("api_key").get<std::string>();
  const auto keyB = tenantB.at("api_key").get<std::string>();
  const auto tenantIdA = tenantA.at("tenant").at("id").get<std::string>();
  const auto tenantIdB = tenantB.at("tenant").at("id").get<std::string>();

  auto fleetRequest = request("POST", "/api/fleets", R"({"name":"A Fleet","slug":"a-fleet","environment":"production"})", keyA);
  fleetRequest.headers["idempotency-key"] = "isolation-a-fleet";
  const auto fleetResponse = app.handle(fleetRequest);
  REQUIRE(fleetResponse.status == 201);
  const auto fleetId = nlohmann::json::parse(fleetResponse.body).at("id").get<std::string>();
  auto deviceRequest = request("POST", "/api/fleets/" + fleetId + "/devices", R"({"stable_key":"a-device","hardware_model":"m1","architecture":"x86_64","device_secret":"isolation-device-secret"})", keyA);
  deviceRequest.headers["idempotency-key"] = "isolation-a-device";
  const auto deviceResponse = app.handle(deviceRequest);
  REQUIRE(deviceResponse.status == 201);
  const auto deviceId = nlohmann::json::parse(deviceResponse.body).at("id").get<std::string>();

  const auto policy = app.storage()->createPolicy(tenantIdA, {{"name", "Isolation Policy"}, {"stage_plan", {100}}, {"two_person_approval", false}});
  REQUIRE(policy.has_value());
  REQUIRE(app.storage()->execute("UPDATE rollout_policies SET status='active' WHERE tenant_id=? AND id=?", {tenantIdA, policy->at("id").get<std::string>()}));
  const auto signingKeyId = edgefleet::shared::Uuid::generate().str();
  const auto artifactId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(app.storage()->execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519','fixture',?,?,datetime('now'),datetime('now'))", {signingKeyId, tenantIdA, "isolation-key", "isolation-fingerprint", "test"}));
  REQUIRE(app.storage()->execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready','artifact.bin','fixture',1,'isolation-digest','{}','fixture',?,?,datetime('now'),datetime('now'))", {artifactId, tenantIdA, "isolation-artifact", "1", "m1", "x86_64", signingKeyId, "test"}));
  const auto release = app.storage()->createRelease(tenantIdA, {{"fleet_id", fleetId}, {"artifact_id", artifactId}, {"policy_id", policy->at("id")}, {"name", "Isolation Release"}});
  REQUIRE(release.has_value());

  auto simulation = request("POST", "/api/simulations", R"({"scenario_name":"isolation","input":{"device_count":1,"duration_seconds":1},"seed":42})", keyA);
  simulation.headers["idempotency-key"] = "isolation-a-simulation";
  const auto simulationResponse = app.handle(simulation);
  REQUIRE(simulationResponse.status == 202);
  const auto simulationId = nlohmann::json::parse(simulationResponse.body).at("id").get<std::string>();
  const auto replayId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(app.storage()->execute("INSERT INTO replay_runs(id,tenant_id,simulation_run_id,source_kind,status,expected_decision_digest,created_at) VALUES(?,?,?,'simulation','queued','isolation-digest',datetime('now'))", {replayId, tenantIdA, simulationId}));
  REQUIRE(app.storage()->execute("INSERT INTO integration_configs(id,tenant_id,adapter_type,endpoint_base_url,secret_ref,settings_json,health_status,created_at,updated_at) VALUES(?,?, 'notification_hub_v1','http://fixture','HUB_KEY','{}','disabled',datetime('now'),datetime('now'))", {edgefleet::shared::Uuid::generate().str(), tenantIdA}));

  for (const auto& path : {"/api/fleets/" + fleetId, "/api/devices/" + deviceId, "/api/artifacts/" + artifactId, "/api/releases/" + release->at("id").get<std::string>(), "/api/simulations/" + simulationId, "/api/replays/" + replayId}) {
    REQUIRE(app.handle(request("GET", path, {}, keyB)).status == 404);
  }
  const auto tenantBEvidence = app.handle(request("GET", "/api/evidence", {}, keyB));
  REQUIRE(tenantBEvidence.status == 200);
  REQUIRE(nlohmann::json::parse(tenantBEvidence.body).at("items").size() == 1);
  REQUIRE(app.storage()->query("SELECT id FROM evidence_events WHERE tenant_id=?", {tenantIdB}).size() == 1);
  REQUIRE(nlohmann::json::parse(app.handle(request("GET", "/api/integrations", {}, keyB)).body).at("items").empty());
  REQUIRE(nlohmann::json::parse(app.handle(request("GET", "/api/devices", {}, keyB)).body).at("items").empty());
  REQUIRE(nlohmann::json::parse(app.handle(request("GET", "/api/releases", {}, keyB)).body).at("items").empty());
}
