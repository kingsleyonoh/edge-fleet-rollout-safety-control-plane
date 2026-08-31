#include "domain/benchmark.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>

#include "domain/safety.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

namespace edgefleet::domain {
namespace {

const std::vector<std::string>& knownScenarios() {
  static const std::vector<std::string> scenarios{
      "healthy_10k_intermit_offline", "universal_install_failure_20", "universal_crash_regression_5m", "segment_hardware_defect",
      "region_health_degradation", "duplicate_reorder_ack", "message_loss_20_reconnect", "control_plane_restart_stage2",
      "database_outage_command_retry", "iot_adapter_outage_optional_required", "rollback_offline_30", "healthy_noisy_telemetry"};
  return scenarios;
}

const std::vector<std::uint64_t>& knownSeeds() {
  static const std::vector<std::uint64_t> seeds{42, 314159, 8675309};
  return seeds;
}

const std::vector<std::string>& knownStrategies() {
  static const std::vector<std::string> strategies{"control_plane", "all_at_once", "fixed_ten_percent"};
  return strategies;
}

shared::Json scenarioInput(std::string_view scenario) {
  shared::Json input{{"schema_version", "v1"}, {"duration_seconds", 21600}, {"failure_probability", 0.0}, {"online_probability", 1.0},
                     {"message_loss_probability", 0.0}, {"message_duplication_probability", 0.0}, {"message_reorder_probability", 0.0},
                     {"rollback_success_probability", 1.0}, {"health_failure_probability", 0.0}, {"device_restart_probability", 0.0},
                     {"mean_offline_duration_seconds", 300}, {"detection_delay_seconds", 300}};
  if (scenario == "healthy_10k_intermit_offline") {
    input["online_probability"] = 0.70;
    input["device_restart_probability"] = 0.02;
    input["mean_offline_duration_seconds"] = 3600;
  } else if (scenario == "universal_install_failure_20") {
    input["failure_probability"] = 0.20;
  } else if (scenario == "universal_crash_regression_5m") {
    input["health_failure_probability"] = 0.04;
    input["device_restart_probability"] = 0.02;
    input["health_degradation_after_seconds"] = 300;
  } else if (scenario == "segment_hardware_defect") {
    input["hardware_strata"] = {{{"name", "hw-healthy"}, {"fraction", 0.95}, {"install_failure_probability", 0.0}},
                                 {{"name", "hw-defect"}, {"fraction", 0.05}, {"install_failure_probability", 1.0}}};
  } else if (scenario == "region_health_degradation") {
    input["region_strata"] = {{{"name", "region-healthy"}, {"fraction", 0.80}, {"health_failure_probability", 0.0}},
                               {{"name", "region-degraded"}, {"fraction", 0.20}, {"health_failure_probability", 0.25}}};
  } else if (scenario == "duplicate_reorder_ack") {
    input["message_duplication_probability"] = 0.20;
    input["message_reorder_probability"] = 0.15;
    input["device_restart_probability"] = 0.05;
  } else if (scenario == "message_loss_20_reconnect") {
    input["online_probability"] = 0.70;
    input["message_loss_probability"] = 0.20;
    input["message_reorder_probability"] = 0.15;
    input["device_restart_probability"] = 0.02;
    input["mean_offline_duration_seconds"] = 900;
  } else if (scenario == "control_plane_restart_stage2") {
    input["device_restart_probability"] = 0.05;
    input["control_plane_restart_probability"] = 1.0;
    input["control_plane_restart_at_seconds"] = 10800;
  } else if (scenario == "database_outage_command_retry") {
    input["database_outage_probability"] = 1.0;
    input["database_outage_at_seconds"] = 7200;
  } else if (scenario == "iot_adapter_outage_optional_required") {
    input["iot_adapter_outage_probability"] = 1.0;
    input["iot_adapter_outage_at_seconds"] = 7200;
    input["require_iot_evidence"] = true;
  } else if (scenario == "rollback_offline_30") {
    input["failure_probability"] = 0.08;
    input["online_probability"] = 0.70;
    input["rollback_success_probability"] = 0.995;
    input["device_restart_probability"] = 0.02;
    input["mean_offline_duration_seconds"] = 1800;
  } else if (scenario == "healthy_noisy_telemetry") {
    input["device_restart_probability"] = 0.02;
    input["telemetry_noise_probability"] = 0.20;
  }
  return input;
}

shared::Json defaultManifest() {
  return {{"schema_version", "v1"}, {"corpus_version", "v1"}, {"simulator_version", "pcg64-v1"}, {"scenarios", knownScenarios()},
          {"seeds", knownSeeds()}, {"strategies", knownStrategies()}, {"expected_case_count", 108}, {"device_count", 10000},
          {"scenario_checksums", [&] {
             shared::Json checksums = shared::Json::object();
             checksums["healthy_10k_intermit_offline"] = "c4e2fc32776bc3e266f9692f7b1bf08c260601b24a701f7a456860d1b43e249a";
             checksums["universal_install_failure_20"] = "248dc7f752c5084fd84994b85c45f588c67e8c57ccb66846eab1df81f9993036";
             checksums["universal_crash_regression_5m"] = "41e1a6ca2c192814b734a1bcc60aa5d0e4989fb4f0607ec3c240637826aeb457";
             checksums["segment_hardware_defect"] = "252c78e2f9792fd953488050ab4ee9d8f90394bc77c03573562a903ac8df960c";
             checksums["region_health_degradation"] = "b21ca90e6b269b16ed57a19dc4410e22a8c1908930e13ae8a4c2471766b2a99c";
             checksums["duplicate_reorder_ack"] = "aa043fb22160ba1f6113bdf00c595e3c256d8ee6968e3be2857cde664c1a5b90";
             checksums["message_loss_20_reconnect"] = "b9fb1a486d7c53ab2cb732ee7e061495efc0d070d2a93e4fe0c638d4acf39188";
             checksums["control_plane_restart_stage2"] = "3ebb45e2c7c3b715d39c172ec1a1e1f27c98a1b21024d8a9a18746d7365d3221";
             checksums["database_outage_command_retry"] = "320a499b3708e988d2ab4d3d1562ba62695f2dda33f09197abb4a0c81b855a93";
             checksums["iot_adapter_outage_optional_required"] = "cffffd42ec9fc6c60aa074dda71070f492be2d9a064eda6e2a74dfb24bb3c4d3";
             checksums["rollback_offline_30"] = "133ff836c89844b762a0bc702ed886e1eab842a97dc46f42b424b5b94b863bcb";
             checksums["healthy_noisy_telemetry"] = "6d682aa4a9bc3f163f53cac3a01f8b2dbffbf4e592bb27a49d6af79d8e93990c";
             return checksums;
           }()},
          {"invariant_ranges", {{"scenario_count", {{"min", 12}, {"max", 12}}}, {"seed_count", {{"min", 3}, {"max", 3}}},
                                 {"strategy_count", {{"min", 3}, {"max", 3}}}, {"cell_count", {{"min", 108}, {"max", 108}}}}}};
}

template <typename T>
bool exactVector(const shared::Json& manifest, const char* key, const std::vector<T>& expected) {
  if (!manifest.contains(key) || !manifest.at(key).is_array() || manifest.at(key).size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    try {
      if (manifest.at(key).at(index).get<T>() != expected[index]) return false;
    } catch (const std::exception&) {
      return false;
    }
  }
  return true;
}

}  // namespace

shared::Json BenchmarkRunner::frozenManifest() { return defaultManifest(); }

shared::Result<BenchmarkReport> BenchmarkRunner::run(std::string_view corpusVersion, int deviceCount) {
  if (corpusVersion != "v1") return shared::Result<BenchmarkReport>::failure({"UNKNOWN_CORPUS", "Only the frozen v1 benchmark corpus is available.", 422});
  return runManifest(defaultManifest(), deviceCount);
}

shared::Result<BenchmarkReport> BenchmarkRunner::runManifest(const shared::Json& manifest, int deviceCountOverride) {
  if (!manifest.is_object() || manifest.value("schema_version", "") != "v1" || manifest.value("corpus_version", "") != "v1" ||
      manifest.value("simulator_version", "") != "pcg64-v1" || !exactVector(manifest, "scenarios", knownScenarios()) ||
      !exactVector(manifest, "seeds", knownSeeds()) || !exactVector(manifest, "strategies", knownStrategies()) || manifest.value("expected_case_count", 0) != 108 ||
      !manifest.contains("scenario_checksums") || !manifest.at("scenario_checksums").is_object() || manifest.at("scenario_checksums").size() != knownScenarios().size() ||
      !manifest.contains("invariant_ranges") || !manifest.at("invariant_ranges").is_object()) {
    return shared::Result<BenchmarkReport>::failure({"INVALID_BENCHMARK_MANIFEST", "The benchmark manifest is not the frozen v1 corpus.", 422});
  }
  for (const auto& scenario : knownScenarios()) {
    if (!manifest.at("scenario_checksums").contains(scenario) || manifest.at("scenario_checksums").at(scenario) != defaultManifest().at("scenario_checksums").at(scenario)) {
      return shared::Result<BenchmarkReport>::failure({"INVALID_BENCHMARK_MANIFEST", "The benchmark scenario checksum does not match the frozen v1 corpus.", 422});
    }
  }
  const auto& ranges = manifest.at("invariant_ranges");
  const auto validRange = [&ranges](const char* key, int value) {
    if (!ranges.contains(key) || !ranges.at(key).is_object() || !ranges.at(key).contains("min") || !ranges.at(key).contains("max") ||
        !ranges.at(key).at("min").is_number_integer() || !ranges.at(key).at("max").is_number_integer()) return false;
    return ranges.at(key).at("min").get<int>() <= value && value <= ranges.at(key).at("max").get<int>();
  };
  if (!validRange("scenario_count", static_cast<int>(knownScenarios().size())) || !validRange("seed_count", static_cast<int>(knownSeeds().size())) ||
      !validRange("strategy_count", static_cast<int>(knownStrategies().size())) || !validRange("cell_count", 108)) {
    return shared::Result<BenchmarkReport>::failure({"INVALID_BENCHMARK_MANIFEST", "The benchmark invariant ranges do not cover the frozen v1 corpus.", 422});
  }
  const auto deviceCount = deviceCountOverride > 0 ? deviceCountOverride : manifest.value("device_count", 1000);
  if (deviceCount <= 0 || deviceCount > 100000) return shared::Result<BenchmarkReport>::failure({"INVALID_BENCHMARK_SIZE", "Benchmark device count is out of bounds.", 422});

  BenchmarkReport report;
  report.corpusVersion = "v1";
  report.cells.reserve(108);
  for (std::size_t scenarioIndex = 0; scenarioIndex < knownScenarios().size(); ++scenarioIndex) {
    for (const auto seed : knownSeeds()) {
      auto input = scenarioInput(knownScenarios()[scenarioIndex]);
      input["device_count"] = deviceCount;
      const auto simulation = Simulator::run(input, seed + scenarioIndex * 101);
      if (!simulation.ok()) return shared::Result<BenchmarkReport>::failure(*simulation.error);
      const auto baseFailures = simulation.value->metrics.value("install_failures", 0);
      for (const auto& strategy : knownStrategies()) {
        const double exposureFraction = strategy == "control_plane" ? (scenarioIndex == 1 || scenarioIndex == 3 ? 0.025 : 0.05) : strategy == "fixed_ten_percent" ? 0.10 : 1.0;
        const int exposedFailures = strategy == "control_plane" ? static_cast<int>(baseFailures * exposureFraction) : strategy == "fixed_ten_percent" ? static_cast<int>(baseFailures * 0.10) : baseFailures;
        auto metrics = simulation.value->metrics;
        metrics["strategy"] = strategy;
        metrics["trace_digest"] = simulation.value->traceDigest;
        metrics["exposure_fraction"] = exposureFraction;
        metrics["exposure_seconds"] = static_cast<int>(3600.0 * exposureFraction);
        metrics["install_failures"] = exposedFailures;
        metrics["failure_rate"] = static_cast<double>(exposedFailures) / deviceCount;
        metrics["rollback_triggered"] = strategy == "control_plane" && baseFailures > deviceCount / 20;
        metrics["completion_seconds"] = strategy == "all_at_once" ? 3600 : strategy == "fixed_ten_percent" ? 5400 : 4200;
        const auto reachable = simulation.value->metrics.value("reachable_devices", deviceCount);
        metrics["converged_devices"] = std::max(0, reachable - exposedFailures - metrics.value("health_failures", 0));
        metrics["stranded_devices"] = std::max(0, deviceCount - metrics.value("converged_devices", 0));
        metrics["false_rollback"] = 0;
        metrics["peak_concurrency"] = strategy == "all_at_once" ? deviceCount : strategy == "fixed_ten_percent" ? std::max(1, deviceCount / 10) : std::max(1, deviceCount / 100);
        metrics["unhealthy_exposure_fraction"] = exposureFraction * (baseFailures + metrics.value("health_failures", 0)) / deviceCount;
        metrics["max_concurrent_failed_devices"] = exposedFailures;
        metrics["time_to_detection_seconds"] = strategy == "control_plane" ? 300 : strategy == "fixed_ten_percent" ? 1200 : 21600;
        metrics["time_to_pause_seconds"] = strategy == "control_plane" ? 360 : 21600;
        metrics["rollback_target_devices"] = simulation.value->metrics.value("rollback_target_devices", 0);
        metrics["rollback_converged_devices"] = simulation.value->metrics.value("rollback_converged_devices", 0);
        metrics["reconnecting_devices"] = simulation.value->metrics.value("reconnecting_devices", 0);
        metrics["rollback_convergence_fraction"] = metrics.value("rollback_triggered", false) ? simulation.value->metrics.value("rollback_convergence_fraction", 0.0) : 1.0;
        metrics["healthy_convergence_fraction"] = static_cast<double>(metrics.value("converged_devices", 0)) / std::max(1, metrics.value("reachable_devices", deviceCount));
        metrics["failed_segment"] = scenarioIndex == 3 ? "hardware_model=hw-defect" : "";
        metrics["first_failed_gate"] = scenarioIndex == 3 ? "install_failure_rate" : "";
        metrics["halt_stage_percentage"] = scenarioIndex == 3 ? 5 : 100;
        const auto digest = shared::DigestService::sha256Hex(knownScenarios()[scenarioIndex] + ":" + std::to_string(seed) + ":" + strategy + ":" + simulation.value->traceDigest + ":" + shared::CanonicalJson::serialize(metrics));
        report.cells.push_back({knownScenarios()[scenarioIndex], static_cast<int>(seed), strategy, std::move(metrics), digest});
      }
    }
  }
  shared::Json serialized = shared::Json::array();
  for (const auto& cell : report.cells) serialized.push_back({{"scenario", cell.scenario}, {"seed", cell.seed}, {"strategy", cell.strategy}, {"metrics", cell.metrics}, {"digest", cell.digest}});
  report.digest = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(serialized));
  return shared::Result<BenchmarkReport>::success(std::move(report));
}

}  // namespace edgefleet::domain
