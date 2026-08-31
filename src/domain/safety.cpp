#include "domain/safety.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <map>
#include <numeric>
#include <queue>

#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

namespace edgefleet::domain {
namespace {

std::string deviceKey(const CohortDevice& device) { return device.hardwareModel + "\x1f" + device.architecture; }

void appendJsonString(std::string& output, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (byte) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (byte < 0x20) {
          output += "\\u00";
          output.push_back(hex[(byte >> 4U) & 0x0fU]);
          output.push_back(hex[byte & 0x0fU]);
        } else output.push_back(character);
        break;
    }
  }
  output.push_back('"');
}

void appendCanonicalCohortMember(std::string& output, const CohortMember& member) {
  output += "{\"architecture\":";
  appendJsonString(output, member.device.architecture);
  output += ",\"cohort_hash\":";
  appendJsonString(output, member.cohortHash);
  output += ",\"hardware_model\":";
  appendJsonString(output, member.device.hardwareModel);
  output += ",\"id\":";
  appendJsonString(output, member.device.id);
  output += ",\"labels\":";
  output += member.device.labels.dump(-1, ' ', false, shared::Json::error_handler_t::strict);
  output += ",\"observed_artifact_digest\":";
  appendJsonString(output, member.device.observedArtifactDigest);
  output += ",\"observed_generation\":";
  output += std::to_string(member.device.observedGeneration);
  output += ",\"ordinal\":";
  output += std::to_string(member.ordinal);
  output += ",\"stable_key\":";
  appendJsonString(output, member.device.stableKey);
  output += ",\"stage\":";
  output += std::to_string(member.stage);
  output.push_back('}');
}

class Pcg64V1 {
 public:
  explicit Pcg64V1(std::uint64_t seed) : state_(0), increment_(0xda3e39cb94b95bdbULL) {
    next();
    state_ += seed;
    next();
  }

  std::uint64_t next() {
    const auto oldState = state_;
    state_ = oldState * 6364136223846793005ULL + increment_;
    const auto xorshifted = ((oldState >> 18U) ^ oldState) >> 27U;
    const auto rotation = oldState >> 59U;
    return (xorshifted >> rotation) | (xorshifted << ((static_cast<std::uint64_t>(0) - rotation) & 31U));
  }

  double unit() { return static_cast<double>(next() % 1000000ULL) / 1000000.0; }

 private:
  std::uint64_t state_;
  std::uint64_t increment_;
};

struct SimulatedEvent {
  std::int64_t at = 0;
  int priority = 0;
  std::uint64_t sequence = 0;
  shared::Json payload;
};

struct EventOrder {
  bool operator()(const SimulatedEvent& left, const SimulatedEvent& right) const {
    if (left.at != right.at) return left.at > right.at;
    if (left.priority != right.priority) return left.priority > right.priority;
    return left.sequence > right.sequence;
  }
};

struct SimulationStratum {
  std::string name;
  double fraction = 1.0;
  std::optional<double> installFailureProbability;
  std::optional<double> healthFailureProbability;
};

shared::Result<std::vector<SimulationStratum>> parseStrata(const shared::Json& input, const char* key, const char* defaultName) {
  if (!input.contains(key)) return shared::Result<std::vector<SimulationStratum>>::success({SimulationStratum{defaultName, 1.0, std::nullopt, std::nullopt}});
  const auto& raw = input.at(key);
  if (!raw.is_array() || raw.empty()) return shared::Result<std::vector<SimulationStratum>>::failure({"INVALID_SCENARIO", std::string(key) + " must be a non-empty array.", 422});
  std::vector<SimulationStratum> result;
  result.reserve(raw.size());
  double total = 0.0;
  for (const auto& item : raw) {
    if (!item.is_object() || !item.contains("name") || !item.at("name").is_string() || item.at("name").get<std::string>().empty() || item.at("name").get<std::string>().size() > 128 ||
        !item.contains("fraction") || !item.at("fraction").is_number()) {
      return shared::Result<std::vector<SimulationStratum>>::failure({"INVALID_SCENARIO", std::string(key) + " entries require a name and fraction.", 422});
    }
    const auto fraction = item.at("fraction").get<double>();
    if (!std::isfinite(fraction) || fraction <= 0.0 || fraction > 1.0) return shared::Result<std::vector<SimulationStratum>>::failure({"INVALID_SCENARIO", std::string(key) + " fractions must be in (0,1].", 422});
    SimulationStratum stratum{item.at("name").get<std::string>(), fraction, std::nullopt, std::nullopt};
    for (const auto& field : {"install_failure_probability", "health_failure_probability"}) {
      if (!item.contains(field)) continue;
      if (!item.at(field).is_number()) return shared::Result<std::vector<SimulationStratum>>::failure({"INVALID_SCENARIO", std::string(key) + " probabilities must be numeric.", 422});
      const auto probability = item.at(field).get<double>();
      if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) return shared::Result<std::vector<SimulationStratum>>::failure({"INVALID_SCENARIO", std::string(key) + " probabilities must be in [0,1].", 422});
      if (std::string_view(field) == "install_failure_probability") stratum.installFailureProbability = probability;
      else stratum.healthFailureProbability = probability;
    }
    total += fraction;
    result.push_back(std::move(stratum));
  }
  if (std::abs(total - 1.0) > 0.000001) return shared::Result<std::vector<SimulationStratum>>::failure({"INVALID_SCENARIO", std::string(key) + " fractions must sum to 1.", 422});
  return shared::Result<std::vector<SimulationStratum>>::success(std::move(result));
}

const SimulationStratum& chooseStratum(Pcg64V1& generator, const std::vector<SimulationStratum>& strata) {
  if (strata.size() == 1) return strata.front();
  const auto pick = generator.unit();
  double cumulative = 0.0;
  for (const auto& stratum : strata) {
    cumulative += stratum.fraction;
    if (pick < cumulative) return stratum;
  }
  return strata.back();
}

}  // namespace

bool CohortDevice::operator==(const CohortDevice& other) const { return id == other.id && stableKey == other.stableKey && hardwareModel == other.hardwareModel && architecture == other.architecture && labels == other.labels && observedArtifactDigest == other.observedArtifactDigest && observedGeneration == other.observedGeneration; }
bool CohortMember::operator==(const CohortMember& other) const { return device == other.device && cohortHash == other.cohortHash && ordinal == other.ordinal && stage == other.stage; }

shared::Result<CohortPlan> CohortPlanner::plan(std::string_view releaseId, std::string_view salt, std::vector<CohortDevice> devices, const std::vector<int>& stagePercentages) {
  if (devices.empty()) return shared::Result<CohortPlan>::failure({"EMPTY_COHORT", "No eligible devices matched the frozen selector.", 422});
  if (stagePercentages.empty() || stagePercentages.back() != 100) return shared::Result<CohortPlan>::failure({"INVALID_STAGE_PLAN", "Stage percentages must end at 100.", 422});
  for (const auto percentage : stagePercentages) if (percentage < 1 || percentage > 100) return shared::Result<CohortPlan>::failure({"INVALID_STAGE_PLAN", "Stage percentages must be between 1 and 100.", 422});
  for (std::size_t index = 1; index < stagePercentages.size(); ++index) if (stagePercentages[index] <= stagePercentages[index - 1]) return shared::Result<CohortPlan>::failure({"INVALID_STAGE_PLAN", "Stage percentages must increase.", 422});
  std::map<std::string, std::vector<CohortMember>> strata;
  for (auto& device : devices) {
    const auto key = deviceKey(device);
    const auto hash = shared::DigestService::hmacSha256Hex(salt, std::string(releaseId) + device.stableKey);
    strata[key].push_back({std::move(device), hash, 0, 0});
  }
  for (auto& [stratum, members] : strata) {
    (void)stratum;
    std::sort(members.begin(), members.end(), [](const auto& left, const auto& right) { return left.cohortHash == right.cohortHash ? left.device.stableKey < right.device.stableKey : left.cohortHash < right.cohortHash; });
  }
  CohortPlan result;
  result.members.reserve(devices.size());
  std::vector<std::string> stratumKeys;
  stratumKeys.reserve(strata.size());
  for (const auto& [key, members] : strata) {
    (void)members;
    stratumKeys.push_back(key);
  }
  std::vector<std::vector<std::size_t>> cumulative(stratumKeys.size(), std::vector<std::size_t>(stagePercentages.size(), 0));
  for (std::size_t stage = 0; stage < stagePercentages.size(); ++stage) {
    const auto targetTotal = std::max<std::size_t>(1, (devices.size() * static_cast<std::size_t>(stagePercentages[stage]) + 99U) / 100U);
    std::size_t allocated = 0;
    std::vector<std::size_t> remainders(stratumKeys.size(), 0);
    for (std::size_t stratum = 0; stratum < stratumKeys.size(); ++stratum) {
      const auto count = strata.at(stratumKeys[stratum]).size();
      const auto numerator = count * static_cast<std::size_t>(stagePercentages[stage]);
      const auto base = numerator / 100U;
      remainders[stratum] = numerator % 100U;
      cumulative[stratum][stage] = std::max(stage == 0 ? std::size_t{0} : cumulative[stratum][stage - 1], base);
      allocated += cumulative[stratum][stage];
    }
    while (allocated < targetTotal) {
      std::size_t selected = stratumKeys.size();
      for (std::size_t stratum = 0; stratum < stratumKeys.size(); ++stratum) {
        const auto count = strata.at(stratumKeys[stratum]).size();
        if (cumulative[stratum][stage] >= count) continue;
        if (selected == stratumKeys.size() || remainders[stratum] > remainders[selected] || (remainders[stratum] == remainders[selected] && stratumKeys[stratum] < stratumKeys[selected])) selected = stratum;
      }
      if (selected == stratumKeys.size()) break;
      ++cumulative[selected][stage];
      remainders[selected] = 0;
      ++allocated;
    }
  }
  for (std::size_t stratum = 0; stratum < stratumKeys.size(); ++stratum) {
    auto& members = strata.at(stratumKeys[stratum]);
    for (std::size_t index = 0; index < members.size(); ++index) {
      auto& member = members[index];
      member.ordinal = static_cast<int>(result.members.size());
      member.stage = static_cast<int>(stagePercentages.size());
      for (std::size_t stage = 0; stage < stagePercentages.size(); ++stage) if (index < cumulative[stratum][stage]) { member.stage = static_cast<int>(stage + 1); break; }
      result.members.push_back(member);
    }
  }
  std::string serialized;
  serialized.reserve(result.members.size() * 256 + 2);
  serialized.push_back('[');
  for (std::size_t index = 0; index < result.members.size(); ++index) {
    if (index > 0) serialized.push_back(',');
    appendCanonicalCohortMember(serialized, result.members[index]);
  }
  serialized.push_back(']');
  result.digest = shared::DigestService::sha256Hex(serialized);
  return shared::Result<CohortPlan>::success(std::move(result));
}

std::string toString(ReleaseState state) {
  switch (state) {
    case ReleaseState::draft: return "draft"; case ReleaseState::validating: return "validating"; case ReleaseState::blocked: return "blocked"; case ReleaseState::ready: return "ready"; case ReleaseState::awaiting_approval: return "awaiting_approval"; case ReleaseState::scheduled: return "scheduled"; case ReleaseState::running: return "running"; case ReleaseState::paused: return "paused"; case ReleaseState::aborting: return "aborting"; case ReleaseState::rolling_back: return "rolling_back"; case ReleaseState::completed: return "completed"; case ReleaseState::aborted: return "aborted"; case ReleaseState::rolled_back: return "rolled_back"; case ReleaseState::failed: return "failed"; case ReleaseState::cancelled: return "cancelled";
  }
  return "failed";
}

std::string toString(ReleaseAction action) {
  switch (action) { case ReleaseAction::validate: return "validate"; case ReleaseAction::submit: return "submit"; case ReleaseAction::approve: return "approve"; case ReleaseAction::schedule: return "schedule"; case ReleaseAction::start: return "start"; case ReleaseAction::cancel: return "cancel"; case ReleaseAction::pause: return "pause"; case ReleaseAction::resume: return "resume"; case ReleaseAction::abort: return "abort"; case ReleaseAction::rollback: return "rollback"; case ReleaseAction::gate_pass: return "gate_pass"; case ReleaseAction::gate_pause: return "gate_pause"; case ReleaseAction::gate_rollback: return "gate_rollback"; case ReleaseAction::gate_advance: return "gate_advance"; case ReleaseAction::gate_override: return "gate_override"; case ReleaseAction::security_block: return "security_block"; }
  return "unknown";
}

std::optional<ReleaseState> releaseStateFromString(std::string_view value) {
  static const std::pair<std::string_view, ReleaseState> states[]{{"draft", ReleaseState::draft}, {"validating", ReleaseState::validating}, {"blocked", ReleaseState::blocked}, {"ready", ReleaseState::ready}, {"awaiting_approval", ReleaseState::awaiting_approval}, {"scheduled", ReleaseState::scheduled}, {"running", ReleaseState::running}, {"paused", ReleaseState::paused}, {"aborting", ReleaseState::aborting}, {"rolling_back", ReleaseState::rolling_back}, {"completed", ReleaseState::completed}, {"aborted", ReleaseState::aborted}, {"rolled_back", ReleaseState::rolled_back}, {"failed", ReleaseState::failed}, {"cancelled", ReleaseState::cancelled}};
  for (const auto& [name, state] : states) if (name == value) return state;
  return std::nullopt;
}

std::optional<ReleaseState> ReleaseStateMachine::transition(ReleaseState current, ReleaseAction action) {
  if (current == ReleaseState::draft && action == ReleaseAction::validate) return ReleaseState::validating;
  if (current == ReleaseState::validating && action == ReleaseAction::validate) return ReleaseState::ready;
  if (current == ReleaseState::blocked && action == ReleaseAction::validate) return ReleaseState::validating;
  if (current == ReleaseState::ready && action == ReleaseAction::submit) return ReleaseState::awaiting_approval;
  if (current == ReleaseState::awaiting_approval && action == ReleaseAction::approve) return ReleaseState::ready;
  if (current == ReleaseState::ready && action == ReleaseAction::schedule) return ReleaseState::scheduled;
  if (current == ReleaseState::ready && action == ReleaseAction::start) return ReleaseState::running;
  if (current == ReleaseState::scheduled && action == ReleaseAction::start) return ReleaseState::running;
  if ((current == ReleaseState::draft || current == ReleaseState::validating || current == ReleaseState::blocked || current == ReleaseState::ready || current == ReleaseState::awaiting_approval || current == ReleaseState::scheduled) && action == ReleaseAction::cancel) return ReleaseState::cancelled;
  if (current == ReleaseState::running && action == ReleaseAction::pause) return ReleaseState::paused;
  if (current == ReleaseState::paused && action == ReleaseAction::resume) return ReleaseState::running;
  if ((current == ReleaseState::running || current == ReleaseState::paused) && action == ReleaseAction::abort) return ReleaseState::aborting;
  if ((current == ReleaseState::running || current == ReleaseState::paused) && action == ReleaseAction::rollback) return ReleaseState::rolling_back;
  if (current == ReleaseState::paused && action == ReleaseAction::gate_pass) return ReleaseState::running;
  if (current == ReleaseState::paused && action == ReleaseAction::gate_override) return ReleaseState::running;
  if (current == ReleaseState::running && action == ReleaseAction::gate_pause) return ReleaseState::paused;
  if (current == ReleaseState::running && action == ReleaseAction::gate_rollback) return ReleaseState::rolling_back;
  if (current == ReleaseState::running && action == ReleaseAction::gate_advance) return ReleaseState::running;
  if (current == ReleaseState::aborting && action == ReleaseAction::abort) return ReleaseState::aborted;
  if (current == ReleaseState::rolling_back && action == ReleaseAction::rollback) return ReleaseState::rolled_back;
  if (current == ReleaseState::running && action == ReleaseAction::gate_pass) return ReleaseState::completed;
  if ((current == ReleaseState::draft || current == ReleaseState::validating || current == ReleaseState::ready || current == ReleaseState::awaiting_approval || current == ReleaseState::scheduled || current == ReleaseState::paused) && action == ReleaseAction::security_block) return ReleaseState::blocked;
  return std::nullopt;
}

std::string toString(GateDecision decision) { switch (decision) { case GateDecision::insufficient_evidence: return "insufficient_evidence"; case GateDecision::pass: return "pass"; case GateDecision::pause: return "pause"; case GateDecision::abort: return "abort"; case GateDecision::rollback: return "rollback"; } return "insufficient_evidence"; }

namespace {

bool exceedsRate(int failures, int denominator, double rate) {
  if (denominator <= 0 || !std::isfinite(rate) || rate < 0.0) return false;
  return failures > static_cast<int>(std::floor(rate * static_cast<double>(denominator)));
}

double policyRatio(const shared::Json& gates, const char* key, double fallback) {
  if (!gates.is_object() || !gates.contains(key) || !gates.at(key).is_number()) return fallback;
  const auto value = gates.at(key).get<double>();
  if (!std::isfinite(value) || value < 0.0) return fallback;
  return value > 1.0 ? value / 100.0 : value;
}

}  // namespace

GateThresholds GateEvaluator::thresholdsFromPolicy(const shared::Json& policy) {
  GateThresholds thresholds;
  auto gates = policy.value("health_gates", shared::Json::object());
  if ((!gates.is_object() || gates.empty()) && policy.contains("health_gates_json") && policy.at("health_gates_json").is_string()) {
    try { gates = shared::Json::parse(policy.at("health_gates_json").get<std::string>()); } catch (const std::exception&) { gates = shared::Json::object(); }
  }
  thresholds.freshCoverage = policyRatio(gates, "fresh_device_coverage", thresholds.freshCoverage);
  thresholds.installFailurePause = policyRatio(gates, "install_failure_rate", thresholds.installFailurePause);
  thresholds.installFailureRollback = std::max(thresholds.installFailureRollback, std::min(1.0, thresholds.installFailurePause * 5.0));
  thresholds.healthFailurePause = policyRatio(gates, "health_failure_rate", thresholds.healthFailurePause);
  thresholds.healthFailureRollback = std::max(thresholds.healthFailureRollback, std::min(1.0, thresholds.healthFailurePause * 5.0));
  thresholds.convergence = policyRatio(gates, "convergence_rate", thresholds.convergence);
  thresholds.offlineFraction = policyRatio(gates, "offline_fraction", thresholds.offlineFraction);
  thresholds.rollbackFailure = policyRatio(gates, "rollback_failure_rate", thresholds.rollbackFailure);
  if (gates.is_object() && gates.contains("crash_free_rate") && gates.at("crash_free_rate").is_number()) {
    auto value = gates.at("crash_free_rate").get<double>();
    if (std::isfinite(value)) thresholds.crashFreePausePercent = value <= 1.0 ? value * 100.0 : value;
  }
  return thresholds;
}

std::vector<std::string> GateEvaluator::failedGates(const GateMetrics& metrics, const GateThresholds& thresholds) {
  std::vector<std::string> failed;
  if (metrics.assigned <= 0 || metrics.fresh < static_cast<int>(std::ceil(thresholds.freshCoverage * metrics.assigned))) failed.emplace_back("fresh_device_coverage");
  if (metrics.assigned > 0 && exceedsRate(metrics.installFailures, metrics.assigned, thresholds.installFailurePause)) failed.emplace_back("install_failure_rate");
  if (metrics.assigned > 0 && exceedsRate(metrics.healthFailures, metrics.assigned, thresholds.healthFailurePause)) failed.emplace_back("health_failure_rate");
  if (metrics.crashFreePercent < thresholds.crashFreePausePercent) failed.emplace_back("crash_free_rate");
  if (metrics.assigned > 0 && metrics.converged < static_cast<int>(std::ceil(thresholds.convergence * metrics.assigned))) failed.emplace_back("convergence_rate");
  if (metrics.assigned > 0 && exceedsRate(metrics.offline, metrics.assigned, thresholds.offlineFraction)) failed.emplace_back("offline_fraction");
  if (metrics.assigned > 0 && exceedsRate(metrics.rollbackFailures, metrics.assigned, thresholds.rollbackFailure)) failed.emplace_back("rollback_failure_rate");
  return failed;
}

GateDecision GateEvaluator::evaluate(const GateMetrics& metrics) { return evaluate(metrics, GateThresholds{}); }

GateDecision GateEvaluator::evaluate(const GateMetrics& metrics, const GateThresholds& thresholds) {
  if (metrics.assigned <= 0) return GateDecision::insufficient_evidence;
  if (exceedsRate(metrics.installFailures, metrics.assigned, thresholds.installFailureRollback) || metrics.crashFreePercent < thresholds.crashFreeRollbackPercent || exceedsRate(metrics.healthFailures, metrics.assigned, thresholds.healthFailureRollback) || exceedsRate(metrics.rollbackFailures, metrics.assigned, thresholds.rollbackFailure)) return GateDecision::rollback;
  if (exceedsRate(metrics.installFailures, metrics.assigned, thresholds.installFailurePause) || metrics.crashFreePercent < thresholds.crashFreePausePercent || exceedsRate(metrics.healthFailures, metrics.assigned, thresholds.healthFailurePause) || exceedsRate(metrics.offline, metrics.assigned, thresholds.offlineFraction) || metrics.converged < static_cast<int>(std::ceil(metrics.assigned * thresholds.convergence))) return GateDecision::pause;
  if (metrics.fresh < static_cast<int>(std::ceil(metrics.assigned * thresholds.freshCoverage))) return GateDecision::insufficient_evidence;
  return GateDecision::pass;
}

shared::Result<SimulationResult> Simulator::run(const shared::Json& input, std::uint64_t seed) {
  return run(input, seed, {});
}

shared::Result<SimulationResult> Simulator::run(const shared::Json& input, std::uint64_t seed, const std::function<bool()>& shouldCancel) {
  const int deviceCount = input.value("device_count", 100);
  const double failureProbability = input.value("failure_probability", 0.0);
  const int duration = input.value("duration_seconds", 3600);
  const double onlineProbability = input.value("online_probability", 1.0);
  const double lossProbability = input.value("message_loss_probability", 0.0);
  const double duplicationProbability = input.value("message_duplication_probability", 0.0);
  const double reorderProbability = input.value("message_reorder_probability", 0.0);
  const double rollbackSuccessProbability = input.value("rollback_success_probability", 1.0);
  const double healthFailureProbability = input.value("health_failure_probability", 0.0);
  const double restartProbability = input.value("device_restart_probability", 0.0);
  const double controlPlaneRestartProbability = input.value("control_plane_restart_probability", 0.0);
  const double databaseOutageProbability = input.value("database_outage_probability", 0.0);
  const double iotAdapterOutageProbability = input.value("iot_adapter_outage_probability", 0.0);
  const int meanOfflineDuration = input.value("mean_offline_duration_seconds", 300);
  const auto connectivityMode = input.value("connectivity_mode", "steady");
  const auto installDurationDistribution = input.value("install_duration_distribution", "bounded_uniform");
  const double flapProbability = input.value("flap_probability", 0.0);
  const int installDurationSeconds = input.value("install_duration_seconds", 60);
  const int controlPlaneRestartAt = input.value("control_plane_restart_at_seconds", duration / 2);
  const int databaseOutageAt = input.value("database_outage_at_seconds", duration / 3);
  const int iotAdapterOutageAt = input.value("iot_adapter_outage_at_seconds", duration / 2);
  const int outageRecoverySeconds = input.value("outage_recovery_seconds", std::max(1, duration / 10));
  std::uint64_t maxEvents = 10000000ULL;
  if (input.contains("max_events")) {
    if (!input.at("max_events").is_number_unsigned() && !input.at("max_events").is_number_integer()) return shared::Result<SimulationResult>::failure({"INVALID_SCENARIO", "max_events must be an integer.", 422});
    try { maxEvents = input.at("max_events").get<std::uint64_t>(); } catch (const std::exception&) { return shared::Result<SimulationResult>::failure({"INVALID_SCENARIO", "max_events must be an integer.", 422}); }
  }
  const auto schema = input.value("schema_version", "v1");
  const auto hardwareStrata = parseStrata(input, "hardware_strata", "default-hardware");
  const auto architectureStrata = parseStrata(input, "architecture_strata", "default-architecture");
  const auto regionStrata = parseStrata(input, "region_strata", "default-region");
  if (!hardwareStrata.ok()) return shared::Result<SimulationResult>::failure(*hardwareStrata.error);
  if (!architectureStrata.ok()) return shared::Result<SimulationResult>::failure(*architectureStrata.error);
  if (!regionStrata.ok()) return shared::Result<SimulationResult>::failure(*regionStrata.error);
  const auto probabilityFields = {failureProbability, onlineProbability, lossProbability, duplicationProbability, reorderProbability, rollbackSuccessProbability,
                                  healthFailureProbability, restartProbability, controlPlaneRestartProbability, databaseOutageProbability, iotAdapterOutageProbability, flapProbability};
  const bool probabilitiesAreValid = std::all_of(probabilityFields.begin(), probabilityFields.end(), [](double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; });
  if (schema != "v1" || deviceCount <= 0 || deviceCount > 1000000 || !probabilitiesAreValid || duration <= 0 || meanOfflineDuration <= 0 || installDurationSeconds <= 0 || outageRecoverySeconds <= 0 ||
      controlPlaneRestartAt < 0 || controlPlaneRestartAt > duration || databaseOutageAt < 0 || databaseOutageAt > duration || iotAdapterOutageAt < 0 || iotAdapterOutageAt > duration ||
      maxEvents == 0 || maxEvents > 100000000ULL ||
      (connectivityMode != "steady" && connectivityMode != "flapping") || (installDurationDistribution != "bounded_uniform" && installDurationDistribution != "fixed")) {
    return shared::Result<SimulationResult>::failure({"INVALID_SCENARIO", "Scenario schema, bounds, probabilities, or distributions are invalid.", 422});
  }
  if (shouldCancel && shouldCancel()) return shared::Result<SimulationResult>::failure({"SIMULATION_CANCELLED", "The simulation was cancelled before event generation.", 409});
  Pcg64V1 generator(seed);
  int failures = 0;
  int offlineDevices = 0;
  int healthFailures = 0;
  int duplicateMessages = 0;
  int lostMessages = 0;
  int restarts = 0;
  int rollbackFailures = 0;
  int reorderedMessages = 0;
  int flappingDevices = 0;
  std::priority_queue<SimulatedEvent, std::vector<SimulatedEvent>, EventOrder> events;
  std::uint64_t eventSequence = 0;
  const bool databaseOutage = generator.unit() < databaseOutageProbability;
  const bool controlPlaneRestart = generator.unit() < controlPlaneRestartProbability;
  const bool iotAdapterOutage = generator.unit() < iotAdapterOutageProbability;
  if (databaseOutage) {
    events.push({databaseOutageAt, 1, eventSequence++, {{"type", "database_outage"}}});
    events.push({std::min<std::int64_t>(duration, databaseOutageAt + outageRecoverySeconds), 1, eventSequence++, {{"type", "database_recovered"}}});
  }
  if (controlPlaneRestart) {
    events.push({controlPlaneRestartAt, 1, eventSequence++, {{"type", "control_plane_restart"}}});
    events.push({std::min<std::int64_t>(duration, controlPlaneRestartAt + outageRecoverySeconds), 1, eventSequence++, {{"type", "control_plane_recovered"}}});
  }
  if (iotAdapterOutage) {
    events.push({iotAdapterOutageAt, 2, eventSequence++, {{"type", "iot_adapter_outage"}}});
    events.push({std::min<std::int64_t>(duration, iotAdapterOutageAt + outageRecoverySeconds), 2, eventSequence++, {{"type", "iot_adapter_recovered"}}});
  }
  std::map<std::string, int> hardwareCounts;
  std::map<std::string, int> hardwareFailures;
  std::map<std::string, int> regionCounts;
  std::map<std::string, int> regionHealthFailures;
  for (int index = 0; index < deviceCount; ++index) {
    if ((index % 256 == 0) && shouldCancel && shouldCancel()) return shared::Result<SimulationResult>::failure({"SIMULATION_CANCELLED", "The simulation was cancelled while generating events.", 409});
    const auto& hardware = chooseStratum(generator, *hardwareStrata.value);
    const auto& architecture = chooseStratum(generator, *architectureStrata.value);
    const auto& region = chooseStratum(generator, *regionStrata.value);
    const bool online = generator.unit() <= onlineProbability;
    const bool failed = generator.unit() < hardware.installFailureProbability.value_or(failureProbability);
    const bool unhealthy = generator.unit() < region.healthFailureProbability.value_or(healthFailureProbability);
    const bool restarted = generator.unit() < restartProbability;
    const bool reordered = generator.unit() < reorderProbability;
    const bool flapping = connectivityMode == "flapping" && generator.unit() < flapProbability;
    const auto installAt = static_cast<std::int64_t>(generator.next() % static_cast<std::uint64_t>(duration));
    const auto installDuration = installDurationDistribution == "fixed" ? static_cast<std::int64_t>(installDurationSeconds) : static_cast<std::int64_t>(1 + generator.next() % 300ULL);
    const auto reportAt = std::min<std::int64_t>(duration, installAt + installDuration + (reordered ? static_cast<std::int64_t>(1 + generator.next() % 30ULL) : 0));
    const bool lost = generator.unit() < lossProbability;
    const bool duplicated = generator.unit() < duplicationProbability;
    failures += failed ? 1 : 0;
    if (failed) ++hardwareFailures[hardware.name];
    ++hardwareCounts[hardware.name];
    ++regionCounts[region.name];
    if (unhealthy) ++regionHealthFailures[region.name];
    offlineDevices += online ? 0 : 1;
    healthFailures += unhealthy ? 1 : 0;
    restarts += restarted ? 1 : 0;
    lostMessages += lost ? 1 : 0;
    duplicateMessages += duplicated ? 1 : 0;
    reorderedMessages += reordered ? 1 : 0;
    flappingDevices += flapping ? 1 : 0;
    events.push({installAt, 10, eventSequence++, {{"type", "install_started"}, {"device", index}, {"hardware_model", hardware.name}, {"architecture", architecture.name}, {"region", region.name}, {"connectivity", flapping ? "flapping" : online ? "online" : "offline"}}});
    const auto reportType = lost ? "report_lost" : failed ? "install_failed" : "install_succeeded";
    events.push({reportAt, 20, eventSequence++, {{"type", reportType}, {"device", index}, {"hardware_model", hardware.name}, {"architecture", architecture.name}, {"region", region.name}, {"online", online}, {"lost", lost}, {"duplicated", duplicated}, {"reordered", reordered}, {"health_failure", unhealthy}, {"restart", restarted}}});
    if (duplicated && !lost) events.push({reportAt, 20, eventSequence++, {{"type", "duplicate_report"}, {"device", index}, {"sequence", 2}}});
    if (reordered) events.push({reportAt, 21, eventSequence++, {{"type", "reordered_report"}, {"device", index}, {"delay_seconds", reportAt - installAt}}});
    if (restarted) events.push({std::min<std::int64_t>(duration, reportAt + 1), 22, eventSequence++, {{"type", "device_restart"}, {"device", index}}});
    if (unhealthy) events.push({std::min<std::int64_t>(duration, reportAt + std::max(1, input.value("health_degradation_after_seconds", 0))), 23, eventSequence++, {{"type", "health_degraded"}, {"device", index}, {"region", region.name}}});
    if (!online) events.push({std::min<std::int64_t>(duration, reportAt + meanOfflineDuration), 30, eventSequence++, {{"type", "reconnect"}, {"device", index}}});
    if (flapping) {
      const auto flapAt = std::min<std::int64_t>(duration, reportAt + std::max<std::int64_t>(1, meanOfflineDuration / 2));
      events.push({flapAt, 25, eventSequence++, {{"type", "connectivity_lost"}, {"device", index}, {"reason", "flapping"}}});
      events.push({std::min<std::int64_t>(duration, flapAt + meanOfflineDuration), 30, eventSequence++, {{"type", "reconnect"}, {"device", index}, {"reason", "flapping"}}});
    }
    if (events.size() > maxEvents) return shared::Result<SimulationResult>::failure({"SCENARIO_TOO_MANY_EVENTS", "The scenario exceeds the bounded event limit.", 422});
  }
  shared::Json trace = shared::Json::array();
  while (!events.empty()) {
    if ((trace.size() % 1024 == 0) && shouldCancel && shouldCancel()) return shared::Result<SimulationResult>::failure({"SIMULATION_CANCELLED", "The simulation was cancelled while processing events.", 409});
    auto event = events.top().payload;
    event["at"] = events.top().at;
    event["priority"] = events.top().priority;
    event["sequence"] = events.top().sequence;
    trace.push_back(std::move(event));
    events.pop();
  }
  const int reachableDevices = deviceCount - offlineDevices;
  const int convergedDevices = std::max(0, reachableDevices - failures - healthFailures - lostMessages);
  const bool rollbackTriggered = failures > std::max(0, deviceCount / 20) || healthFailures > std::max(0, deviceCount / 10);
  const int reconnectingDevices = offlineDevices;
  const int rollbackTargetDevices = rollbackTriggered ? std::min(deviceCount, std::max(reconnectingDevices, failures + healthFailures)) : 0;
  int rollbackConvergedDevices = 0;
  if (rollbackTriggered) {
    for (int index = 0; index < rollbackTargetDevices; ++index) if (generator.unit() < rollbackSuccessProbability) ++rollbackConvergedDevices;
    rollbackFailures = rollbackTargetDevices - rollbackConvergedDevices;
  }
  const double rollbackConvergenceFraction = rollbackTargetDevices == 0 ? 1.0 : static_cast<double>(rollbackConvergedDevices) / rollbackTargetDevices;
  shared::Json hardwareCountJson = shared::Json::object();
  shared::Json hardwareFailureJson = shared::Json::object();
  shared::Json regionCountJson = shared::Json::object();
  shared::Json regionFailureJson = shared::Json::object();
  for (const auto& [name, count] : hardwareCounts) hardwareCountJson[name] = count;
  for (const auto& [name, count] : hardwareFailures) hardwareFailureJson[name] = count;
  for (const auto& [name, count] : regionCounts) regionCountJson[name] = count;
  for (const auto& [name, count] : regionHealthFailures) regionFailureJson[name] = count;
  SimulationResult result;
  result.trace = trace;
  result.metrics = {{"device_count", deviceCount}, {"reachable_devices", reachableDevices}, {"offline_devices", offlineDevices}, {"reconnecting_devices", reconnectingDevices}, {"install_failures", failures}, {"failure_rate", static_cast<double>(failures) / deviceCount}, {"health_failures", healthFailures}, {"rollback_failures", rollbackFailures}, {"rollback_target_devices", rollbackTargetDevices}, {"rollback_converged_devices", rollbackConvergedDevices}, {"rollback_convergence_fraction", rollbackConvergenceFraction}, {"duplicate_messages", duplicateMessages}, {"lost_messages", lostMessages}, {"reordered_messages", reorderedMessages}, {"restarts", restarts}, {"flapping_devices", flappingDevices}, {"database_outages", databaseOutage ? 1 : 0}, {"control_plane_restarts", controlPlaneRestart ? 1 : 0}, {"iot_adapter_outages", iotAdapterOutage ? 1 : 0}, {"rollback_triggered", rollbackTriggered}, {"pause_triggered", rollbackTriggered || failures > std::max(0, deviceCount / 100)}, {"converged_devices", convergedDevices}, {"stranded_devices", std::max(0, deviceCount - convergedDevices)}, {"false_rollback", rollbackTriggered && failures == 0 && healthFailures == 0 ? 1 : 0}, {"hardware_counts", hardwareCountJson}, {"hardware_failures", hardwareFailureJson}, {"region_counts", regionCountJson}, {"region_health_failures", regionFailureJson}, {"seed", seed}, {"duration_seconds", duration}, {"simulated_event_count", trace.size()}, {"max_events", maxEvents}, {"connectivity_mode", connectivityMode}, {"install_duration_distribution", installDurationDistribution}};
  result.traceDigest = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(result.trace));
  result.resultDigest = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(result.metrics) + result.traceDigest);
  return shared::Result<SimulationResult>::success(std::move(result));
}

std::vector<std::uint64_t> Simulator::pcg64GoldenVector(std::uint64_t seed, std::size_t count) {
  if (count > 1024) count = 1024;
  Pcg64V1 generator(seed);
  std::vector<std::uint64_t> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) result.push_back(generator.next());
  return result;
}

}  // namespace edgefleet::domain
