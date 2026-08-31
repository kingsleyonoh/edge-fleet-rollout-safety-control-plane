#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "domain/safety.hpp"

TEST_CASE("cohort planning is independent of input order and preserves strata", "[unit]") {
  std::vector<edgefleet::domain::CohortDevice> devices{
      {"d-2", "beta", "m2", "arm64", {}},
      {"d-1", "alpha", "m1", "x86_64", {}},
      {"d-3", "gamma", "m1", "x86_64", {}},
      {"d-4", "delta", "m2", "arm64", {}},
  };
  const auto first = edgefleet::domain::CohortPlanner::plan("release-1", "salt", devices, {25, 50, 100});
  std::reverse(devices.begin(), devices.end());
  const auto second = edgefleet::domain::CohortPlanner::plan("release-1", "salt", devices, {25, 50, 100});

  REQUIRE(first.ok());
  REQUIRE(second.ok());
  REQUIRE(first.value->digest == second.value->digest);
  REQUIRE(first.value->members == second.value->members);
  REQUIRE(first.value->members.size() == 4);
  REQUIRE(first.value->members.front().cohortHash.size() == 64);
  nlohmann::json serialized = nlohmann::json::array();
  for (const auto& member : first.value->members) serialized.push_back({{"id", member.device.id}, {"stable_key", member.device.stableKey}, {"hardware_model", member.device.hardwareModel}, {"architecture", member.device.architecture}, {"cohort_hash", member.cohortHash}, {"ordinal", member.ordinal}, {"stage", member.stage}, {"labels", member.device.labels}, {"observed_artifact_digest", member.device.observedArtifactDigest}, {"observed_generation", member.device.observedGeneration}});
  const auto expectedDigest = edgefleet::shared::DigestService::sha256Hex(edgefleet::shared::CanonicalJson::serialize(serialized));
  REQUIRE(first.value->digest == expectedDigest);
}

TEST_CASE("ten-thousand-device cohort remains byte-identical across one hundred input shuffles", "[unit][safety][performance]") {
  std::vector<edgefleet::domain::CohortDevice> devices;
  devices.reserve(10000);
  for (int index = 0; index < 10000; ++index) {
    devices.push_back({"device-" + std::to_string(index), "stable-" + std::to_string(index), index % 5 == 0 ? "hw-a" : "hw-b",
                       index % 3 == 0 ? "arm64" : "x86_64", {{"region", index % 4 == 0 ? "eu" : "us"}}});
  }

  const auto planningStarted = std::chrono::steady_clock::now();
  const auto baseline = edgefleet::domain::CohortPlanner::plan("release-10k", "fixed-salt", devices, {1, 5, 20, 50, 100});
  const auto planningSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - planningStarted).count();
  REQUIRE(baseline.ok());
  INFO("10,000-device cohort planning seconds=" << planningSeconds);
  REQUIRE(planningSeconds < 2.0);
  REQUIRE(baseline.value->members.size() == devices.size());
  const auto baselineBytes = edgefleet::shared::CanonicalJson::serialize(
      nlohmann::json{{"digest", baseline.value->digest}, {"members", baseline.value->members.size()}, {"last_stage", baseline.value->members.back().stage}});

  for (int shuffle = 0; shuffle < 100; ++shuffle) {
    auto candidate = devices;
    std::mt19937 generator(static_cast<std::mt19937::result_type>(shuffle + 1));
    std::shuffle(candidate.begin(), candidate.end(), generator);
    const auto plan = edgefleet::domain::CohortPlanner::plan("release-10k", "fixed-salt", std::move(candidate), {1, 5, 20, 50, 100});
    REQUIRE(plan.ok());
    REQUIRE(plan.value->digest == baseline.value->digest);
    REQUIRE(plan.value->members == baseline.value->members);
    REQUIRE(edgefleet::shared::CanonicalJson::serialize(
                nlohmann::json{{"digest", plan.value->digest}, {"members", plan.value->members.size()}, {"last_stage", plan.value->members.back().stage}}) == baselineBytes);
  }
}

TEST_CASE("release transitions reject illegal and terminal actions", "[unit]") {
  using edgefleet::domain::ReleaseAction;
  using edgefleet::domain::ReleaseState;
  REQUIRE(edgefleet::domain::ReleaseStateMachine::transition(ReleaseState::draft, ReleaseAction::validate).value() == ReleaseState::validating);
  REQUIRE_FALSE(edgefleet::domain::ReleaseStateMachine::transition(ReleaseState::completed, ReleaseAction::pause).has_value());
  REQUIRE_FALSE(edgefleet::domain::ReleaseStateMachine::transition(ReleaseState::running, ReleaseAction::cancel).has_value());
  REQUIRE(edgefleet::domain::ReleaseStateMachine::transition(ReleaseState::running, ReleaseAction::gate_pass).value() == ReleaseState::completed);
  REQUIRE(edgefleet::domain::ReleaseStateMachine::transition(ReleaseState::running, ReleaseAction::gate_advance).value() == ReleaseState::running);
  REQUIRE(edgefleet::domain::ReleaseStateMachine::transition(ReleaseState::paused, ReleaseAction::gate_pass).value() == ReleaseState::running);
  REQUIRE_FALSE(edgefleet::domain::ReleaseStateMachine::transition(ReleaseState::cancelled, ReleaseAction::validate).has_value());
}

TEST_CASE("gate evaluation uses precedence and exact count thresholds", "[unit]") {
  edgefleet::domain::GateMetrics metrics;
  metrics.assigned = 100;
  metrics.fresh = 100;
  metrics.installFailures = 6;
  metrics.crashFreePercent = 96.0;
  REQUIRE(edgefleet::domain::GateEvaluator::evaluate(metrics) == edgefleet::domain::GateDecision::rollback);

  metrics.installFailures = 0;
  metrics.crashFreePercent = 99.9;
  metrics.healthFailures = 3;
  REQUIRE(edgefleet::domain::GateEvaluator::evaluate(metrics) == edgefleet::domain::GateDecision::pause);
}

TEST_CASE("frozen policy gate thresholds are applied with floor semantics", "[unit][safety]") {
  const auto thresholds = edgefleet::domain::GateEvaluator::thresholdsFromPolicy({
      {"health_gates", {{"install_failure_rate", 0.03}, {"convergence_rate", 0.90}, {"fresh_device_coverage", 0.50}}}});
  edgefleet::domain::GateMetrics metrics;
  metrics.assigned = 100;
  metrics.fresh = 100;
  metrics.converged = 100;
  metrics.installFailures = 3;
  REQUIRE(edgefleet::domain::GateEvaluator::evaluate(metrics, thresholds) == edgefleet::domain::GateDecision::pass);
  metrics.installFailures = 4;
  REQUIRE(edgefleet::domain::GateEvaluator::evaluate(metrics, thresholds) == edgefleet::domain::GateDecision::pause);
  const auto failed = edgefleet::domain::GateEvaluator::failedGates(metrics, thresholds);
  REQUIRE(std::find(failed.begin(), failed.end(), "install_failure_rate") != failed.end());
}

TEST_CASE("simulator result is reproducible for a frozen input and seed", "[unit]") {
  const auto input = nlohmann::json{{"device_count", 50}, {"failure_probability", 0.02}, {"duration_seconds", 100}};
  const auto first = edgefleet::domain::Simulator::run(input, 42);
  const auto second = edgefleet::domain::Simulator::run(input, 42);
  REQUIRE(first.ok());
  REQUIRE(second.ok());
  REQUIRE(first.value->resultDigest == second.value->resultDigest);
  REQUIRE(first.value->traceDigest == second.value->traceDigest);
}

TEST_CASE("simulator frozen scenario has a compiler-independent trace and result digest", "[unit][simulation][cross-platform]") {
  std::ifstream input(std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "fixtures" / "scenarios" / "control-plane-restart-stage2.json");
  REQUIRE(input.is_open());
  const auto scenario = nlohmann::json::parse(input);
  const auto result = edgefleet::domain::Simulator::run(scenario, 42);
  REQUIRE(result.ok());
  REQUIRE(result.value->resultDigest == "8d07b59e44449998bb348148d2fc0e61114604419e860c921b3fc10971fe2a09");
  REQUIRE(result.value->traceDigest == "c1e33728139a85b4df17f035aaee7ad1050a776ac0a24383961b582699848098");
}

TEST_CASE("pcg64-v1 matches its compiler-independent golden vector", "[unit][simulation]") {
  const auto vector = edgefleet::domain::Simulator::pcg64GoldenVector(42, 8);
  REQUIRE(vector == std::vector<std::uint64_t>{14250125454581ULL, 31441755614196764ULL, 1025967704830ULL, 7024060133218613625ULL,
                                                12670271646340602094ULL, 354825915945654ULL, 1927566241744ULL, 7871074256664898645ULL});
}

TEST_CASE("simulator freezes outage, reorder, reconnect, and rollback evidence in its trace", "[unit][simulation]") {
  const auto input = nlohmann::json{{"schema_version", "v1"},
                                    {"device_count", 8},
                                    {"duration_seconds", 600},
                                    {"failure_probability", 1.0},
                                    {"online_probability", 0.0},
                                    {"message_reorder_probability", 1.0},
                                    {"control_plane_restart_probability", 1.0},
                                    {"database_outage_probability", 1.0},
                                    {"iot_adapter_outage_probability", 1.0},
                                    {"mean_offline_duration_seconds", 120},
                                    {"rollback_success_probability", 1.0}};
  const auto result = edgefleet::domain::Simulator::run(input, 42);
  REQUIRE(result.ok());
  REQUIRE(result.value->metrics.at("database_outages") == 1);
  REQUIRE(result.value->metrics.at("control_plane_restarts") == 1);
  REQUIRE(result.value->metrics.at("iot_adapter_outages") == 1);
  REQUIRE(result.value->metrics.at("reordered_messages") == 8);
  REQUIRE(result.value->metrics.at("reconnecting_devices") == 8);
  REQUIRE(result.value->metrics.at("rollback_convergence_fraction") == 1.0);
  REQUIRE(result.value->metrics.at("rollback_failures") == 0);
  REQUIRE(result.value->trace.size() > 8);
}

TEST_CASE("simulator validates bounded distributions, models flapping, and honors cancellation", "[unit][simulation]") {
  const auto invalidDistribution = edgefleet::domain::Simulator::run({{"device_count", 8}, {"duration_seconds", 600}, {"install_duration_distribution", "unbounded"}}, 42);
  REQUIRE_FALSE(invalidDistribution.ok());
  const auto tooManyEvents = edgefleet::domain::Simulator::run({{"device_count", 8}, {"duration_seconds", 600}, {"max_events", 1}}, 42);
  REQUIRE_FALSE(tooManyEvents.ok());
  REQUIRE(tooManyEvents.error->code == "SCENARIO_TOO_MANY_EVENTS");

  const auto flapping = edgefleet::domain::Simulator::run({{"device_count", 32}, {"duration_seconds", 600}, {"connectivity_mode", "flapping"}, {"flap_probability", 1.0}}, 42);
  REQUIRE(flapping.ok());
  REQUIRE(flapping.value->metrics.at("flapping_devices") == 32);
  REQUIRE(std::any_of(flapping.value->trace.begin(), flapping.value->trace.end(), [](const auto& event) { return event.value("type", "") == "connectivity_lost"; }));

  int checks = 0;
  const auto cancelled = edgefleet::domain::Simulator::run({{"device_count", 10000}, {"duration_seconds", 600}}, 42, [&checks] { return ++checks > 1; });
  REQUIRE_FALSE(cancelled.ok());
  REQUIRE(cancelled.error->code == "SIMULATION_CANCELLED");
}

TEST_CASE("simulator applies hardware and region strata to device outcomes", "[unit][simulation]") {
  const auto result = edgefleet::domain::Simulator::run({
      {"schema_version", "v1"},
      {"device_count", 200},
      {"duration_seconds", 600},
      {"hardware_strata", {{{"name", "healthy"}, {"fraction", 0.75}, {"install_failure_probability", 0.0}},
                            {{"name", "defect"}, {"fraction", 0.25}, {"install_failure_probability", 1.0}}}},
      {"region_strata", {{{"name", "stable"}, {"fraction", 0.80}, {"health_failure_probability", 0.0}},
                          {{"name", "degraded"}, {"fraction", 0.20}, {"health_failure_probability", 1.0}}}},
  }, 42);
  REQUIRE(result.ok());
  REQUIRE(result.value->metrics.at("hardware_counts").at("healthy").get<int>() + result.value->metrics.at("hardware_counts").at("defect").get<int>() == 200);
  REQUIRE(result.value->metrics.at("hardware_failures").at("defect") > 0);
  REQUIRE(result.value->metrics.at("region_health_failures").at("degraded") > 0);
  REQUIRE(std::any_of(result.value->trace.begin(), result.value->trace.end(), [](const auto& event) {
    return event.value("hardware_model", "") == "defect" && event.value("region", "") == "degraded";
  }));
}
