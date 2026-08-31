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

TEST_CASE("notification_hub_v1 factory preserves idempotent fixture delivery behavior", "[contract][integration]") {
  const auto adapter = edgefleet::infrastructure::createAdapter("notification_hub_v1");
  REQUIRE(adapter);
  REQUIRE(adapter->type() == edgefleet::infrastructure::AdapterType::notification_hub_v1);
  const auto retry = adapter->fixtureDelivery({{"fixture_mode", true}, {"fixture_status", 503}}, "hub-contract", 0);
  REQUIRE(retry.disposition == edgefleet::infrastructure::DeliveryDisposition::retryable_failure);
  REQUIRE(retry.errorCode == "RETRYABLE_ADAPTER_FAILURE");
}

TEST_CASE("notification_hub_v1 publishes one event per idempotency key within the contract budget", "[contract][integration][performance]") {
  std::mutex mutex;
  std::map<std::string, int> deliveries;
  edgefleet::web::HttpServer server("127.0.0.1", 0, [&](const edgefleet::web::HttpRequest& request) {
    const auto idempotency = request.headers.find("idempotency-key");
    const auto apiKey = request.headers.find("x-api-key");
    if (request.method != "POST" || request.target != "/api/events" || idempotency == request.headers.end() || apiKey == request.headers.end() || apiKey->second != "contract-secret") {
      return edgefleet::web::HttpResponse{400, "application/json", R"({"error":"invalid notification request"})", {}};
    }
    {
      std::lock_guard lock(mutex);
      ++deliveries[idempotency->second];
    }
    return edgefleet::web::HttpResponse{202, "application/json", R"({"published":true})", {}};
  });
  std::thread serverThread([&server] { server.start(); });
  for (int attempt = 0; attempt < 100 && server.boundPort() == 0; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(server.boundPort() != 0);

  auto adapter = edgefleet::infrastructure::createAdapter("notification_hub_v1");
  REQUIRE(adapter);
  edgefleet::shared::HttpClientPool clientPool(8);
  std::vector<double> latencies;
  latencies.reserve(100);
  for (int index = 0; index < 100; ++index) {
    const auto started = std::chrono::steady_clock::now();
    const auto result = adapter->liveDelivery("http://127.0.0.1:" + std::to_string(server.boundPort()), "contract-secret", {{"event_type", "release.paused"}},
                                              "notification-contract-" + std::to_string(index), {}, clientPool);
    latencies.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    REQUIRE(result.disposition == edgefleet::infrastructure::DeliveryDisposition::published);
    REQUIRE(result.statusCode == 202);
  }
  server.stop();
  serverThread.join();
  std::sort(latencies.begin(), latencies.end());
  const auto p95 = latencies.at(94);
  REQUIRE(p95 < 5000.0);
  std::lock_guard lock(mutex);
  REQUIRE(deliveries.size() == 100);
  REQUIRE(std::all_of(deliveries.begin(), deliveries.end(), [](const auto& entry) { return entry.second == 1; }));
}
