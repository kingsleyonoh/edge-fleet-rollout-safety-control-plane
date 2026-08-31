#include "domain/replay.hpp"

#include "domain/safety.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

#include <algorithm>

namespace {

constexpr std::size_t kDivergenceLimit = 1U << 20U;

std::string capped(const std::string& value) { return value.size() <= kDivergenceLimit ? value : value.substr(0, kDivergenceLimit); }

}  // namespace

namespace edgefleet::domain {

shared::Result<ReplayResult> ReplayEngine::simulation(const shared::Json& frozenInput, std::uint64_t seed,
                                                      const std::string& expectedDigest) {
  const auto result = Simulator::run(frozenInput, seed);
  if (!result.ok()) return shared::Result<ReplayResult>::failure(*result.error);
  ReplayResult replay;
  replay.expectedDigest = expectedDigest;
  replay.actualDigest = result.value->resultDigest;
  replay.status = replay.expectedDigest == replay.actualDigest ? "reproduced" : "diverged";
  if (replay.status == "diverged") replay.divergence = { {"kind", "result_digest"}, {"expected", replay.expectedDigest}, {"actual", replay.actualDigest}, {"source", "simulation"} };
  return shared::Result<ReplayResult>::success(std::move(replay));
}

shared::Result<ReplayResult> ReplayEngine::evidence(const shared::Json& events, const std::string& expectedDigest) {
  return evidence(events, shared::Json{}, expectedDigest);
}

shared::Result<ReplayResult> ReplayEngine::evidence(const shared::Json& events, const shared::Json& expectedEvents,
                                                    const std::string& expectedDigest) {
  if (!events.is_array() || events.empty()) return shared::Result<ReplayResult>::failure({"EMPTY_REPLAY_SOURCE", "Replay requires one non-empty frozen evidence source.", 422});
  const auto actual = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(events));
  ReplayResult replay;
  replay.expectedDigest = expectedDigest;
  replay.actualDigest = actual;
  std::optional<std::size_t> firstDivergence;
  if (expectedEvents.is_array()) {
    const auto common = (std::min)(events.size(), expectedEvents.size());
    for (std::size_t index = 0; index < common; ++index) {
      if (shared::CanonicalJson::serialize(events.at(index)) != shared::CanonicalJson::serialize(expectedEvents.at(index))) {
        firstDivergence = index;
        break;
      }
    }
    if (!firstDivergence.has_value() && events.size() != expectedEvents.size()) firstDivergence = common;
  }
  replay.status = replay.expectedDigest == replay.actualDigest && !firstDivergence.has_value() ? "reproduced" : "diverged";
  if (replay.status == "diverged") {
    replay.divergence = {{"kind", "evidence_digest"}, {"expected", replay.expectedDigest}, {"actual", replay.actualDigest}};
    if (firstDivergence.has_value()) {
      replay.divergence["first_divergence_index"] = *firstDivergence;
      if (*firstDivergence < expectedEvents.size()) replay.divergence["expected_event"] = shared::Json::parse(capped(shared::CanonicalJson::serialize(expectedEvents.at(*firstDivergence))));
      if (*firstDivergence < events.size()) replay.divergence["actual_event"] = shared::Json::parse(capped(shared::CanonicalJson::serialize(events.at(*firstDivergence))));
    } else {
      replay.divergence["first_divergence_index"] = nullptr;
      replay.divergence["comparison"] = "expected digest differs but no expected event snapshot was retained";
    }
  }
  return shared::Result<ReplayResult>::success(std::move(replay));
}

}  // namespace edgefleet::domain
