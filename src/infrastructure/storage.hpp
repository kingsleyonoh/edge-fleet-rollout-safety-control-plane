#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "shared/types.hpp"

namespace edgefleet::infrastructure {

class Storage {
 public:
  virtual ~Storage() = default;
  virtual bool open() = 0;
  virtual bool migrate(const std::string& directory) = 0;
  virtual bool healthy() const = 0;
  virtual int schemaVersion() const = 0;
  virtual const std::string& lastError() const = 0;

  virtual std::optional<shared::Json> createTenant(const std::string& name, const std::string& legalName,
                                                    const std::string& displayName, const std::string& timezone,
                                                    const std::string& apiPrefix, const std::string& apiHash) = 0;
  virtual std::optional<shared::Json> findPrincipal(const std::string& apiPrefix, const std::string& apiHash) = 0;
  virtual std::optional<shared::Json> getTenant(const std::string& tenantId) = 0;
  virtual bool updateTenant(const std::string& tenantId, const shared::Json& fields) = 0;

  virtual std::optional<shared::Json> createFleet(const std::string& tenantId, const std::string& slug,
                                                   const std::string& name, const std::string& environment,
                                                   const shared::Json& labelSchema = shared::Json::object()) = 0;
  virtual std::vector<shared::Json> listFleets(const std::string& tenantId) = 0;
  virtual std::optional<shared::Json> getFleet(const std::string& tenantId, const std::string& fleetId) = 0;
  virtual std::optional<shared::Json> createDevice(const std::string& tenantId, const std::string& fleetId,
                                                    const shared::Json& fields, const std::string& secretHash) = 0;
  virtual std::vector<shared::Json> listDevices(const std::string& tenantId, const std::string& fleetId = "") = 0;
  virtual std::optional<shared::Json> getDevice(const std::string& tenantId, const std::string& deviceId) = 0;

  virtual std::optional<shared::Json> createPolicy(const std::string& tenantId, const shared::Json& fields) = 0;
  virtual std::vector<shared::Json> listPolicies(const std::string& tenantId) = 0;
  virtual std::optional<shared::Json> getPolicy(const std::string& tenantId, const std::string& policyId) = 0;
  virtual std::optional<shared::Json> createRelease(const std::string& tenantId, const shared::Json& fields) = 0;
  virtual std::vector<shared::Json> listReleases(const std::string& tenantId) = 0;
  virtual std::optional<shared::Json> getRelease(const std::string& tenantId, const std::string& releaseId) = 0;
  virtual bool updateRelease(const std::string& tenantId, const std::string& releaseId, const std::string& status,
                             int expectedVersion) = 0;

  virtual std::optional<shared::Json> appendEvidence(const std::string& tenantId, const std::string& eventType,
                                                      const std::string& aggregateType, const std::string& aggregateId,
                                                      const shared::Json& payload, const std::string& actorType = "operator",
                                                      const std::string& actorId = "system") = 0;
  virtual shared::Json verifyEvidence(const std::string& tenantId) const = 0;
  virtual bool corruptFirstEvidenceForTest(const std::string& tenantId) = 0;
  virtual std::vector<shared::Json> query(const std::string& sql, const std::vector<std::string>& params = {}) const = 0;
  virtual bool execute(const std::string& sql, const std::vector<std::string>& params = {}) = 0;
  virtual bool transaction(const std::function<bool()>& operation) = 0;
};

}  // namespace edgefleet::infrastructure
