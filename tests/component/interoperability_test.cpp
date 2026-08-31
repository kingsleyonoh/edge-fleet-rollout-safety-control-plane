#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>

#include "application/interoperability.hpp"
#include "infrastructure/integrations.hpp"
#include "shared/http_client_pool.hpp"
#include "web/http_server.hpp"

namespace {

std::string fixture(const char* name) {
  std::ifstream input(std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "fixtures" / "interoperability" / name, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("interoperability fixtures require strict v1 envelopes", "[component]") {
  const auto fleet = edgefleet::application::Interoperability::parseFleetCsv(fixture("fleet.csv"));
  REQUIRE(fleet.ok());
  REQUIRE(fleet.value->size() == 2);
  const auto evidence = edgefleet::application::Interoperability::parseEvidenceNdjson(fixture("evidence.ndjson"));
  REQUIRE(evidence.ok());
  REQUIRE(evidence.value->size() == 1);
  REQUIRE_FALSE(edgefleet::application::Interoperability::parseFleetCsv("stable_key,hardware_model,architecture,environment\na,m,x,production\na,m,x,production\n").ok());
  REQUIRE_FALSE(edgefleet::application::Interoperability::parseEvidenceNdjson("{\"schema_version\":\"v1\",\"event_type\":\"x\",\"aggregate_type\":\"y\",\"aggregate_id\":\"z\",\"payload\":{},\"extra\":true}\n").ok());
}

TEST_CASE("live adapter transport uses a bounded HTTP client and classifies health responses", "[component][integration]") {
  edgefleet::web::HttpServer server("127.0.0.1", 0, [](const edgefleet::web::HttpRequest& request) {
    REQUIRE(request.method == "GET");
    return edgefleet::web::HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
  });
  std::thread serverThread([&server] { server.start(); });
  for (int attempt = 0; attempt < 100 && server.boundPort() == 0; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(server.boundPort() != 0);
  edgefleet::shared::HttpClientPool clientPool(2);
  const auto result = edgefleet::infrastructure::AdapterContract::testConnection("notification_hub_v1", "http://127.0.0.1:" + std::to_string(server.boundPort()), "fixture-secret", {}, clientPool);
  REQUIRE(result.disposition == edgefleet::infrastructure::DeliveryDisposition::published);
  REQUIRE(result.statusCode == 200);
  server.stop();
  serverThread.join();
}

TEST_CASE("live adapter contracts use one bounded request per external operation", "[component][integration][http]") {
  std::mutex mutex;
  std::vector<std::string> requests;
  edgefleet::web::HttpServer server("127.0.0.1", 0, [&](const edgefleet::web::HttpRequest& request) {
    std::lock_guard lock(mutex);
    requests.push_back(request.method + " " + request.target);
    if (request.target == "/health") return edgefleet::web::HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
    if (request.target == "/api/events") return edgefleet::web::HttpResponse{202, "application/json", R"({"published":true})", {}};
    if (request.target == "/api/workflows/wf-1/execute") return edgefleet::web::HttpResponse{202, "application/json", R"({"execution_id":"exec-1","status":"queued"})", {}};
    if (request.target == "/api/readings") return edgefleet::web::HttpResponse{200, "application/json", R"({"readings":[{"device_id":"device-1","metric_name":"availability","value":1.0,"observed_at":"2026-08-29T00:00:00Z","source_event_id":"iot-1"}]})", {}};
    if (request.target == "/api/executions/exec-1") return edgefleet::web::HttpResponse{200, "application/json", R"({"status":"completed"})", {}};
    return edgefleet::web::HttpResponse{404, "application/json", R"({"error":"not found"})", {}};
  });
  std::thread serverThread([&server] { server.start(); });
  for (int attempt = 0; attempt < 100 && server.boundPort() == 0; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(server.boundPort() != 0);

  const auto endpoint = "http://127.0.0.1:" + std::to_string(server.boundPort());
  edgefleet::shared::HttpClientPool clientPool(2);
  const auto health = edgefleet::infrastructure::AdapterContract::testConnection("notification_hub_v1", endpoint, "secret", {}, clientPool);
  REQUIRE(health.disposition == edgefleet::infrastructure::DeliveryDisposition::published);
  const auto notification = edgefleet::infrastructure::AdapterContract::liveDelivery("notification_hub_v1", endpoint, "secret", {{"event_type", "release.paused"}}, "event-1", {}, clientPool);
  REQUIRE(notification.disposition == edgefleet::infrastructure::DeliveryDisposition::published);
  const auto workflow = edgefleet::infrastructure::AdapterContract::liveDelivery("workflow_manual_v1", endpoint, "secret", {{"event_type", "release.paused"}}, "event-2", {{"workflow_id", "wf-1"}}, clientPool);
  REQUIRE(workflow.disposition == edgefleet::infrastructure::DeliveryDisposition::published);
  REQUIRE(workflow.externalReference == "exec-1");
  const auto readings = edgefleet::infrastructure::AdapterContract::liveIotReadings(endpoint, "secret", "cursor-1", clientPool);
  REQUIRE(readings.ok());
  REQUIRE(readings.value->at("readings").size() == 1);
  const auto status = edgefleet::infrastructure::AdapterContract::liveWorkflowStatus(endpoint, "secret", "exec-1", clientPool);
  REQUIRE(status.ok());
  REQUIRE(*status.value == "completed");

  server.stop();
  serverThread.join();
  std::lock_guard lock(mutex);
  REQUIRE(std::count(requests.begin(), requests.end(), "GET /health") == 1);
  REQUIRE(std::count(requests.begin(), requests.end(), "POST /api/events") == 1);
  REQUIRE(std::count(requests.begin(), requests.end(), "POST /api/workflows/wf-1/execute") == 1);
  REQUIRE(std::count(requests.begin(), requests.end(), "GET /api/readings") == 1);
  REQUIRE(std::count(requests.begin(), requests.end(), "GET /api/executions/exec-1") == 1);
}
