#pragma once

#include <string>

#include "infrastructure/storage.hpp"

namespace edgefleet::shared {

class JobLeaseStore {
 public:
  static bool acquire(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                      const std::string& shardKey, const std::string& owner, int leaseSeconds);
  static bool release(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                      const std::string& shardKey, const std::string& owner);
};

}  // namespace edgefleet::shared
