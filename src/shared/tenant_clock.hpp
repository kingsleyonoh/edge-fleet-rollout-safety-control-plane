#pragma once

#include <chrono>
#include <string>

namespace edgefleet::shared {

class TenantClock {
 public:
  static std::chrono::system_clock::time_point now();
  static std::string nowIso8601();
};

}  // namespace edgefleet::shared
