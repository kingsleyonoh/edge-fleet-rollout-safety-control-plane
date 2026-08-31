#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "domain/replay.hpp"
#include "domain/safety.hpp"
#include "infrastructure/sqlite_storage.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/types.hpp"

TEST_CASE("reference simulation and replay duration targets pass", "[component][performance]") {
  std::ifstream scenarioFile(std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "fixtures" / "scenarios" / "healthy-10k.json");
  REQUIRE(scenarioFile.is_open());
  auto scenario = nlohmann::json::parse(scenarioFile);
  scenario["duration_seconds"] = 30 * 24 * 60 * 60;
  const auto simulationStarted = std::chrono::steady_clock::now();
  const auto simulation = edgefleet::domain::Simulator::run(scenario, 42);
  const auto simulationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - simulationStarted).count();
  REQUIRE(simulation.ok());
  INFO("10,000-device 30-day simulation seconds=" << simulationSeconds);
  REQUIRE(simulationSeconds < 60.0);

  edgefleet::shared::Json events = edgefleet::shared::Json::array();
  for (int index = 0; index < 100000; ++index) events.push_back({{"sequence_no", index + 1}, {"event_type", "device.observed"}, {"device_id", "device-" + std::to_string(index % 1000)}, {"value", index % 7}});
  const auto expectedDigest = edgefleet::shared::DigestService::sha256Hex(edgefleet::shared::CanonicalJson::serialize(events));
  const auto replayStarted = std::chrono::steady_clock::now();
  const auto replay = edgefleet::domain::ReplayEngine::evidence(events, expectedDigest);
  const auto replaySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - replayStarted).count();
  REQUIRE(replay.ok());
  REQUIRE(replay.value->status == "reproduced");
  INFO("100,000-event replay seconds=" << replaySeconds);
  REQUIRE(replaySeconds < 30.0);
  std::cout << "reference_performance simulation_10k_30d_seconds=" << simulationSeconds << " replay_100k_seconds=" << replaySeconds << "\n";
}

TEST_CASE("reference HTTP workload fixture seeds five hundred device principals", "[component][performance][fixture]") {
  const auto* configuredRoot = std::getenv("EDGEFLEET_REFERENCE_FIXTURE_ROOT");
  if (configuredRoot == nullptr || *configuredRoot == '\0') SKIP("EDGEFLEET_REFERENCE_FIXTURE_ROOT is not configured");
  const auto root = std::filesystem::path(configuredRoot);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  edgefleet::infrastructure::SqliteStorage storage((root / "edgefleet.db").string());
  REQUIRE(storage.open());
  REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
  const auto apiSecret = std::string("reference-") + edgefleet::shared::Uuid::generate().str();
  const auto deviceSecret = std::string("device-") + edgefleet::shared::Uuid::generate().str();
  const auto tenant = storage.createTenant("Reference", "Reference Ltd", "Reference", "UTC", "edge_live_reference", edgefleet::shared::DigestService::argon2idHash(apiSecret));
  REQUIRE(tenant.has_value());
  const auto fleet = storage.createFleet(tenant->at("id").get<std::string>(), "reference-fleet", "Reference Fleet", "production");
  REQUIRE(fleet.has_value());
  std::ofstream ids(root / "device-ids.txt");
  for (int index = 0; index < 500; ++index) {
    const auto device = storage.createDevice(tenant->at("id").get<std::string>(), fleet->at("id").get<std::string>(),
                                             {{"stable_key", "reference-device-" + std::to_string(index)}, {"hardware_model", "m1"}, {"architecture", "x86_64"}},
                                             edgefleet::shared::DigestService::sha256Hex(deviceSecret));
    REQUIRE(device.has_value());
    ids << device->at("id").get<std::string>() << (index == 499 ? '\n' : ',');
  }
  ids.close();
  REQUIRE(static_cast<bool>(ids));
  std::ofstream operatorKey(root / "operator-api-key.txt");
  operatorKey << "edge_live_reference." << apiSecret << '\n';
  std::ofstream deviceKey(root / "device-secret.txt");
  deviceKey << deviceSecret << '\n';
  REQUIRE(static_cast<bool>(operatorKey));
  REQUIRE(static_cast<bool>(deviceKey));
  std::cout << "reference_fixture_root=" << root.string() << " credential_files=operator-api-key.txt,device-secret.txt device_count=500\n";
}
