#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "shared/types.hpp"

namespace edgefleet::domain {

struct CohortDevice {
  std::string id;
  std::string stableKey;
  std::string hardwareModel;
  std::string architecture;
  shared::Json labels;
  std::string observedArtifactDigest = {};
  long long observedGeneration = 0;
  bool operator==(const CohortDevice& other) const;
};

struct CohortMember {
  CohortDevice device;
  std::string cohortHash;
  int ordinal = 0;
  int stage = 0;
  bool operator==(const CohortMember& other) const;
};

struct CohortPlan {
  std::vector<CohortMember> members;
  std::string digest;
};

class CohortPlanner {
 public:
  static shared::Result<CohortPlan> plan(std::string_view releaseId, std::string_view salt,
                                         std::vector<CohortDevice> devices,
                                         const std::vector<int>& stagePercentages);
};

enum class ReleaseState { draft, validating, blocked, ready, awaiting_approval, scheduled, running, paused, aborting, rolling_back, completed, aborted, rolled_back, failed, cancelled };
enum class ReleaseAction { validate, submit, approve, schedule, start, cancel, pause, resume, abort, rollback, gate_pass, gate_pause, gate_rollback, gate_advance, gate_override, security_block };
std::string toString(ReleaseState state);
std::string toString(ReleaseAction action);
std::optional<ReleaseState> releaseStateFromString(std::string_view value);

class ReleaseStateMachine {
 public:
  static std::optional<ReleaseState> transition(ReleaseState current, ReleaseAction action);
};

struct GateMetrics {
  int assigned = 0;
  int fresh = 0;
  int installFailures = 0;
  int healthFailures = 0;
  int rollbackFailures = 0;
  int converged = 0;
  int offline = 0;
  double crashFreePercent = 100.0;
};

struct GateThresholds {
  double freshCoverage = 0.80;
  double installFailurePause = 0.01;
  double installFailureRollback = 0.05;
  double crashFreePausePercent = 99.5;
  double crashFreeRollbackPercent = 97.0;
  double healthFailurePause = 0.02;
  double healthFailureRollback = 0.10;
  double convergence = 0.98;
  double offlineFraction = 0.20;
  double rollbackFailure = 0.05;
};

enum class GateDecision { insufficient_evidence, pass, pause, abort, rollback };
std::string toString(GateDecision decision);

class GateEvaluator {
 public:
  static GateDecision evaluate(const GateMetrics& metrics);
  static GateDecision evaluate(const GateMetrics& metrics, const GateThresholds& thresholds);
  static GateThresholds thresholdsFromPolicy(const shared::Json& policy);
  static std::vector<std::string> failedGates(const GateMetrics& metrics, const GateThresholds& thresholds);
};

struct SimulationResult {
  std::string simulatorVersion = "pcg64-v1";
  std::string traceDigest;
  std::string resultDigest;
  shared::Json trace = shared::Json::array();
  shared::Json metrics;
};

class Simulator {
 public:
  static std::vector<std::uint64_t> pcg64GoldenVector(std::uint64_t seed, std::size_t count);
  static shared::Result<SimulationResult> run(const shared::Json& input, std::uint64_t seed);
  static shared::Result<SimulationResult> run(const shared::Json& input, std::uint64_t seed, const std::function<bool()>& shouldCancel);
};

}  // namespace edgefleet::domain
