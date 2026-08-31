#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <fstream>
#include <limits>
#include <thread>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "application/control_plane.hpp"
#include "domain/artifact.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/types.hpp"
#include "web/http_server.hpp"
#include "web/policy_registry.hpp"

namespace {

edgefleet::web::HttpRequest request(std::string method, std::string target, std::string body = {}, std::string key = {}) {
  edgefleet::web::HttpRequest result{std::move(method), std::move(target), {}, std::move(body)};
  if (!key.empty()) result.headers["authorization"] = "Bearer " + key;
  return result;
}

}  // namespace

TEST_CASE("the HTTP adapter serves a real loopback socket with security headers", "[integration][http]") {
  edgefleet::web::HttpServer server("127.0.0.1", 0, [](const auto& request) {
    return edgefleet::web::HttpResponse{200, "application/json", "{\"path\":\"" + request.target + "\"}", {}};
  });
#ifdef _WIN32
  WSADATA data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
  std::atomic<bool> started{false};
  std::thread serverThread([&] { started = server.start(); });
  for (int attempt = 0; attempt < 100 && server.boundPort() == 0; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(server.boundPort() != 0);
#ifdef _WIN32
  const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  REQUIRE(socket != INVALID_SOCKET);
#else
  const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(socket >= 0);
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<unsigned short>(server.boundPort()));
  REQUIRE(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  REQUIRE(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  const std::string requestText = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  REQUIRE(::send(socket, requestText.data(), static_cast<int>(requestText.size()), 0) == static_cast<int>(requestText.size()));
  std::string response;
  char buffer[1024]{};
  int received = 0;
  while ((received = ::recv(socket, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, received);
#ifdef _WIN32
  closesocket(socket);
  WSACleanup();
#else
  close(socket);
#endif
  server.stop();
  serverThread.join();
  REQUIRE(started);
  REQUIRE(response.find("HTTP/1.1 200 OK") != std::string::npos);
  REQUIRE(response.find("Content-Security-Policy:") != std::string::npos);
  REQUIRE(response.find("{\"path\":\"/health\"}") != std::string::npos);
}

TEST_CASE("the HTTP adapter spools large request bodies and enforces the transport ceiling", "[integration][http][security]") {
  const auto spoolDirectory = std::filesystem::temp_directory_path() / ("edgefleet-http-spool-" + edgefleet::shared::Uuid::generate().str());
  std::atomic<bool> spooled{false};
  edgefleet::web::HttpServer server("127.0.0.1", 0, [&](const auto& request) {
    spooled = request.body.empty() && !request.bodyFilePath.empty() && request.bodySize == 2ULL * 1024ULL * 1024ULL && std::filesystem::file_size(request.bodyFilePath) == request.bodySize;
    return edgefleet::web::HttpResponse{200, "application/json", "{\"spooled\":true}", {}};
  }, spoolDirectory.string(), 3ULL * 1024ULL * 1024ULL);
#ifdef _WIN32
  WSADATA data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
  std::atomic<bool> started{false};
  std::thread serverThread([&] { started = server.start(); });
  for (int attempt = 0; attempt < 100 && server.boundPort() == 0; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(server.boundPort() != 0);
#ifdef _WIN32
  const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  REQUIRE(socket != INVALID_SOCKET);
#else
  const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(socket >= 0);
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<unsigned short>(server.boundPort()));
  REQUIRE(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  REQUIRE(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  const std::string payload(2ULL * 1024ULL * 1024ULL, 'x');
  const auto header = "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
  const auto wire = header + payload;
  std::size_t sent = 0;
  while (sent < wire.size()) {
    const auto count = ::send(socket, wire.data() + sent, static_cast<int>(std::min<std::size_t>(wire.size() - sent, 1024ULL * 1024ULL)), 0);
    REQUIRE(count > 0);
    sent += static_cast<std::size_t>(count);
  }
  std::string response;
  char buffer[1024]{};
  int received = 0;
  while ((received = ::recv(socket, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, received);
#ifdef _WIN32
  closesocket(socket);
  WSACleanup();
#else
  close(socket);
#endif
  server.stop();
  serverThread.join();
  REQUIRE(started);
  REQUIRE(spooled);
  REQUIRE(response.find("HTTP/1.1 200 OK") != std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(spoolDirectory, error);
}

TEST_CASE("rate limits, browser CSRF, and adapter SSRF validation fail closed", "[integration][security]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-security-test-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));

  for (int index = 0; index < 5; ++index) {
    const auto response = app.handle(request("POST", "/api/tenants/register", "{\"name\":\"Rate tenant " + std::to_string(index) + "\"}"));
    REQUIRE(response.status == 201);
  }
  const auto limited = app.handle(request("POST", "/api/tenants/register", R"({"name":"Rate tenant blocked"})"));
  REQUIRE(limited.status == 429);
  REQUIRE(limited.headers.at("Retry-After") != "");

  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Browser tenant"})"));
  REQUIRE(setup.status == 429);
}

TEST_CASE("browser mutations require the session-bound CSRF token and live adapters reject unsafe targets", "[integration][security]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-csrf-test-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));

  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"CSRF tenant"})"));
  REQUIRE(setup.status == 201);
  const auto apiKey = nlohmann::json::parse(setup.body).at("api_key").get<std::string>();
  const auto session = app.handle(request("POST", "/auth/session", "{\"api_key\":\"" + apiKey + "\"}"));
  REQUIRE(session.status == 204);
  const auto cookies = session.headers.at("Set-Cookie");
  const auto sessionStart = cookies.find("edgefleet_session=");
  const auto sessionValueStart = sessionStart + std::string("edgefleet_session=").size();
  const auto sessionValueEnd = cookies.find(';', sessionValueStart);
  const auto sessionToken = cookies.substr(sessionValueStart, sessionValueEnd - sessionValueStart);
  const auto csrfStart = cookies.find("edgefleet_csrf=");
  const auto csrfValueStart = csrfStart + std::string("edgefleet_csrf=").size();
  const auto csrfValueEnd = cookies.find(';', csrfValueStart);
  const auto csrfToken = cookies.substr(csrfValueStart, csrfValueEnd - csrfValueStart);

  auto browserMutation = request("PATCH", "/api/tenants/me", R"({"display_name":"No CSRF","legal_name":"CSRF tenant Ltd"})");
  browserMutation.headers["cookie"] = "edgefleet_session=" + sessionToken;
  browserMutation.headers["idempotency-key"] = "csrf-missing";
  REQUIRE(app.handle(browserMutation).status == 403);
  browserMutation.headers["x-csrf-token"] = csrfToken;
  REQUIRE(app.handle(browserMutation).status == 200);

  auto unsafeAdapter = request("PUT", "/api/integrations/notification_hub_v1", R"({"endpoint_base_url":"http://169.254.169.254/latest","secret_ref":"EDGEFLEET_NOTIFICATION_API_KEY","settings":{"fixture_mode":false}})", apiKey);
  unsafeAdapter.headers["idempotency-key"] = "unsafe-adapter";
  const auto unsafeResponse = app.handle(unsafeAdapter);
  REQUIRE(unsafeResponse.status == 422);
  REQUIRE(nlohmann::json::parse(unsafeResponse.body).at("error").at("code") == "UNSAFE_ADAPTER_URL");
}

TEST_CASE("integration administration configures, tests, enables, and safely disables supported adapters", "[integration][adapter][security]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-adapter-admin-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Adapter Admin Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto apiKey = nlohmann::json::parse(setup.body).at("api_key").get<std::string>();
  auto configure = request("PUT", "/api/integrations/notification_hub_v1", R"({"endpoint_base_url":"http://localhost","secret_ref":"NOTIFICATION_HUB_API_KEY","required_for_promotion":false,"settings":{"fixture_mode":true}})", apiKey);
  configure.headers["idempotency-key"] = "adapter-configure";
  const auto configured = app.handle(configure);
  REQUIRE(configured.status == 200);
  REQUIRE(nlohmann::json::parse(configured.body).at("enabled") == 0);
  auto testConnection = request("POST", "/api/integrations/notification_hub_v1/test", "{}", apiKey);
  testConnection.headers["idempotency-key"] = "adapter-test";
  const auto tested = app.handle(testConnection);
  REQUIRE(tested.status == 200);
  REQUIRE(nlohmann::json::parse(tested.body).at("status") == "fixture_ok");
  auto enable = request("POST", "/api/integrations/notification_hub_v1/enable", R"({"reason":"Enable after fixture health check"})", apiKey);
  enable.headers["idempotency-key"] = "adapter-enable";
  const auto enabled = app.handle(enable);
  REQUIRE(enabled.status == 200);
  REQUIRE(nlohmann::json::parse(enabled.body).at("enabled") == true);
  auto disable = request("POST", "/api/integrations/notification_hub_v1/disable", R"({"reason":"Disable for standalone safety test"})", apiKey);
  disable.headers["idempotency-key"] = "adapter-disable";
  const auto disabled = app.handle(disable);
  REQUIRE(disabled.status == 200);
  REQUIRE(nlohmann::json::parse(disabled.body).at("enabled") == false);
  auto unknown = request("PUT", "/api/integrations/unknown_adapter", R"({"settings":{"fixture_mode":true}})", apiKey);
  unknown.headers["idempotency-key"] = "adapter-unknown";
  REQUIRE(app.handle(unknown).status == 422);
}

TEST_CASE("device reports remain immutable and prove convergence only with matching generation and digest", "[integration][device][safety]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-device-report-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Device Report Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto apiKey = nlohmann::json::parse(setup.body).at("api_key").get<std::string>();
  const auto tenantId = nlohmann::json::parse(setup.body).at("tenant").at("id").get<std::string>();
  const auto fleet = app.storage()->createFleet(tenantId, "report-fleet", "Report Fleet", "production");
  REQUIRE(fleet.has_value());
  const auto device = app.storage()->createDevice(tenantId, fleet->at("id"), {{"stable_key", "report-device"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}}, edgefleet::shared::DigestService::sha256Hex("device-secret"));
  REQUIRE(device.has_value());
  const auto policy = app.storage()->createPolicy(tenantId, {{"name", "Report Policy"}, {"stage_plan", {100}}, {"two_person_approval", false}});
  REQUIRE(policy.has_value());
  const auto signingKeyId = edgefleet::shared::Uuid::generate().str();
  const auto artifactId = edgefleet::shared::Uuid::generate().str();
  const auto stageId = edgefleet::shared::Uuid::generate().str();
  const auto assignmentId = edgefleet::shared::Uuid::generate().str();
  const auto commandId = edgefleet::shared::Uuid::generate().str();
  const auto targetDigest = edgefleet::shared::DigestService::sha256Hex("target-bytes");
  REQUIRE(app.storage()->execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519','fixture',?,?,datetime('now'),datetime('now'))", {signingKeyId, tenantId, "report-key", "report-fingerprint", "test"}));
  REQUIRE(app.storage()->execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready','target.bin','fixture',11,?, '{}','fixture',?,?,datetime('now'),datetime('now'))", {artifactId, tenantId, "target", "1", "m1", "x86_64", targetDigest, signingKeyId, "test"}));
  const auto release = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", artifactId}, {"policy_id", policy->at("id")}, {"name", "Report Release"}});
  REQUIRE(release.has_value());
  REQUIRE(app.storage()->execute("UPDATE releases SET status='running',version=1 WHERE tenant_id=? AND id=?", {tenantId, release->at("id").get<std::string>()}));
  REQUIRE(app.storage()->execute("INSERT INTO release_stages(id,tenant_id,release_id,ordinal,target_percentage,status,eligible_count,assigned_count,created_at,updated_at) VALUES(?,?,?,1,100,'active',1,1,datetime('now'),datetime('now'))", {stageId, tenantId, release->at("id").get<std::string>()}));
  REQUIRE(app.storage()->execute("INSERT INTO release_assignments(id,tenant_id,release_id,stage_id,device_id,desired_artifact_id,desired_generation,state,commanded_at,updated_at) VALUES(?,?,?,?,?,?,1,'commanded',datetime('now'),datetime('now'))", {assignmentId, tenantId, release->at("id").get<std::string>(), stageId, device->at("id").get<std::string>(), artifactId}));
  REQUIRE(app.storage()->execute("INSERT INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) VALUES(?,?,?,?,?,?, 'install',?,?,? ,?,datetime('now'),datetime('now','+1 day'),datetime('now'))", {commandId, tenantId, release->at("id").get<std::string>(), stageId, assignmentId, device->at("id").get<std::string>(), "1", artifactId, "{}", "report-command"}));

  const auto signedReport = [&](std::int64_t sequence, std::string reportId, std::int64_t generation, std::string digest) {
    const auto body = nlohmann::json{{"device_id", device->at("id")}, {"report_id", std::move(reportId)}, {"report_sequence", sequence}, {"report_type", "observation"}, {"release_id", release->at("id")}, {"command_id", commandId}, {"observed_generation", generation}, {"observed_artifact_digest", std::move(digest)}, {"health", {{"availability", 1.0}}}}.dump();
    auto result = request("POST", "/api/agent/v1/reports", body);
    result.headers["x-device-id"] = device->at("id").get<std::string>();
    result.headers["x-device-secret"] = "device-secret";
    result.headers["x-device-key-version"] = "1";
    result.headers["x-device-sequence"] = std::to_string(sequence);
    result.headers["x-device-signature"] = edgefleet::shared::DigestService::hmacSha256Hex("device-secret", "POST /api/agent/v1/reports " + std::to_string(sequence) + " " + edgefleet::shared::DigestService::sha256Hex(body));
    return std::pair{std::move(result), body};
  };

  auto first = signedReport(2, "report-2", 1, targetDigest);
  const auto accepted = app.handle(first.first);
  REQUIRE(accepted.status == 202);
  REQUIRE(nlohmann::json::parse(app.handle(request("GET", "/api/devices/" + device->at("id").get<std::string>(), {}, apiKey)).body).at("observed_generation") == 1);
  REQUIRE(nlohmann::json::parse(app.handle(request("GET", "/api/devices/" + device->at("id").get<std::string>(), {}, apiKey)).body).at("observed_artifact_digest") == targetDigest);
  REQUIRE(app.storage()->query("SELECT state FROM release_assignments WHERE tenant_id=? AND id=?", {tenantId, assignmentId}).front().at("state") == "converged");
  REQUIRE(app.handle(first.first).status == 202);
  REQUIRE(app.storage()->query("SELECT id FROM device_reports WHERE tenant_id=? AND device_id=?", {tenantId, device->at("id").get<std::string>()}).size() == 1);

  auto outOfOrder = signedReport(1, "report-1", 0, "old-digest");
  const auto outOfOrderResponse = app.handle(outOfOrder.first);
  REQUIRE((outOfOrderResponse.status == 202 || outOfOrderResponse.status == 409));
  const auto projection = app.storage()->query("SELECT last_report_sequence,observed_generation,observed_artifact_digest FROM devices WHERE tenant_id=? AND id=?", {tenantId, device->at("id").get<std::string>()}).front();
  REQUIRE(projection.at("last_report_sequence") == 2);
  REQUIRE(projection.at("observed_generation") == 1);
  REQUIRE(projection.at("observed_artifact_digest") == targetDigest);
  REQUIRE(app.storage()->query("SELECT id FROM health_samples WHERE tenant_id=? AND device_id=?", {tenantId, device->at("id").get<std::string>()}).size() == 1);
}

TEST_CASE("release controls are optimistic and pre-run cancellation issues no command", "[integration][safety][race]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-control-race-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Control Race Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto apiKey = nlohmann::json::parse(setup.body).at("api_key").get<std::string>();
  const auto tenantId = nlohmann::json::parse(setup.body).at("tenant").at("id").get<std::string>();
  const auto fleet = app.storage()->createFleet(tenantId, "control-fleet", "Control Fleet", "production");
  REQUIRE(fleet.has_value());
  const auto policy = app.storage()->createPolicy(tenantId, {{"name", "Control Policy"}, {"stage_plan", {100}}, {"two_person_approval", false}});
  REQUIRE(policy.has_value());
  const auto signingKeyId = edgefleet::shared::Uuid::generate().str();
  const auto artifactId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(app.storage()->execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519','fixture',?,?,datetime('now'),datetime('now'))", {signingKeyId, tenantId, "control-key", "control-fingerprint", "test"}));
  REQUIRE(app.storage()->execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready','target.bin','fixture',11,'digest', '{}','fixture',?,?,datetime('now'),datetime('now'))", {artifactId, tenantId, "target", "1", "m1", "x86_64", signingKeyId, "test"}));
  const auto running = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", artifactId}, {"policy_id", policy->at("id")}, {"name", "Running Release"}});
  REQUIRE(running.has_value());
  REQUIRE(app.storage()->execute("UPDATE releases SET status='running',version=1,frozen_policy_json='{}' WHERE tenant_id=? AND id=?", {tenantId, running->at("id").get<std::string>()}));
  std::array<int, 2> statuses{};
    { auto control = request("POST", "/api/releases/" + running->at("id").get<std::string>() + "/pause", R"({"expected_version":1,"reason":"first operator pause"})", apiKey); control.headers["idempotency-key"] = "race-first"; statuses[0] = app.handle(control).status; }
    { auto control = request("POST", "/api/releases/" + running->at("id").get<std::string>() + "/pause", R"({"expected_version":1,"reason":"second operator pause"})", apiKey); control.headers["idempotency-key"] = "race-second"; statuses[1] = app.handle(control).status; }
    REQUIRE(((statuses[0] == 200 && statuses[1] == 409) || (statuses[0] == 409 && statuses[1] == 200)));
  REQUIRE(app.storage()->query("SELECT id FROM evidence_events WHERE tenant_id=? AND event_type='release.pause'", {tenantId}).size() == 1);

  for (const auto& adapter : {"iot_rest_v1", "notification_hub_v1", "workflow_manual_v1"}) {
    REQUIRE(app.storage()->execute("INSERT INTO integration_configs(id,tenant_id,adapter_type,enabled,required_for_promotion,endpoint_base_url,secret_ref,settings_json,health_status,created_at,updated_at) VALUES(?,?,?,1,0,'http://127.0.0.1:1','OUTAGE_KEY','{\"fixture_mode\":false}','unhealthy',datetime('now'),datetime('now'))", {edgefleet::shared::Uuid::generate().str(), tenantId, adapter}));
  }
  auto abortDuringOutage = request("POST", "/api/releases/" + running->at("id").get<std::string>() + "/abort", R"({"expected_version":2,"reason":"abort while optional adapters are unavailable"})", apiKey);
  abortDuringOutage.headers["idempotency-key"] = "abort-during-adapter-outage";
  REQUIRE(app.handle(abortDuringOutage).status == 200);
  const auto rollbackRelease = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", artifactId}, {"rollback_artifact_id", artifactId}, {"policy_id", policy->at("id")}, {"name", "Rollback During Outage"}});
  REQUIRE(rollbackRelease.has_value());
  REQUIRE(app.storage()->execute("UPDATE releases SET status='running',frozen_rollback_json='{}' WHERE tenant_id=? AND id=?", {tenantId, rollbackRelease->at("id").get<std::string>()}));
  auto rollbackDuringOutage = request("POST", "/api/releases/" + rollbackRelease->at("id").get<std::string>() + "/rollback", R"({"expected_version":1,"reason":"rollback while optional adapters are unavailable"})", apiKey);
  rollbackDuringOutage.headers["idempotency-key"] = "rollback-during-adapter-outage";
  REQUIRE(app.handle(rollbackDuringOutage).status == 200);
  REQUIRE(app.storage()->getRelease(tenantId, rollbackRelease->at("id").get<std::string>())->at("status") == "rolling_back");

  const auto draft = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", artifactId}, {"policy_id", policy->at("id")}, {"name", "Cancellable Release"}});
  REQUIRE(draft.has_value());
  auto cancel = request("POST", "/api/releases/" + draft->at("id").get<std::string>() + "/cancel", R"({"expected_version":1,"reason":"cancel before any device command"})", apiKey);
  cancel.headers["idempotency-key"] = "pre-run-cancel";
  REQUIRE(app.handle(cancel).status == 200);
  REQUIRE(app.storage()->query("SELECT id FROM rollout_commands WHERE tenant_id=? AND release_id=?", {tenantId, draft->at("id").get<std::string>()}).empty());
  REQUIRE(app.storage()->query("SELECT id FROM approval_requests WHERE tenant_id=? AND release_id=? AND status='superseded'", {tenantId, draft->at("id").get<std::string>()}).empty());
}

TEST_CASE("gate overrides are bound, two-person, expiring, and restart observation", "[integration][safety][override]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-gate-override-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  config.minObservationSeconds = 2;
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Override Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto setupBody = nlohmann::json::parse(setup.body);
  const auto adminKey = setupBody.at("api_key").get<std::string>();
  const auto tenantId = setupBody.at("tenant").at("id").get<std::string>();
  const auto fleet = app.storage()->createFleet(tenantId, "override-fleet", "Override Fleet", "production");
  const auto policy = app.storage()->createPolicy(tenantId, {{"name", "Override Policy"}, {"stage_plan", {100}}, {"two_person_approval", true}});
  REQUIRE(fleet.has_value());
  REQUIRE(policy.has_value());
  const auto keyId = edgefleet::shared::Uuid::generate().str();
  const auto artifactId = edgefleet::shared::Uuid::generate().str();
  const auto releaseId = edgefleet::shared::Uuid::generate().str();
  const auto stageId = edgefleet::shared::Uuid::generate().str();
  const auto evaluationId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(app.storage()->execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519','fixture',?,?,datetime('now'),datetime('now'))", {keyId, tenantId, "override-key", "override-fingerprint", "test"}));
  REQUIRE(app.storage()->execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready','target.bin','fixture',11,'override-digest','{}','fixture',?,?,datetime('now'),datetime('now'))", {artifactId, tenantId, "override-target", "1", "m1", "x86_64", keyId, "test"}));
  const auto release = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", artifactId}, {"policy_id", policy->at("id")}, {"name", "Override Release"}});
  REQUIRE(release.has_value());
  REQUIRE(app.storage()->execute("UPDATE releases SET id=?,status='paused',version=7,current_stage_ordinal=1,frozen_policy_json='{}',membership_digest='frozen-membership' WHERE tenant_id=? AND id=?", {releaseId, tenantId, release->at("id").get<std::string>()}));
  REQUIRE(app.storage()->execute("INSERT INTO release_stages(id,tenant_id,release_id,ordinal,target_percentage,status,eligible_count,created_at,updated_at) VALUES(?,?,?,1,100,'active',0,datetime('now'),datetime('now'))", {stageId, tenantId, releaseId}));
  REQUIRE(app.storage()->execute("INSERT INTO health_gate_evaluations(id,tenant_id,release_id,stage_id,decision,sample_window_start,sample_window_end,sample_count,eligible_device_count,fresh_device_count,metrics_json,failed_gates_json,evidence_digest,evaluated_at) VALUES(?,?,?,?,'pause',datetime('now','-1 minute'),datetime('now'),0,0,0,'{}','[\"install_failure_rate\"]','override-evidence-1',datetime('now'))", {evaluationId, tenantId, releaseId, stageId}));

  auto override = request("POST", "/api/releases/" + releaseId + "/gates/" + evaluationId + "/override", R"({"expected_version":7,"reason":"bounded emergency review"})", adminKey);
  override.headers["idempotency-key"] = "override-request";
  const auto requested = app.handle(override);
    REQUIRE(requested.status == 202);
  const auto approvalId = nlohmann::json::parse(requested.body).at("id").get<std::string>();
  const auto approval = app.storage()->query("SELECT gate_evaluation_id,captured_release_version,evidence_digest,expires_at FROM approval_requests WHERE tenant_id=? AND id=?", {tenantId, approvalId});
  REQUIRE(approval.size() == 1);
  REQUIRE(approval.front().at("gate_evaluation_id") == evaluationId);
  REQUIRE(approval.front().at("captured_release_version") == 7);
  REQUIRE(approval.front().at("evidence_digest") == "override-evidence-1");
  const auto expiryMinutes = app.storage()->query("SELECT (julianday(expires_at)-julianday('now'))*24*60 AS minutes FROM approval_requests WHERE tenant_id=? AND id=?", {tenantId, approvalId});
  REQUIRE(expiryMinutes.size() == 1);
  REQUIRE(expiryMinutes.front().at("minutes").get<double>() > 29.0);
  REQUIRE(expiryMinutes.front().at("minutes").get<double>() < 31.0);

  auto selfApprove = request("POST", "/api/approvals/" + approvalId + "/approve", R"({"reason":"self approval is forbidden"})", adminKey);
  selfApprove.headers["idempotency-key"] = "override-self-approve";
  REQUIRE(app.handle(selfApprove).status == 403);
  auto credential = request("POST", "/api/credentials", R"({"label":"override-approver","role":"approver"})", adminKey);
  credential.headers["idempotency-key"] = "override-approver-create";
  const auto approver = nlohmann::json::parse(app.handle(credential).body);
  auto approve = request("POST", "/api/approvals/" + approvalId + "/approve", R"({"reason":"second operator reviewed bounded override"})", approver.at("api_key").get<std::string>());
  approve.headers["idempotency-key"] = "override-approve";
  REQUIRE(app.handle(approve).status == 200);
  const auto afterApproval = app.storage()->getRelease(tenantId, releaseId);
  REQUIRE(afterApproval.has_value());
  REQUIRE(afterApproval->at("status") == "running");
  REQUIRE(afterApproval->at("version") == 8);
  REQUIRE(app.storage()->query("SELECT id FROM release_stages WHERE tenant_id=? AND release_id=? AND status='active' AND observation_ends_at IS NOT NULL AND gate_decision_json IS NULL", {tenantId, releaseId}).size() == 1);

  const auto expiredEvaluationId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(app.storage()->execute("INSERT INTO health_gate_evaluations(id,tenant_id,release_id,stage_id,decision,sample_window_start,sample_window_end,sample_count,eligible_device_count,fresh_device_count,metrics_json,failed_gates_json,evidence_digest,evaluated_at) VALUES(?,?,?,?,'pause',datetime('now','-1 minute'),datetime('now'),0,0,0,'{}','[]','override-evidence-2',datetime('now'))", {expiredEvaluationId, tenantId, releaseId, stageId}));
  auto expiredOverride = request("POST", "/api/releases/" + releaseId + "/gates/" + expiredEvaluationId + "/override", R"({"expected_version":8,"reason":"test expiry"})", adminKey);
  expiredOverride.headers["idempotency-key"] = "override-expired-request";
  const auto expiredRequested = app.handle(expiredOverride);
  REQUIRE(expiredRequested.status == 202);
  const auto expiredApprovalId = nlohmann::json::parse(expiredRequested.body).at("id").get<std::string>();
  REQUIRE(app.storage()->execute("UPDATE approval_requests SET expires_at=datetime('now','-1 minute') WHERE tenant_id=? AND id=?", {tenantId, expiredApprovalId}));
  auto expiredApprove = request("POST", "/api/approvals/" + expiredApprovalId + "/approve", R"({"reason":"expired override"})", approver.at("api_key").get<std::string>());
  expiredApprove.headers["idempotency-key"] = "override-expired-approve";
  REQUIRE(app.handle(expiredApprove).status == 409);
}

TEST_CASE("stale required IoT evidence keeps a healthy-looking stage from promoting", "[integration][safety][iot]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-iot-gate-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"IoT Gate Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto setupBody = nlohmann::json::parse(setup.body);
  const auto apiKey = setupBody.at("api_key").get<std::string>();
  const auto tenantId = setupBody.at("tenant").at("id").get<std::string>();
  const auto fleet = app.storage()->createFleet(tenantId, "iot-fleet", "IoT Fleet", "production");
  const auto device = fleet.has_value() ? app.storage()->createDevice(tenantId, fleet->at("id"), {{"stable_key", "iot-device"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}}, "device-hash") : std::nullopt;
  const auto policy = app.storage()->createPolicy(tenantId, {{"name", "IoT Required Policy"}, {"stage_plan", {100}}, {"two_person_approval", false}, {"require_iot_evidence", true}, {"health_gates", {{"fresh_device_coverage", 0.5}, {"convergence_rate", 0.5}}}});
  REQUIRE(fleet.has_value());
  REQUIRE(device.has_value());
  REQUIRE(policy.has_value());
  const auto keyId = edgefleet::shared::Uuid::generate().str();
  const auto artifactId = edgefleet::shared::Uuid::generate().str();
  const auto releaseId = edgefleet::shared::Uuid::generate().str();
  const auto stageId = edgefleet::shared::Uuid::generate().str();
  const auto assignmentId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(app.storage()->execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519','fixture',?,?,datetime('now'),datetime('now'))", {keyId, tenantId, "iot-key", "iot-fingerprint", "test"}));
  REQUIRE(app.storage()->execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready','target.bin','fixture',11,'iot-digest','{}','fixture',?,?,datetime('now'),datetime('now'))", {artifactId, tenantId, "iot-target", "1", "m1", "x86_64", keyId, "test"}));
  const auto release = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", artifactId}, {"policy_id", policy->at("id")}, {"name", "IoT Release"}});
  REQUIRE(release.has_value());
  REQUIRE(app.storage()->execute("UPDATE releases SET id=?,status='running',version=1,current_stage_ordinal=1,frozen_policy_json=?,membership_digest='iot-membership' WHERE tenant_id=? AND id=?", {releaseId, edgefleet::shared::CanonicalJson::serialize({{"require_iot_evidence", true}, {"health_gates", {{"fresh_device_coverage", 0.5}, {"convergence_rate", 0.5}}}}), tenantId, release->at("id").get<std::string>()}));
  REQUIRE(app.storage()->execute("INSERT INTO release_stages(id,tenant_id,release_id,ordinal,target_percentage,status,eligible_count,assigned_count,created_at,updated_at) VALUES(?,?,?,1,100,'active',1,1,datetime('now'),datetime('now'))", {stageId, tenantId, releaseId}));
  REQUIRE(app.storage()->execute("INSERT INTO release_assignments(id,tenant_id,release_id,stage_id,device_id,desired_artifact_id,desired_generation,state,latest_report_sequence,updated_at) VALUES(?,?,?,?,?,?,1,'converged',1,datetime('now'))", {assignmentId, tenantId, releaseId, stageId, device->at("id").get<std::string>(), artifactId}));
  REQUIRE(app.storage()->execute("UPDATE devices SET last_seen_at=datetime('now'),observed_generation=1,observed_artifact_digest='iot-digest' WHERE tenant_id=? AND id=?", {tenantId, device->at("id").get<std::string>()}));
  REQUIRE(app.storage()->execute("INSERT INTO integration_configs(id,tenant_id,adapter_type,enabled,required_for_promotion,endpoint_base_url,secret_ref,settings_json,health_status,created_at,updated_at) VALUES(?,?, 'iot_rest_v1',1,1,'http://fixture','IOT_KEY','{}','healthy',datetime('now'),datetime('now'))", {edgefleet::shared::Uuid::generate().str(), tenantId}));
  REQUIRE(app.storage()->execute("INSERT INTO health_samples(id,tenant_id,release_id,stage_id,device_id,source,source_event_id,metric_name,metric_value,unit,observed_at,received_at,freshness_state,created_at) VALUES(?,?,?,?,?,'iot_rest_v1','stale-event','availability',1.0,'ratio',datetime('now','-1 hour'),datetime('now'),'stale',datetime('now'))", {edgefleet::shared::Uuid::generate().str(), tenantId, releaseId, stageId, device->at("id").get<std::string>()}));

  auto evaluate = request("POST", "/api/releases/" + releaseId + "/gates/evaluate", R"({"stage_ordinal":1})", apiKey);
  evaluate.headers["idempotency-key"] = "iot-stale-evaluation";
  const auto response = app.handle(evaluate);
    REQUIRE(response.status == 201);
  const auto body = nlohmann::json::parse(response.body);
  REQUIRE(body.at("decision") == "insufficient_evidence");
  REQUIRE(std::find(body.at("failed_gates").begin(), body.at("failed_gates").end(), "required_iot_evidence") != body.at("failed_gates").end());
  REQUIRE(app.storage()->getRelease(tenantId, releaseId)->at("status") == "running");
  REQUIRE(app.storage()->execute("DELETE FROM integration_configs WHERE tenant_id=? AND adapter_type='iot_rest_v1'", {tenantId}));
  auto unavailable = request("POST", "/api/releases/" + releaseId + "/gates/evaluate", R"({"stage_ordinal":1})", apiKey);
  unavailable.headers["idempotency-key"] = "iot-unavailable-evaluation";
  const auto unavailableResponse = app.handle(unavailable);
  REQUIRE(unavailableResponse.status == 201);
  const auto unavailableBody = nlohmann::json::parse(unavailableResponse.body);
  REQUIRE(std::find(unavailableBody.at("failed_gates").begin(), unavailableBody.at("failed_gates").end(), "required_iot_evidence") != unavailableBody.at("failed_gates").end());
}

TEST_CASE("signing-key compromise blocks artifacts, pauses releases, and prevents unsafe rollback", "[integration][safety][security]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-key-compromise-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Key Compromise Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto setupBody = nlohmann::json::parse(setup.body);
  const auto apiKey = setupBody.at("api_key").get<std::string>();
  const auto tenantId = setupBody.at("tenant").at("id").get<std::string>();
  const auto fleet = app.storage()->createFleet(tenantId, "security-fleet", "Security Fleet", "production");
  const auto policy = app.storage()->createPolicy(tenantId, {{"name", "Security Policy"}, {"stage_plan", {100}}, {"two_person_approval", false}, {"rollback_requirement", "required"}});
  REQUIRE(fleet.has_value());
  REQUIRE(policy.has_value());
  REQUIRE(app.storage()->execute("UPDATE rollout_policies SET status='active' WHERE tenant_id=? AND id=?", {tenantId, policy->at("id").get<std::string>()}));
  const auto keyId = edgefleet::shared::Uuid::generate().str();
  const auto targetId = edgefleet::shared::Uuid::generate().str();
  const auto rollbackId = edgefleet::shared::Uuid::generate().str();
  const auto releaseId = edgefleet::shared::Uuid::generate().str();
  REQUIRE(app.storage()->execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,'ed25519','fixture',?,?,datetime('now'),datetime('now'))", {keyId, tenantId, "compromised-key", "compromised-fingerprint", "test"}));
  for (const auto& artifactId : {targetId, rollbackId}) {
    REQUIRE(app.storage()->execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,? ,?,'ready','artifact.bin','fixture',11,?,'{}','fixture',?,?,datetime('now'),datetime('now'))", {artifactId, tenantId, artifactId == targetId ? "target" : "rollback", artifactId == targetId ? "1" : "0", "m1", "x86_64", artifactId + "-digest", keyId, "test"}));
  }
  const auto release = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", targetId}, {"rollback_artifact_id", rollbackId}, {"policy_id", policy->at("id")}, {"name", "Compromise Release"}});
  REQUIRE(release.has_value());
  REQUIRE(app.storage()->execute("UPDATE releases SET id=?,status='running',version=1,frozen_policy_json='{}',frozen_rollback_json='{\"artifact_id\":\"rollback\"}' WHERE tenant_id=? AND id=?", {releaseId, tenantId, release->at("id").get<std::string>()}));

  auto compromise = request("POST", "/api/artifact-signing-keys/" + keyId + "/compromise", R"({"reason":"signing material was exposed"})", apiKey);
  compromise.headers["idempotency-key"] = "compromise-key";
  const auto compromised = app.handle(compromise);
  REQUIRE(compromised.status == 200);
  REQUIRE(app.storage()->query("SELECT status,validation_error FROM artifacts WHERE tenant_id=? AND signature_key_id=? ORDER BY id", {tenantId, keyId}).size() == 2);
  REQUIRE(app.storage()->query("SELECT id FROM artifacts WHERE tenant_id=? AND signature_key_id=? AND status='blocked' AND validation_error='SIGNING_KEY_COMPROMISED'", {tenantId, keyId}).size() == 2);
  const auto paused = app.storage()->getRelease(tenantId, releaseId);
  REQUIRE(paused.has_value());
  REQUIRE(paused->at("status") == "paused");
  REQUIRE(paused->at("status_reason_code") == "SIGNING_KEY_COMPROMISED");

  const auto blockedRelease = app.storage()->createRelease(tenantId, {{"fleet_id", fleet->at("id")}, {"artifact_id", targetId}, {"rollback_artifact_id", rollbackId}, {"policy_id", policy->at("id")}, {"name", "Blocked Target Release"}});
  REQUIRE(blockedRelease.has_value());
  auto validateBlocked = request("POST", "/api/releases/" + blockedRelease->at("id").get<std::string>() + "/validate", R"({"expected_version":1})", apiKey);
  validateBlocked.headers["idempotency-key"] = "validate-blocked-target";
  REQUIRE(nlohmann::json::parse(app.handle(validateBlocked).body).at("error").at("code") == "ARTIFACT_NOT_READY");

  auto rollback = request("POST", "/api/releases/" + releaseId + "/rollback", R"({"expected_version":2,"reason":"attempt unsafe rollback"})", apiKey);
  rollback.headers["idempotency-key"] = "unsafe-rollback-after-compromise";
  const auto rollbackResponse = app.handle(rollback);
  REQUIRE(rollbackResponse.status == 422);
  REQUIRE(nlohmann::json::parse(rollbackResponse.body).at("error").at("code") == "ROLLBACK_ARTIFACT_NOT_READY");
}

TEST_CASE("artifact uploads fail closed when the configured free-space reserve is exhausted", "[integration][safety][storage]") {
  const auto root = std::filesystem::temp_directory_path() / ("edgefleet-low-space-" + edgefleet::shared::Uuid::generate().str());
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = (root / "edgefleet.db").string();
  config.artifactStorePath = (root / "artifacts").string();
  config.artifactTempPath = (root / "tmp").string();
  config.artifactMinFreeBytes = (std::numeric_limits<std::int64_t>::max)();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Low Space Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto apiKey = nlohmann::json::parse(setup.body).at("api_key").get<std::string>();
  auto createKey = request("POST", "/api/artifact-signing-keys", "{}", apiKey);
  createKey.headers["idempotency-key"] = "low-space-key";
  const auto key = nlohmann::json::parse(app.handle(createKey).body);
  const auto manifest = nlohmann::json{{"artifact", "low-space"}};
  const auto manifestJson = edgefleet::shared::CanonicalJson::serialize(manifest);
  const auto digest = edgefleet::shared::DigestService::sha256Hex(manifestJson);
  const auto signedPayload = edgefleet::shared::CanonicalJson::serialize({{"digest", digest}, {"size_bytes", manifestJson.size()}, {"name", "low-space"}, {"version", "1"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}, {"manifest", manifest}});
  const auto signature = edgefleet::domain::ArtifactSigner::sign(signedPayload, key.at("private_key_pem").get<std::string>());
  REQUIRE(signature.ok());
  auto upload = request("POST", "/api/artifacts", nlohmann::json{{"name", "low-space"}, {"version", "1"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}, {"manifest", manifest}, {"signature", *signature.value}, {"signing_key_id", key.at("id")}}.dump(), apiKey);
  upload.headers["idempotency-key"] = "low-space-upload";
  const auto response = app.handle(upload);
  REQUIRE(response.status == 507);
  REQUIRE(nlohmann::json::parse(response.body).at("error").at("code") == "ARTIFACT_STORE_LOW_SPACE");
}

TEST_CASE("setup, authentication, idempotency, and tenant isolation use real SQLite", "[integration]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-http-test-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));

  const auto setup = app.handle(request("POST", "/api/setup", R"({"name":"Tenant A","legal_name":"Tenant A Ltd"})"));
  REQUIRE(setup.status == 201);
  const auto setupBody = nlohmann::json::parse(setup.body);
  const auto apiKey = setupBody.at("api_key").get<std::string>();
  const auto tenantId = setupBody.at("tenant").at("id").get<std::string>();

  const auto missingKey = app.handle(request("GET", "/api/fleets"));
  REQUIRE(missingKey.status == 401);
  REQUIRE(nlohmann::json::parse(missingKey.body).at("error").contains("trace_id"));
  auto tracedRequest = request("GET", "/api/fleets");
  tracedRequest.headers["traceparent"] = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";
  const auto tracedResponse = app.handle(tracedRequest);
  REQUIRE(tracedResponse.status == 401);
  REQUIRE(nlohmann::json::parse(tracedResponse.body).at("error").at("trace_id") == "0123456789abcdef0123456789abcdef");
  REQUIRE(tracedResponse.headers.at("X-Trace-ID") == "0123456789abcdef0123456789abcdef");

  const auto missingIdempotency = app.handle(request("POST", "/api/fleets", R"({"name":"Fleet A","slug":"fleet-a"})", apiKey));
  REQUIRE(missingIdempotency.status == 428);

  auto create = request("POST", "/api/fleets", R"({"name":"Fleet A","slug":"fleet-a","environment":"production"})", apiKey);
  create.headers["idempotency-key"] = "fleet-a-create";
  const auto created = app.handle(create);
  REQUIRE(created.status == 201);
  const auto replayed = app.handle(create);
  REQUIRE(replayed.status == created.status);
  REQUIRE(replayed.body == created.body);

  const auto fleets = app.handle(request("GET", "/api/fleets", {}, apiKey));
  REQUIRE(fleets.status == 200);
  REQUIRE(nlohmann::json::parse(fleets.body).at("items").size() == 1);
  REQUIRE(nlohmann::json::parse(fleets.body).at("items").at(0).at("tenant_id") == tenantId);

  const auto integrationsPage = app.handle(request("GET", "/app/settings/integrations", {}, apiKey));
  REQUIRE(integrationsPage.status == 200);
  REQUIRE(integrationsPage.body.find("Configure an adapter") != std::string::npos);
  REQUIRE(integrationsPage.body.find("skip-link") != std::string::npos);
  REQUIRE(integrationsPage.body.find("/api/integrations") == std::string::npos);

  auto viewerCredential = request("POST", "/api/credentials", R"({"label":"viewer","role":"viewer"})", apiKey);
  viewerCredential.headers["idempotency-key"] = "viewer-credential";
  const auto viewerResponse = app.handle(viewerCredential);
  REQUIRE(viewerResponse.status == 201);
  const auto viewerKey = nlohmann::json::parse(viewerResponse.body).at("api_key").get<std::string>();
  const auto viewerSettings = app.handle(request("GET", "/app/settings/integrations", {}, viewerKey));
  REQUIRE(viewerSettings.status == 403);
  const auto viewerTenant = app.handle(request("GET", "/api/tenants/me", {}, viewerKey));
  REQUIRE(viewerTenant.status == 200);
  REQUIRE(nlohmann::json::parse(viewerTenant.body).at("id") == tenantId);

}

TEST_CASE("operator roles reach the shared read matrix and cannot use device protocol routes", "[integration][security][role-matrix]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-role-matrix-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Matrix Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto adminKey = nlohmann::json::parse(setup.body).at("api_key").get<std::string>();
  const std::vector<std::pair<std::string, std::string>> roles{{"release_manager", "matrix-manager"}, {"approver", "matrix-approver"}, {"viewer", "matrix-viewer"}};
  std::map<std::string, std::string> keys{{"admin", adminKey}};
  for (const auto& [role, idempotency] : roles) {
    auto credential = request("POST", "/api/credentials", nlohmann::json{{"label", role}, {"role", role}}.dump(), adminKey);
    credential.headers["idempotency-key"] = idempotency;
    const auto response = app.handle(credential);
    REQUIRE(response.status == 201);
    keys[role] = nlohmann::json::parse(response.body).at("api_key").get<std::string>();
  }

  const std::vector<std::string> readRoutes{"/api/tenants/me", "/api/fleets", "/api/devices", "/api/artifact-signing-keys", "/api/artifacts", "/api/policies",
                                            "/api/releases", "/api/approvals", "/api/simulations", "/api/benchmarks", "/api/replays", "/api/evidence",
                                            "/api/notices", "/api/integrations"};
  for (const auto& [role, key] : keys) {
    for (const auto& route : readRoutes) {
      const auto response = app.handle(request("GET", route, {}, key));
      REQUIRE(response.status == 200);
    }
    const auto deviceRoute = app.handle(request("GET", "/api/agent/v1/desired-state", {}, key));
    REQUIRE(deviceRoute.status == 403);
  }

  const std::vector<std::pair<std::string, std::string>> mutationRoutes{{"/api/fleets", "POST"}, {"/api/policies", "POST"}, {"/api/releases", "POST"}, {"/api/simulations", "POST"}, {"/api/evidence", "POST"}};
  for (const auto& [role, key] : keys) {
    for (const auto& [route, method] : mutationRoutes) {
      auto mutation = request(method, route, "{}", key);
      mutation.headers["idempotency-key"] = "matrix-" + role + "-" + route.substr(5);
      const auto response = app.handle(mutation);
      const bool allowed = role == "admin" || (role == "release_manager" && (route == "/api/fleets" || route == "/api/policies" || route == "/api/releases" || route == "/api/simulations"));
      INFO("role=" << role << " route=" << route << " status=" << response.status);
      REQUIRE((allowed ? response.status != 403 : response.status == 403));
    }
  }
}

TEST_CASE("every registered operator route has a real handler allow or deny result", "[integration][security][role-matrix]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-route-matrix-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Registered Route Tenant"})"));
  REQUIRE(setup.status == 201);
  const auto adminKey = nlohmann::json::parse(setup.body).at("api_key").get<std::string>();
  auto fleetCreate = request("POST", "/api/fleets", R"({"name":"Route probe fleet","slug":"route-probe","environment":"development"})", adminKey);
  fleetCreate.headers["idempotency-key"] = "route-probe-fleet";
  const auto fleetCreateResponse = app.handle(fleetCreate);
  REQUIRE(fleetCreateResponse.status == 201);
  const auto fleetId = nlohmann::json::parse(fleetCreateResponse.body).at("id").get<std::string>();
  std::map<std::string, std::string> keys{{"admin", adminKey}};
  for (const auto& role : {std::string("release_manager"), std::string("approver"), std::string("viewer")}) {
    auto credential = request("POST", "/api/credentials", nlohmann::json{{"label", "route-" + role}, {"role", role}}.dump(), adminKey);
    credential.headers["idempotency-key"] = "route-credential-" + role;
    const auto response = app.handle(credential);
    REQUIRE(response.status == 201);
    keys[role] = nlohmann::json::parse(response.body).at("api_key").get<std::string>();
  }
  const auto cookieValue = [](const std::string& header, const std::string& name) {
    const auto marker = name + "=";
    const auto start = header.find(marker);
    if (start == std::string::npos) return std::string{};
    const auto valueStart = start + marker.size();
    const auto valueEnd = header.find_first_of(";\r\n", valueStart);
    return header.substr(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
  };
  std::map<std::string, std::pair<std::string, std::string>> sessions;
  for (const auto& [roleName, key] : keys) {
    const auto session = app.handle(request("POST", "/auth/session", nlohmann::json{{"api_key", key}}.dump(), key));
    REQUIRE(session.status == 204);
    const auto setCookie = session.headers.at("Set-Cookie");
    sessions[roleName] = {cookieValue(setCookie, "edgefleet_session"), cookieValue(setCookie, "edgefleet_csrf")};
    REQUIRE_FALSE(sessions[roleName].first.empty());
    REQUIRE_FALSE(sessions[roleName].second.empty());
  }
  const auto concretePath = [&fleetId](std::string path) {
    for (const auto& replacement : {std::pair<std::string, std::string>{"{id}", "missing-id"}, {"{evaluation_id}", "missing-evaluation"}, {"{digest}", "missing-digest"}, {"{adapter}", "unknown-adapter"}}) {
      std::size_t position = 0;
      while ((position = path.find(replacement.first, position)) != std::string::npos) {
        path.replace(position, replacement.first.size(), replacement.second);
        position += replacement.second.size();
      }
    }
    const auto fleetMarker = path.find("/api/fleets/missing-id");
    if (fleetMarker != std::string::npos) path.replace(fleetMarker + std::string("/api/fleets/").size(), std::string("missing-id").size(), fleetId);
    return path;
  };
  for (const auto& route : edgefleet::web::routePermissions()) {
    if (route.role == "device") continue;
    const auto path = concretePath(route.prefix);
    for (const auto& [roleName, ignoredKey] : keys) {
      (void)ignoredKey;
      edgefleet::shared::TenantContext context;
      context.tenantId = "route-matrix";
      context.actorId = roleName;
      context.role = edgefleet::shared::roleFromString(roleName).value();
      context.authenticated = true;
      const auto expectedAllowed = route.role == "admin" && route.permission != "read" ? roleName == "admin" : edgefleet::web::requireRole(context, route.resource, route.permission);
      auto probe = request(route.method, path, route.method == "GET" ? std::string{} : std::string("{}"));
      probe.headers["cookie"] = "edgefleet_session=" + sessions.at(roleName).first + "; edgefleet_csrf=" + sessions.at(roleName).second;
      probe.headers["x-peer-address"] = "route-matrix-" + roleName;
      if (route.method != "GET") {
        probe.headers["idempotency-key"] = "route-probe-" + roleName + "-" + route.method + "-" + path;
        probe.headers["x-csrf-token"] = sessions.at(roleName).second;
      }
      const auto response = app.handle(probe);
      INFO("role=" << roleName << " method=" << route.method << " path=" << path << " status=" << response.status);
      REQUIRE((expectedAllowed ? response.status != 403 : response.status == 403));
    }
  }
}

TEST_CASE("a signed artifact can be frozen into a release and controlled with four eyes", "[integration]") {
  const auto db = std::filesystem::temp_directory_path() / ("edgefleet-release-test-" + edgefleet::shared::Uuid::generate().str() + ".db");
  auto config = edgefleet::shared::Config::defaultsForTests();
  config.sqlitePath = db.string();
  config.environment = "test";
  edgefleet::application::ControlPlane app(config);
  REQUIRE(app.initialize((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto setup = app.handle(request("POST", "/api/tenants/register", R"({"name":"Release Tenant"})"));
  const auto parsedSetup = nlohmann::json::parse(setup.body);
  const auto apiKey = parsedSetup.at("api_key").get<std::string>();

  auto createFleet = request("POST", "/api/fleets", R"({"name":"Production","slug":"production","environment":"production"})", apiKey);
  createFleet.headers["idempotency-key"] = "release-fleet";
  const auto fleet = nlohmann::json::parse(app.handle(createFleet).body);
  const auto fleetId = fleet.at("id").get<std::string>();

  auto createDevice = request("POST", "/api/fleets/" + fleetId + "/devices", R"({"stable_key":"device-1","hardware_model":"m1","architecture":"x86_64","device_secret":"device-secret"})", apiKey);
  createDevice.headers["idempotency-key"] = "release-device";
  const auto deviceResponse = app.handle(createDevice);
  REQUIRE(deviceResponse.status == 201);

  auto createKey = request("POST", "/api/artifact-signing-keys", "{}", apiKey);
  createKey.headers["idempotency-key"] = "release-key";
  const auto key = nlohmann::json::parse(app.handle(createKey).body);
  const auto manifest = nlohmann::json{{"artifact", "release-v1"}, {"bytes", "fixture"}};
  const auto manifestJson = edgefleet::shared::CanonicalJson::serialize(manifest);
  const auto digest = edgefleet::shared::DigestService::sha256Hex(manifestJson);
  const auto signedPayload = edgefleet::shared::CanonicalJson::serialize({{"digest", digest}, {"size_bytes", manifestJson.size()}, {"name", "release"}, {"version", "1.0.0"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}, {"manifest", manifest}});
  const auto signature = edgefleet::domain::ArtifactSigner::sign(signedPayload, key.at("private_key_pem").get<std::string>());
  REQUIRE(signature.ok());
  auto createArtifact = request("POST", "/api/artifacts", nlohmann::json{{"name","release"},{"version","1.0.0"},{"hardware_model","m1"},{"architecture","x86_64"},{"manifest",manifest},{"signature",*signature.value},{"signing_key_id",key.at("id")}}.dump(), apiKey);
  createArtifact.headers["idempotency-key"] = "release-artifact";
  const auto artifactResponse = app.handle(createArtifact);
  REQUIRE(artifactResponse.status == 201);
  const auto artifactId = nlohmann::json::parse(artifactResponse.body).at("id").get<std::string>();
  auto createRollbackKey = request("POST", "/api/artifact-signing-keys", "{}", apiKey);
  createRollbackKey.headers["idempotency-key"] = "release-rollback-key";
  const auto rollbackKey = nlohmann::json::parse(app.handle(createRollbackKey).body);
  const auto rollbackManifest = nlohmann::json{{"artifact", "release-v0"}, {"bytes", "fixture"}};
  const auto rollbackManifestJson = edgefleet::shared::CanonicalJson::serialize(rollbackManifest);
  const auto rollbackDigest = edgefleet::shared::DigestService::sha256Hex(rollbackManifestJson);
  const auto rollbackSignedPayload = edgefleet::shared::CanonicalJson::serialize({{"digest", rollbackDigest}, {"size_bytes", rollbackManifestJson.size()}, {"name", "rollback"}, {"version", "0.9.0"}, {"hardware_model", "m1"}, {"architecture", "x86_64"}, {"manifest", rollbackManifest}});
  const auto rollbackSignature = edgefleet::domain::ArtifactSigner::sign(rollbackSignedPayload, rollbackKey.at("private_key_pem").get<std::string>());
  REQUIRE(rollbackSignature.ok());
  auto createRollback = request("POST", "/api/artifacts", nlohmann::json{{"name","rollback"},{"version","0.9.0"},{"hardware_model","m1"},{"architecture","x86_64"},{"manifest",rollbackManifest},{"signature",*rollbackSignature.value},{"signing_key_id",rollbackKey.at("id")}}.dump(), apiKey);
  createRollback.headers["idempotency-key"] = "release-rollback-artifact";
  const auto rollbackResponse = app.handle(createRollback);
  REQUIRE(rollbackResponse.status == 201);
  const auto rollbackArtifactId = nlohmann::json::parse(rollbackResponse.body).at("id").get<std::string>();

  auto createPolicy = request("POST", "/api/policies", R"({"name":"Safe","rollback_requirement":"required","stage_plan":[100]})", apiKey);
  createPolicy.headers["idempotency-key"] = "release-policy";
  const auto policyId = nlohmann::json::parse(app.handle(createPolicy).body).at("id").get<std::string>();
  auto activatePolicy = request("POST", "/api/policies/" + policyId + "/activate", R"({"reason":"activate policy for release test"})", apiKey);
  activatePolicy.headers["idempotency-key"] = "release-policy-activate";
  REQUIRE(app.handle(activatePolicy).status == 200);

  auto createRelease = request("POST", "/api/releases", nlohmann::json{{"name","Release 1"},{"fleet_id",fleetId},{"artifact_id",artifactId},{"rollback_artifact_id",rollbackArtifactId},{"policy_id",policyId}}.dump(), apiKey);
  createRelease.headers["idempotency-key"] = "release-create";
  const auto releaseResponse = app.handle(createRelease);
  REQUIRE(releaseResponse.status == 201);
  const auto releaseId = nlohmann::json::parse(releaseResponse.body).at("id").get<std::string>();
  auto validate = request("POST", "/api/releases/" + releaseId + "/validate", R"({"expected_version":1})", apiKey);
  validate.headers["idempotency-key"] = "release-validate";
  const auto validationResponse = app.handle(validate);
  REQUIRE(validationResponse.status == 200);
  auto missingReason = request("POST", "/api/releases/" + releaseId + "/submit", "{}", apiKey);
  missingReason.headers["idempotency-key"] = "release-submit-missing-reason";
  REQUIRE(app.handle(missingReason).status == 422);
  auto scheduleWithoutApproval = request("POST", "/api/releases/" + releaseId + "/schedule", R"({"expected_version":3,"scheduled_for":"2099-01-01T00:00:00Z","reason":"schedule after explicit approval"})", apiKey);
  scheduleWithoutApproval.headers["idempotency-key"] = "release-schedule-without-approval";
  REQUIRE(app.handle(scheduleWithoutApproval).status == 409);
  auto submit = request("POST", "/api/releases/" + releaseId + "/submit", R"({"expected_version":3,"reason":"request release start authorization"})", apiKey);
  submit.headers["idempotency-key"] = "release-submit";
  REQUIRE(app.handle(submit).status == 200);
  const auto approvals = nlohmann::json::parse(app.handle(request("GET", "/api/approvals", {}, apiKey)).body).at("items");
  REQUIRE(approvals.size() == 1);
  const auto approvalId = approvals.at(0).at("id").get<std::string>();
  auto credential = request("POST", "/api/credentials", R"({"label":"approver","role":"approver"})", apiKey);
  credential.headers["idempotency-key"] = "approver-create";
  const auto approverKey = nlohmann::json::parse(app.handle(credential).body).at("api_key").get<std::string>();
  auto approve = request("POST", "/api/approvals/" + approvalId + "/approve", R"({"reason":"second operator reviewed the release"})", approverKey);
  approve.headers["idempotency-key"] = "approval-decide";
  REQUIRE(app.handle(approve).status == 200);
  auto start = request("POST", "/api/releases/" + releaseId + "/start", R"({"expected_version":5,"reason":"start the approved release"})", apiKey);
  start.headers["idempotency-key"] = "release-start";
  const auto startResponse = app.handle(start);
  REQUIRE(startResponse.status == 200);
  const auto assignments = nlohmann::json::parse(app.handle(request("GET", "/api/releases/" + releaseId + "/assignments", {}, apiKey)).body).at("items");
  REQUIRE(assignments.size() == 1);

  auto pause = request("POST", "/api/releases/" + releaseId + "/pause", R"({"expected_version":6,"reason":"pause for approval-path coverage"})", apiKey);
  pause.headers["idempotency-key"] = "release-pause";
  REQUIRE(app.handle(pause).status == 200);
  auto resume = request("POST", "/api/releases/" + releaseId + "/resume", R"({"expected_version":7,"reason":"resume after review"})", apiKey);
  resume.headers["idempotency-key"] = "release-resume";
  REQUIRE(app.handle(resume).status == 202);
  const auto resumeApproval = nlohmann::json::parse(app.handle(request("GET", "/api/approvals", {}, apiKey)).body).at("items").at(0).at("id").get<std::string>();
  auto approveResume = request("POST", "/api/approvals/" + resumeApproval + "/approve", R"({"reason":"second operator approved resume"})", approverKey);
  approveResume.headers["idempotency-key"] = "approval-resume";
  REQUIRE(app.handle(approveResume).status == 200);
  auto abort = request("POST", "/api/releases/" + releaseId + "/abort", R"({"expected_version":8,"reason":"abort after safety review"})", apiKey);
  abort.headers["idempotency-key"] = "release-abort";
  REQUIRE(app.handle(abort).status == 202);
  const auto abortApproval = nlohmann::json::parse(app.handle(request("GET", "/api/approvals", {}, apiKey)).body).at("items").at(0).at("id").get<std::string>();
  auto approveAbort = request("POST", "/api/approvals/" + abortApproval + "/approve", R"({"reason":"second operator approved abort"})", approverKey);
  approveAbort.headers["idempotency-key"] = "approval-abort";
  REQUIRE(app.handle(approveAbort).status == 200);
  const auto aborted = nlohmann::json::parse(app.handle(request("GET", "/api/releases/" + releaseId, {}, apiKey)).body);
  REQUIRE(aborted.at("status") == "aborting");
  const auto commands = app.storage()->query("SELECT command_type FROM rollout_commands WHERE tenant_id=? AND release_id=? ORDER BY issued_at", {aborted.at("tenant_id").get<std::string>(), releaseId});
  REQUIRE(std::any_of(commands.begin(), commands.end(), [](const auto& command) { return command.value("command_type", "") == "cancel"; }));
}
