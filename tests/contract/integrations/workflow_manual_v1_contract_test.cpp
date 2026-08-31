#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "infrastructure/integrations.hpp"
#include "shared/http_client_pool.hpp"
#include "web/http_server.hpp"

TEST_CASE("workflow_manual_v1 factory preserves ambiguous delivery safety", "[contract][integration]") {
  const auto adapter = edgefleet::infrastructure::createAdapter(edgefleet::infrastructure::AdapterType::workflow_manual_v1);
  REQUIRE(adapter);
  const auto ambiguous = adapter->fixtureDelivery({{"fixture_mode", true}, {"fixture_behavior", "timeout_after_write"}}, "workflow-contract", 0);
  REQUIRE(ambiguous.disposition == edgefleet::infrastructure::DeliveryDisposition::ambiguous_delivery);
  REQUIRE(ambiguous.errorCode == "AMBIGUOUS_DELIVERY");
  REQUIRE_FALSE(edgefleet::infrastructure::createAdapter("unknown_adapter"));
}

TEST_CASE("workflow_manual_v1 trigger stays non-authoritative and within the contract budget", "[contract][integration][performance]") {
  std::mutex mutex;
  std::map<std::string, int> executions;
  bool releaseMutationAttempted = false;
  edgefleet::web::HttpServer server("127.0.0.1", 0, [&](const edgefleet::web::HttpRequest& request) {
    if (request.target.starts_with("/api/releases")) releaseMutationAttempted = true;
    const auto idempotency = request.headers.find("idempotency-key");
    const auto apiKey = request.headers.find("x-api-key");
    if (request.method != "POST" || request.target != "/api/workflows/wf-1/execute" || idempotency == request.headers.end() || apiKey == request.headers.end() || apiKey->second != "contract-secret") {
      return edgefleet::web::HttpResponse{400, "application/json", R"({"error":"invalid workflow request"})", {}};
    }
    {
      std::lock_guard lock(mutex);
      ++executions[idempotency->second];
    }
    return edgefleet::web::HttpResponse{202, "application/json", R"({"execution_id":"exec-contract","status":"queued"})", {}};
  });
  std::thread serverThread([&server] { server.start(); });
  for (int attempt = 0; attempt < 100 && server.boundPort() == 0; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(server.boundPort() != 0);

  auto adapter = edgefleet::infrastructure::createAdapter("workflow_manual_v1");
  REQUIRE(adapter);
  edgefleet::shared::HttpClientPool clientPool(8);
  std::vector<double> latencies;
  latencies.reserve(100);
  for (int index = 0; index < 100; ++index) {
    const auto started = std::chrono::steady_clock::now();
    const auto result = adapter->liveDelivery("http://127.0.0.1:" + std::to_string(server.boundPort()), "contract-secret", {{"event_type", "release.paused"}},
                                              "workflow-contract-" + std::to_string(index), {{"workflow_id", "wf-1"}}, clientPool);
    latencies.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    REQUIRE(result.disposition == edgefleet::infrastructure::DeliveryDisposition::published);
    REQUIRE(result.statusCode == 202);
    REQUIRE(result.externalReference == "exec-contract");
  }
  server.stop();
  serverThread.join();
  std::sort(latencies.begin(), latencies.end());
  REQUIRE(latencies.at(94) < 5000.0);
  std::lock_guard lock(mutex);
  REQUIRE(executions.size() == 100);
  REQUIRE(std::all_of(executions.begin(), executions.end(), [](const auto& entry) { return entry.second == 1; }));
  REQUIRE_FALSE(releaseMutationAttempted);
}
