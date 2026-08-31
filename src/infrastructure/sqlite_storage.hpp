#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "infrastructure/storage.hpp"

namespace edgefleet::infrastructure {

class SqliteStorage final : public Storage {
 public:
  explicit SqliteStorage(std::string path, int busyTimeoutMs = 5000);
  ~SqliteStorage() override;
  SqliteStorage(const SqliteStorage&) = delete;
  SqliteStorage& operator=(const SqliteStorage&) = delete;

  bool open() override;
  void close();
  bool migrate(const std::string& directory) override;
  bool healthy() const override;
  int schemaVersion() const override;
  const std::string& lastError() const override { return lastError_; }

  std::optional<shared::Json> createTenant(const std::string& name, const std::string& legalName,
                                           const std::string& displayName, const std::string& timezone,
                                           const std::string& apiPrefix, const std::string& apiHash) override;
  std::optional<shared::Json> findPrincipal(const std::string& apiPrefix, const std::string& apiHash) override;
  std::optional<shared::Json> getTenant(const std::string& tenantId) override;
  bool updateTenant(const std::string& tenantId, const shared::Json& fields) override;

  std::optional<shared::Json> createFleet(const std::string& tenantId, const std::string& slug,
                                          const std::string& name, const std::string& environment,
                                          const shared::Json& labelSchema = shared::Json::object()) override;
  std::vector<shared::Json> listFleets(const std::string& tenantId) override;
  std::optional<shared::Json> getFleet(const std::string& tenantId, const std::string& fleetId) override;

  std::optional<shared::Json> createDevice(const std::string& tenantId, const std::string& fleetId,
                                           const shared::Json& fields, const std::string& secretHash) override;
  std::vector<shared::Json> listDevices(const std::string& tenantId, const std::string& fleetId = "") override;
  std::optional<shared::Json> getDevice(const std::string& tenantId, const std::string& deviceId) override;

  std::optional<shared::Json> createPolicy(const std::string& tenantId, const shared::Json& fields) override;
  std::vector<shared::Json> listPolicies(const std::string& tenantId) override;
  std::optional<shared::Json> getPolicy(const std::string& tenantId, const std::string& policyId) override;

  std::optional<shared::Json> createRelease(const std::string& tenantId, const shared::Json& fields) override;
  std::vector<shared::Json> listReleases(const std::string& tenantId) override;
  std::optional<shared::Json> getRelease(const std::string& tenantId, const std::string& releaseId) override;
  bool updateRelease(const std::string& tenantId, const std::string& releaseId, const std::string& status, int expectedVersion) override;

  std::optional<shared::Json> appendEvidence(const std::string& tenantId, const std::string& eventType,
                                             const std::string& aggregateType, const std::string& aggregateId,
                                             const shared::Json& payload, const std::string& actorType = "operator",
                                             const std::string& actorId = "system") override;
  shared::Json verifyEvidence(const std::string& tenantId) const override;
  bool corruptFirstEvidenceForTest(const std::string& tenantId) override;

  std::vector<shared::Json> query(const std::string& sql, const std::vector<std::string>& params = {}) const override;
  bool execute(const std::string& sql, const std::vector<std::string>& params = {}) override;
  bool transaction(const std::function<bool()>& operation) override;

 private:
  bool executeUnlocked(const std::string& sql, const std::vector<std::string>& params);
  std::vector<shared::Json> queryUnlocked(const std::string& sql, const std::vector<std::string>& params) const;
  std::optional<shared::Json> first(const std::string& sql, const std::vector<std::string>& params) const;
  std::string now() const;
  void setError(std::string message) const;

  std::string path_;
  int busyTimeoutMs_ = 5000;
  sqlite3* db_ = nullptr;
  mutable std::recursive_mutex mutex_;
  bool inTransaction_ = false;
  mutable std::string lastError_;
};

}  // namespace edgefleet::infrastructure
