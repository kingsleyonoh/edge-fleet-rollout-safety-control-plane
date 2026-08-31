#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "domain/benchmark.hpp"
#include "domain/replay.hpp"
#include "domain/safety.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

TEST_CASE("the frozen benchmark corpus produces 108 deterministic cells", "[unit]") {
  const auto first = edgefleet::domain::BenchmarkRunner::run("v1", 20);
  const auto second = edgefleet::domain::BenchmarkRunner::run("v1", 20);
  REQUIRE(first.ok());
  REQUIRE(second.ok());
  REQUIRE(first.value->cells.size() == 108);
  REQUIRE(first.value->digest == second.value->digest);
  REQUIRE(first.value->cells.front().seed == 42);
  REQUIRE(first.value->cells.front().strategy == "control_plane");
  REQUIRE(first.value->cells.front().metrics.contains("exposure_seconds"));
  REQUIRE(first.value->cells.front().metrics.contains("false_rollback"));
  const auto universal = std::find_if(first.value->cells.begin(), first.value->cells.end(), [](const auto& cell) {
    return cell.scenario == "universal_install_failure_20" && cell.seed == 42 && cell.strategy == "control_plane";
  });
  REQUIRE(universal != first.value->cells.end());
  REQUIRE(universal->metrics.at("exposure_fraction").get<double>() <= 0.025);
  const auto rollback = std::find_if(first.value->cells.begin(), first.value->cells.end(), [](const auto& cell) {
    return cell.scenario == "rollback_offline_30" && cell.strategy == "control_plane";
  });
  REQUIRE(rollback != first.value->cells.end());
  REQUIRE(rollback->metrics.at("rollback_convergence_fraction").get<double>() >= 0.98);
  const auto sameTrace = std::find_if(first.value->cells.begin(), first.value->cells.end(), [](const auto& cell) {
    return cell.scenario == "healthy_10k_intermit_offline" && cell.seed == 42 && cell.strategy == "all_at_once";
  });
  REQUIRE(sameTrace != first.value->cells.end());
  REQUIRE(sameTrace->metrics.at("trace_digest") == first.value->cells.front().metrics.at("trace_digest"));
  for (const auto& scenario : {"control_plane_restart_stage2", "database_outage_command_retry", "duplicate_reorder_ack", "message_loss_20_reconnect"}) {
    const auto firstCell = std::find_if(first.value->cells.begin(), first.value->cells.end(), [&](const auto& cell) { return cell.scenario == scenario && cell.seed == 42 && cell.strategy == "control_plane"; });
    const auto secondCell = std::find_if(second.value->cells.begin(), second.value->cells.end(), [&](const auto& cell) { return cell.scenario == scenario && cell.seed == 42 && cell.strategy == "control_plane"; });
    REQUIRE(firstCell != first.value->cells.end());
    REQUIRE(secondCell != second.value->cells.end());
    REQUIRE(firstCell->digest == secondCell->digest);
    REQUIRE(firstCell->metrics.at("trace_digest") == secondCell->metrics.at("trace_digest"));
  }
}

TEST_CASE("the frozen benchmark manifest is backed by versioned scenario bytes", "[unit][benchmark]") {
  const auto root = std::filesystem::path(EDGEFLEET_SOURCE_DIR);
  const auto manifestPath = root / "fixtures" / "benchmarks" / "v1" / "manifest.json";
  std::ifstream manifestFile(manifestPath, std::ios::binary);
  REQUIRE(manifestFile.is_open());
  const auto manifest = nlohmann::json::parse(manifestFile);
  const std::vector<std::pair<std::string, std::string>> scenarios{
      {"healthy_10k_intermit_offline", "healthy-10k.json"}, {"universal_install_failure_20", "universal-install-failure-20.json"},
      {"universal_crash_regression_5m", "universal-crash-regression-5m.json"}, {"segment_hardware_defect", "segment-hardware-defect.json"},
      {"region_health_degradation", "region-health-degradation.json"}, {"duplicate_reorder_ack", "duplicate-reorder-ack.json"},
      {"message_loss_20_reconnect", "message-loss-20-reconnect.json"}, {"control_plane_restart_stage2", "control-plane-restart-stage2.json"},
      {"database_outage_command_retry", "database-outage-command-retry.json"}, {"iot_adapter_outage_optional_required", "iot-adapter-outage-optional-required.json"},
      {"rollback_offline_30", "rollback-offline-30.json"}, {"healthy_noisy_telemetry", "healthy-noisy-telemetry.json"}};
  REQUIRE(manifest.at("scenario_checksums").size() == scenarios.size());
  for (const auto& [name, filename] : scenarios) {
    const auto path = root / "fixtures" / "scenarios" / filename;
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(edgefleet::shared::DigestService::sha256File(path.string()) == manifest.at("scenario_checksums").at(name).get<std::string>());
    std::ifstream scenarioFile(path, std::ios::binary);
    const auto scenario = nlohmann::json::parse(scenarioFile);
    REQUIRE(scenario.at("scenario_name").get<std::string>() == name);
    REQUIRE(scenario.at("schema_version").get<std::string>() == "v1");
  }
}

TEST_CASE("replay uses one frozen simulation source and reports divergence", "[unit]") {
  const auto input = edgefleet::shared::Json{{"device_count", 25}, {"failure_probability", 0.02}, {"duration_seconds", 3600}};
  const auto simulation = edgefleet::domain::Simulator::run(input, 42);
  REQUIRE(simulation.ok());
  const auto reproduced = edgefleet::domain::ReplayEngine::simulation(input, 42, simulation.value->resultDigest);
  REQUIRE(reproduced.ok());
  REQUIRE(reproduced.value->status == "reproduced");
  const auto diverged = edgefleet::domain::ReplayEngine::simulation(input, 43, simulation.value->resultDigest);
  REQUIRE(diverged.ok());
  REQUIRE(diverged.value->status == "diverged");
  REQUIRE(diverged.value->divergence.at("kind") == "result_digest");
}

TEST_CASE("evidence replay reports the first changed event", "[unit]") {
  const auto expected = edgefleet::shared::Json::array({
      edgefleet::shared::Json{{"sequence_no", 1}, {"event_type", "release.validated"}},
      edgefleet::shared::Json{{"sequence_no", 2}, {"event_type", "release.started"}},
  });
  auto actual = expected;
  actual.at(1)["event_type"] = "release.paused";
  const auto expectedDigest = edgefleet::shared::DigestService::sha256Hex(edgefleet::shared::CanonicalJson::serialize(expected));
  const auto replay = edgefleet::domain::ReplayEngine::evidence(actual, expected, expectedDigest);
  REQUIRE(replay.ok());
  REQUIRE(replay.value->status == "diverged");
  REQUIRE(replay.value->divergence.at("first_divergence_index") == 1);
  REQUIRE(replay.value->divergence.at("expected_event").at("event_type") == "release.started");
  REQUIRE(replay.value->divergence.at("actual_event").at("event_type") == "release.paused");
}
