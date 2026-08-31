#include "shared/job_lease_store.hpp"

#include "shared/types.hpp"

namespace edgefleet::shared {

bool JobLeaseStore::acquire(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                            const std::string& shardKey, const std::string& owner, int leaseSeconds) {
  if (leaseSeconds < 1 || leaseSeconds > 3600) return false;
  const auto id = Uuid::generate().str();
  const auto seconds = std::to_string(leaseSeconds);
  const auto sql = "INSERT INTO job_leases(id,tenant_id,job_name,shard_key,lease_owner,lease_expires_at,heartbeat_at,created_at,updated_at) VALUES(?,?,?,?,?,datetime('now','+" + seconds + " seconds'),datetime('now'),datetime('now'),datetime('now')) ON CONFLICT(tenant_id,job_name,shard_key) DO UPDATE SET lease_owner=excluded.lease_owner,lease_expires_at=excluded.lease_expires_at,heartbeat_at=excluded.heartbeat_at,updated_at=excluded.updated_at WHERE job_leases.lease_expires_at <= datetime('now') OR job_leases.lease_owner=excluded.lease_owner";
  if (!storage.execute(sql, {id, tenantId, jobName, shardKey, owner})) return false;
  return !storage.query("SELECT id FROM job_leases WHERE tenant_id=? AND job_name=? AND shard_key=? AND lease_owner=? AND lease_expires_at > datetime('now')", {tenantId, jobName, shardKey, owner}).empty();
}

bool JobLeaseStore::release(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                            const std::string& shardKey, const std::string& owner) {
  return storage.execute("DELETE FROM job_leases WHERE tenant_id=? AND job_name=? AND shard_key=? AND lease_owner=?", {tenantId, jobName, shardKey, owner});
}

}  // namespace edgefleet::shared
