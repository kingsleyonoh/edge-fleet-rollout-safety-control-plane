#pragma once

#include <cstdint>
#include <string>

#include "shared/types.hpp"

namespace edgefleet::domain {

struct ReplayResult {
  std::string status;
  std::string expectedDigest;
  std::string actualDigest;
  shared::Json divergence = shared::Json::object();
};

class ReplayEngine {
 public:
  static shared::Result<ReplayResult> simulation(const shared::Json& frozenInput, std::uint64_t seed,
                                                 const std::string& expectedDigest);
  static shared::Result<ReplayResult> evidence(const shared::Json& events, const std::string& expectedDigest);
  static shared::Result<ReplayResult> evidence(const shared::Json& events, const shared::Json& expectedEvents,
                                               const std::string& expectedDigest);
};

}  // namespace edgefleet::domain
