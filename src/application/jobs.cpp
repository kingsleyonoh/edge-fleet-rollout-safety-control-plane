#include "application/jobs.hpp"

#include <chrono>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <sstream>

#if defined(EDGEFLEET_HAS_DUCKDB)
#include <duckdb.h>
#endif

#include "application/evidence_export.hpp"
#include "domain/benchmark.hpp"
#include "domain/replay.hpp"
#include "domain/safety.hpp"
#include "infrastructure/integrations.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/http_client_pool.hpp"
#include "shared/job_lease_store.hpp"
#include "shared/logger.hpp"
#include "shared/secret_resolver.hpp"
#include "shared/tenant_clock.hpp"
#include "shared/types.hpp"

namespace edgefleet::application {

namespace {

std::size_t targetCount(std::size_t eligible, int percentage) {
  if (eligible == 0) return 0;
  const auto bounded = std::clamp(percentage, 1, 100);
  return std::min(eligible, std::max<std::size_t>(1, (eligible * static_cast<std::size_t>(bounded) + 99U) / 100U));
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream content;
  content << input.rdbuf();
  return input.is_open() ? content.str() : std::string{};
}

std::string sqlQuote(const std::string& value) {
  std::string result = "'";
  for (const auto character : value) {
    result += character;
    if (character == '\'') result += '\'';
  }
  result += '\'';
  return result;
}

std::optional<domain::ReleaseAction> gateAction(domain::GateDecision decision, bool hasNextStage) {
  if (decision == domain::GateDecision::pass) return hasNextStage ? domain::ReleaseAction::gate_advance : domain::ReleaseAction::gate_pass;
  if (decision == domain::GateDecision::pause) return domain::ReleaseAction::gate_pause;
  if (decision == domain::GateDecision::rollback) return domain::ReleaseAction::gate_rollback;
  if (decision == domain::GateDecision::abort) return domain::ReleaseAction::abort;
  return std::nullopt;
}

bool legalGateTransition(const std::string& current, domain::GateDecision decision, bool hasNextStage) {
  const auto state = domain::releaseStateFromString(current);
  const auto action = gateAction(decision, hasNextStage);
  return state.has_value() && (decision == domain::GateDecision::insufficient_evidence || (action.has_value() && domain::ReleaseStateMachine::transition(*state, *action).has_value()));
}

int reclaimExpiredWork(infrastructure::Storage& storage, const std::string& tenantId) {
  int reclaimed = 0;
  for (const auto& table : {"simulation_runs", "replay_runs", "benchmark_runs", "evidence_exports"}) {
    const auto rows = storage.query("SELECT id FROM " + std::string(table) + " WHERE tenant_id=? AND status='running' AND lease_expires_at <= CURRENT_TIMESTAMP", {tenantId});
    if (rows.empty()) continue;
    if (!storage.execute("UPDATE " + std::string(table) + " SET status='queued',lease_owner=NULL,lease_expires_at=NULL,started_at=NULL WHERE tenant_id=? AND status='running' AND lease_expires_at <= CURRENT_TIMESTAMP", {tenantId})) return -1;
    reclaimed += static_cast<int>(rows.size());
  }
  return reclaimed;
}

bool writeDuckDbReport(const std::filesystem::path& path, const domain::BenchmarkReport& report) {
#if defined(EDGEFLEET_HAS_DUCKDB)
  duckdb_database database = nullptr;
  if (duckdb_open(path.string().c_str(), &database) == DuckDBError || database == nullptr) return false;
  duckdb_connection connection = nullptr;
  if (duckdb_connect(database, &connection) == DuckDBError || connection == nullptr) {
    duckdb_close(&database);
    return false;
  }
  const auto execute = [&connection](const std::string& sql) {
    duckdb_result result{};
    const auto status = duckdb_query(connection, sql.c_str(), &result);
    duckdb_destroy_result(&result);
    return status == DuckDBSuccess;
  };
  if (!execute("CREATE TABLE benchmark_results (scenario VARCHAR, seed BIGINT, strategy VARCHAR, metrics VARCHAR, result_digest VARCHAR)")) {
    duckdb_disconnect(&connection);
    duckdb_close(&database);
    return false;
  }
  for (const auto& cell : report.cells) {
    const auto sql = "INSERT INTO benchmark_results VALUES (" + sqlQuote(cell.scenario) + "," + std::to_string(cell.seed) + "," + sqlQuote(cell.strategy) + "," + sqlQuote(shared::CanonicalJson::serialize(cell.metrics)) + "," + sqlQuote(cell.digest) + ")";
    if (!execute(sql)) {
      duckdb_disconnect(&connection);
      duckdb_close(&database);
      return false;
    }
  }
  duckdb_disconnect(&connection);
  duckdb_close(&database);
  return true;
#else
  (void)path;
  (void)report;
  return false;
#endif
}

int activateNextStage(infrastructure::Storage& storage, const std::string& tenantId, const std::string& releaseId,
                      int completedOrdinal) {
  const auto next = storage.query("SELECT id,ordinal,target_percentage,eligible_count FROM release_stages WHERE tenant_id=? AND release_id=? AND ordinal=? AND status='pending'", {tenantId, releaseId, std::to_string(completedOrdinal + 1)});
  if (next.empty()) return 0;
  const auto stageId = next.front().at("id").get<std::string>();
  const auto eligible = next.front().at("eligible_count").get<std::size_t>();
  const auto percentage = next.front().at("target_percentage").get<int>();
  const auto count = targetCount(eligible, percentage);
  int observationSeconds = 900;
  const auto release = storage.query("SELECT frozen_policy_json FROM releases WHERE tenant_id=? AND id=?", {tenantId, releaseId});
  if (!release.empty()) {
    try {
      const auto policy = shared::Json::parse(release.front().value("frozen_policy_json", "{}"));
      observationSeconds = std::clamp(policy.value("min_observation_sec", observationSeconds), 1, 7 * 24 * 60 * 60);
    } catch (const std::exception&) {
      return -1;
    }
  }
  const auto members = storage.query("SELECT m.device_id FROM release_memberships m LEFT JOIN release_assignments a ON a.tenant_id=m.tenant_id AND a.release_id=m.release_id AND a.device_id=m.device_id WHERE m.tenant_id=? AND m.release_id=? AND m.cohort_ordinal < ? AND a.id IS NULL ORDER BY m.cohort_ordinal", {tenantId, releaseId, std::to_string(count)});
  int assigned = 0;
  for (const auto& member : members) {
    const auto deviceId = member.at("device_id").get<std::string>();
    const auto device = storage.query("SELECT desired_generation FROM devices WHERE tenant_id=? AND id=? AND lifecycle_status NOT IN ('quarantined','decommissioned')", {tenantId, deviceId});
    if (device.empty()) continue;
    const auto generation = std::max<long long>(1, device.front().value("desired_generation", 0LL) + 1);
    const auto assignmentId = shared::Uuid::generate().str();
    const auto assignment = storage.execute("INSERT OR IGNORE INTO release_assignments(id,tenant_id,release_id,stage_id,device_id,desired_artifact_id,desired_generation,state,commanded_at,updated_at) SELECT ?,tenant_id,?,?,?,artifact_id,?,'commanded',datetime('now'),datetime('now') FROM releases WHERE tenant_id=? AND id=?", {assignmentId, releaseId, stageId, deviceId, std::to_string(generation), tenantId, releaseId});
    if (!assignment) continue;
    if (!storage.execute("UPDATE devices SET desired_generation=CASE WHEN desired_generation<? THEN ? ELSE desired_generation END,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {std::to_string(generation), std::to_string(generation), tenantId, deviceId})) return -1;
    if (!storage.execute("INSERT OR IGNORE INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) SELECT ?,tenant_id,release_id,stage_id,id,?,'install',desired_generation,desired_artifact_id,?,?,datetime('now'),datetime('now','+7 days'),datetime('now') FROM release_assignments WHERE tenant_id=? AND id=?", {shared::Uuid::generate().str(), deviceId, shared::CanonicalJson::serialize({{"type", "install"}, {"assignment_id", assignmentId}, {"generation", generation}}), "release-install-" + assignmentId, tenantId, assignmentId})) return -1;
    ++assigned;
  }
  if (!storage.execute("UPDATE release_stages SET status='active',started_at=COALESCE(started_at,datetime('now')),observation_started_at=COALESCE(observation_started_at,datetime('now')),observation_ends_at=COALESCE(observation_ends_at,datetime('now','+" + std::to_string(observationSeconds) + " seconds')),assigned_count=(SELECT COUNT(*) FROM release_assignments WHERE tenant_id=? AND stage_id=?),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='pending'", {tenantId, stageId, tenantId, stageId})) return -1;
  if (!storage.execute("UPDATE releases SET current_stage_ordinal=?,version=version+1,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='running' AND current_stage_ordinal=?", {std::to_string(completedOrdinal + 1), tenantId, releaseId, std::to_string(completedOrdinal)})) return -1;
  const auto advanced = storage.query("SELECT current_stage_ordinal,status FROM releases WHERE tenant_id=? AND id=?", {tenantId, releaseId});
  if (advanced.empty() || advanced.front().value("status", "") != "running" || advanced.front().value("current_stage_ordinal", 0) != completedOrdinal + 1) return -1;
  return assigned;
}

bool issueRollbackCommands(infrastructure::Storage& storage, const std::string& tenantId, const std::string& releaseId) {
  const auto release = storage.query("SELECT r.rollback_artifact_id,a.storage_key,a.sha256_digest,a.status,k.status AS key_status FROM releases r JOIN artifacts a ON a.tenant_id=r.tenant_id AND a.id=r.rollback_artifact_id JOIN artifact_signing_keys k ON k.tenant_id=a.tenant_id AND k.id=a.signature_key_id WHERE r.tenant_id=? AND r.id=? AND r.rollback_artifact_id IS NOT NULL", {tenantId, releaseId});
  if (release.empty()) return false;
  if (release.front().value("status", "") != "ready" || release.front().value("key_status", "") != "active" || release.front().value("storage_key", "").empty() ||
      !shared::DigestService::constantTimeEqual(shared::DigestService::sha256File(release.front().value("storage_key", "")), release.front().value("sha256_digest", ""))) return false;
  const auto rollbackArtifact = release.front().value("rollback_artifact_id", "");
  if (rollbackArtifact.empty()) return false;
  const auto assignments = storage.query("SELECT id,device_id,stage_id FROM release_assignments WHERE tenant_id=? AND release_id=? AND state IN ('commanded','acknowledged','converged','failed','stranded')", {tenantId, releaseId});
  for (const auto& assignment : assignments) {
    const auto device = storage.query("SELECT desired_generation FROM devices WHERE tenant_id=? AND id=?", {tenantId, assignment.at("device_id").get<std::string>()});
    if (device.empty()) continue;
    const auto generation = device.front().value("desired_generation", 0LL) + 1;
    if (!storage.execute("UPDATE release_assignments SET desired_artifact_id=?,desired_generation=?,state='pending',updated_at=datetime('now') WHERE tenant_id=? AND id=?", {rollbackArtifact, std::to_string(generation), tenantId, assignment.at("id").get<std::string>()})) return false;
    if (!storage.execute("UPDATE devices SET desired_generation=CASE WHEN desired_generation<? THEN ? ELSE desired_generation END,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {std::to_string(generation), std::to_string(generation), tenantId, assignment.at("device_id").get<std::string>()})) return false;
    if (!storage.execute("INSERT OR IGNORE INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) VALUES(?,?,?,?,?,?, 'rollback',?,?,?, ?,datetime('now'),datetime('now','+7 days'),datetime('now'))", {shared::Uuid::generate().str(), tenantId, releaseId, assignment.at("stage_id").get<std::string>(), assignment.at("id").get<std::string>(), assignment.at("device_id").get<std::string>(), std::to_string(generation), rollbackArtifact, "{}", "rollback-" + assignment.at("id").get<std::string>() + "-" + std::to_string(generation)})) return false;
  }
  return true;
}

bool settleTerminalRelease(infrastructure::Storage& storage, const std::string& tenantId, const std::string& releaseId, const std::string& owner) {
  const auto release = storage.query("SELECT status FROM releases WHERE tenant_id=? AND id=?", {tenantId, releaseId});
  if (release.empty()) return false;
  const auto status = release.front().value("status", "");
  if (status != "aborting" && status != "rolling_back") return true;
  const auto active = storage.query("SELECT id FROM release_assignments WHERE tenant_id=? AND release_id=? AND state IN ('pending','commanded','acknowledged','cancelling')", {tenantId, releaseId});
  if (!active.empty()) return true;
  const auto assignments = storage.query("SELECT state,failure_code FROM release_assignments WHERE tenant_id=? AND release_id=?", {tenantId, releaseId});
  if (assignments.empty()) return false;
  const bool rollbackFailed = std::any_of(assignments.begin(), assignments.end(), [](const auto& row) { return row.value("state", "") == "failed" && row.value("failure_code", "") == "ROLLBACK_FAILED"; });
  const auto next = status == "aborting" ? std::string("aborted") : rollbackFailed ? std::string("failed") : std::string("rolled_back");
  const auto currentState = domain::releaseStateFromString(status);
  const auto action = status == "aborting" ? domain::ReleaseAction::abort : domain::ReleaseAction::rollback;
  if (!currentState.has_value() || !domain::ReleaseStateMachine::transition(*currentState, action).has_value()) return false;
  const auto current = storage.query("SELECT version FROM releases WHERE tenant_id=? AND id=? AND status=?", {tenantId, releaseId, status});
  if (current.empty() || !storage.updateRelease(tenantId, releaseId, next, current.front().value("version", 0))) return false;
  if (!storage.execute("UPDATE releases SET ended_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status=?", {tenantId, releaseId, next})) return false;
  return storage.appendEvidence(tenantId, "release." + next, "release", releaseId, {{"from", status}, {"to", next}}, "job", owner).has_value();
}

}  // namespace

bool JobCoordinator::acquire(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                             const std::string& shardKey, const std::string& owner, int leaseSeconds) {
  return shared::JobLeaseStore::acquire(storage, tenantId, jobName, shardKey, owner, leaseSeconds);
}

bool JobCoordinator::release(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                             const std::string& shardKey, const std::string& owner) {
  return shared::JobLeaseStore::release(storage, tenantId, jobName, shardKey, owner);
}

int JobCoordinator::recoverExpired(infrastructure::Storage& storage, const std::string& tenantId) {
  const auto before = storage.query("SELECT COUNT(*) AS count FROM job_leases WHERE tenant_id=? AND lease_expires_at <= datetime('now')", {tenantId});
  if (before.empty()) return 0;
  if (!storage.execute("DELETE FROM job_leases WHERE tenant_id=? AND lease_expires_at <= datetime('now')", {tenantId})) return -1;
  return before.front().at("count").get<int>();
}

int MaintenanceJobRunner::expireApprovals(infrastructure::Storage& storage, const std::string& tenantId) {
  const auto rows = storage.query("SELECT id FROM approval_requests WHERE tenant_id=? AND status='requested' AND expires_at <= datetime('now')", {tenantId});
  const auto stale = storage.query("SELECT a.id FROM approval_requests a JOIN releases r ON r.tenant_id=a.tenant_id AND r.id=a.release_id LEFT JOIN health_gate_evaluations g ON g.tenant_id=a.tenant_id AND g.id=a.gate_evaluation_id WHERE a.tenant_id=? AND a.status='requested' AND (a.captured_release_version <> r.version OR (a.action='gate_override' AND (g.id IS NULL OR a.evidence_digest <> g.evidence_digest)))", {tenantId});
  for (const auto& row : rows) {
    if (!storage.execute("UPDATE approval_requests SET status='expired',updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='requested'", {tenantId, row.at("id").get<std::string>()}) ||
        !storage.appendEvidence(tenantId, "approval.expired", "approval", row.at("id").get<std::string>(), {}, "job", "maintenance").has_value()) return -1;
  }
  for (const auto& row : stale) {
    if (!storage.execute("UPDATE approval_requests SET status='superseded',updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='requested'", {tenantId, row.at("id").get<std::string>()}) ||
        !storage.appendEvidence(tenantId, "approval.superseded", "approval", row.at("id").get<std::string>(), {}, "job", "maintenance").has_value()) return -1;
  }
  return static_cast<int>(rows.size() + stale.size());
}

int MaintenanceJobRunner::expireCommands(infrastructure::Storage& storage, const std::string& tenantId) {
  const auto rows = storage.query("SELECT id,assignment_id FROM rollout_commands WHERE tenant_id=? AND expires_at <= datetime('now') AND command_type='install'", {tenantId});
  int expired = 0;
  for (const auto& row : rows) {
    const auto assignment = storage.query("SELECT state FROM release_assignments WHERE tenant_id=? AND id=? AND state IN ('pending','commanded')", {tenantId, row.at("assignment_id").get<std::string>()});
    if (assignment.empty()) continue;
    if (!storage.execute("UPDATE release_assignments SET state='stranded',failure_code='COMMAND_EXPIRED',updated_at=datetime('now') WHERE tenant_id=? AND id=? AND state IN ('pending','commanded')", {tenantId, row.at("assignment_id").get<std::string>()}) ||
        !storage.appendEvidence(tenantId, "rollout.command_expired", "command", row.at("id").get<std::string>(), {{"assignment_id", row.at("assignment_id")}}, "job", "maintenance").has_value()) return -1;
    ++expired;
  }
  return expired;
}

int MaintenanceJobRunner::cleanupIdempotency(infrastructure::Storage& storage, const std::string& tenantId) {
  const auto rows = storage.query("SELECT id FROM idempotency_records WHERE tenant_id=? AND created_at <= datetime('now','-7 days')", {tenantId});
  if (!rows.empty() && !storage.execute("DELETE FROM idempotency_records WHERE tenant_id=? AND created_at <= datetime('now','-7 days')", {tenantId})) return -1;
  return static_cast<int>(rows.size());
}

bool MaintenanceJobRunner::checkpointEvidence(infrastructure::Storage& storage, const std::string& tenantId, std::int64_t checkpointInterval) {
  if (checkpointInterval < 1) return false;
  const auto verification = storage.verifyEvidence(tenantId);
  if (!verification.value("valid", false)) return false;
  const auto events = storage.query("SELECT sequence_no,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no DESC LIMIT 1", {tenantId});
  if (events.empty()) return true;
  const auto sequence = events.front().at("sequence_no").get<std::int64_t>();
  if (sequence < checkpointInterval || sequence % checkpointInterval != 0) return true;
  return storage.execute("INSERT OR IGNORE INTO evidence_checkpoints(id,tenant_id,sequence_no,event_hash,projection_version,projection_state_json,verified_at,created_at) VALUES(?,?,?,?,?,?,datetime('now'),datetime('now'))", {shared::Uuid::generate().str(), tenantId, std::to_string(sequence), events.front().at("event_hash").get<std::string>(), "v1", "{}"});
}

int MaintenanceJobRunner::cleanupTemporaryUploads(const std::filesystem::path& artifactTempPath, std::int64_t olderThanSeconds) {
  if (artifactTempPath.empty() || olderThanSeconds < 0 || !std::filesystem::exists(artifactTempPath)) return 0;
  const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(olderThanSeconds);
  int removed = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(artifactTempPath, error)) {
    if (error) break;
    if (!entry.is_regular_file(error) || error) continue;
    if (entry.last_write_time(error) < cutoff && std::filesystem::remove(entry.path(), error) && !error) ++removed;
  }
  return removed;
}

MaintenanceSummary MaintenanceJobRunner::run(infrastructure::Storage& storage, const std::string& tenantId,
                                              const std::string& owner, const std::filesystem::path& artifactTempPath) {
  MaintenanceSummary summary;
  if (!JobCoordinator::acquire(storage, tenantId, "maintenance", "default", owner, 60)) return summary;
  const bool committed = storage.transaction([&] {
    summary.expiredApprovals = expireApprovals(storage, tenantId);
    if (summary.expiredApprovals < 0) return false;
    summary.expiredCommands = expireCommands(storage, tenantId);
    if (summary.expiredCommands < 0) return false;
    summary.deletedIdempotencyRecords = cleanupIdempotency(storage, tenantId);
    if (summary.deletedIdempotencyRecords < 0) return false;
    summary.checkpointedTenants = checkpointEvidence(storage, tenantId) ? 1 : 0;
    if (summary.checkpointedTenants == 0 && !storage.query("SELECT id FROM evidence_events WHERE tenant_id=? LIMIT 1", {tenantId}).empty() && !storage.verifyEvidence(tenantId).value("valid", false)) return false;
    summary.recoveredLeases = JobCoordinator::recoverExpired(storage, tenantId);
    if (summary.recoveredLeases < 0 || reclaimExpiredWork(storage, tenantId) < 0) return false;
    return true;
  });
  cleanupTemporaryUploads(artifactTempPath);
  JobCoordinator::release(storage, tenantId, "maintenance", "default", owner);
  if (!committed) return {};
  return summary;
}

int ScheduledReleaseStarter::run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner) {
  if (!JobCoordinator::acquire(storage, tenantId, "scheduled_release_starter", "default", owner, 60)) return 0;
  const auto releases = storage.query("SELECT r.id,r.version,COALESCE(p.two_person_approval,1) AS two_person_approval FROM releases r LEFT JOIN rollout_policies p ON p.tenant_id=r.tenant_id AND p.id=r.policy_id WHERE r.tenant_id=? AND r.status='scheduled' AND r.scheduled_for <= datetime('now') ORDER BY r.scheduled_for,r.id", {tenantId});
  int started = 0;
  for (const auto& release : releases) {
    const auto releaseId = release.at("id").get<std::string>();
    const bool requiresApproval = release.value("two_person_approval", 1) != 0;
    const auto approvals = storage.query("SELECT id FROM approval_requests WHERE tenant_id=? AND release_id=? AND action='start' AND status='approved' AND approved_release_version=? AND consumed_at IS NULL AND expires_at > datetime('now')", {tenantId, releaseId, std::to_string(release.at("version").get<int>())});
    if (requiresApproval && approvals.empty()) continue;
    const bool committed = storage.transaction([&] {
      if (!storage.updateRelease(tenantId, releaseId, "running", release.at("version").get<int>())) return false;
      // Validation freezes the first stage as pending. Set the cursor to zero so the
      // shared activator creates its assignments exactly once for scheduled starts.
      if (!storage.execute("UPDATE releases SET started_at=COALESCE(started_at,datetime('now')),current_stage_ordinal=0,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='running'", {tenantId, releaseId})) return false;
      if (activateNextStage(storage, tenantId, releaseId, 0) < 0) return false;
      if (requiresApproval && !storage.execute("UPDATE approval_requests SET consumed_at=datetime('now') WHERE tenant_id=? AND release_id=? AND action='start' AND status='approved' AND approved_release_version=? AND consumed_at IS NULL", {tenantId, releaseId, std::to_string(release.at("version").get<int>())})) return false;
      return storage.appendEvidence(tenantId, "release.scheduled_started", "release", releaseId, { {"release_version", release.at("version").get<int>() + 1} }, "job", owner).has_value();
    });
    if (committed) ++started;
  }
  JobCoordinator::release(storage, tenantId, "scheduled_release_starter", "default", owner);
  return started;
}

int StageGateEvaluatorJob::run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner) {
  if (!JobCoordinator::acquire(storage, tenantId, "stage_gate_evaluator", "default", owner, 60)) return 0;
  const auto stages = storage.query("SELECT s.id,s.release_id,s.ordinal,s.eligible_count,s.observation_started_at,s.observation_ends_at,r.version FROM release_stages s JOIN releases r ON r.tenant_id=s.tenant_id AND r.id=s.release_id WHERE s.tenant_id=? AND s.status='active' AND r.status='running' ORDER BY s.release_id,s.ordinal", {tenantId});
  int evaluated = 0;
  for (const auto& stage : stages) {
    if (!stage.value("observation_ends_at", "").empty() && storage.query("SELECT 1 AS ready WHERE datetime('now') >= ?", {stage.at("observation_ends_at").get<std::string>()}).empty()) continue;
    const auto assignments = storage.query("SELECT a.state FROM release_assignments a JOIN release_stages s ON s.tenant_id=a.tenant_id AND s.id=a.stage_id WHERE a.tenant_id=? AND a.release_id=? AND s.ordinal<=?", {tenantId, stage.at("release_id").get<std::string>(), std::to_string(stage.at("ordinal").get<int>())});
    domain::GateMetrics metrics;
    metrics.assigned = static_cast<int>(assignments.size());
    for (const auto& assignment : assignments) {
      const auto state = assignment.value("state", "");
      metrics.converged += state == "converged" ? 1 : 0;
      metrics.installFailures += state == "failed" ? 1 : 0;
      metrics.offline += state == "stranded" ? 1 : 0;
    }
    const auto freshReports = storage.query("SELECT COUNT(DISTINCT d.device_id) AS count FROM device_reports d JOIN release_assignments a ON a.tenant_id=d.tenant_id AND a.device_id=d.device_id AND a.release_id=d.release_id JOIN release_stages s ON s.tenant_id=a.tenant_id AND s.id=a.stage_id WHERE d.tenant_id=? AND d.release_id=? AND s.ordinal<=? AND d.server_received_at >= ? AND d.server_received_at <= datetime('now')", {tenantId, stage.at("release_id").get<std::string>(), std::to_string(stage.at("ordinal").get<int>()), stage.value("observation_started_at", "")});
    const auto samples = storage.query("SELECT COUNT(DISTINCT h.device_id) AS count FROM health_samples h JOIN release_stages s ON s.tenant_id=h.tenant_id AND s.id=h.stage_id WHERE h.tenant_id=? AND h.release_id=? AND s.ordinal<=? AND h.freshness_state='fresh' AND h.observed_at >= ? AND h.observed_at <= datetime('now')", {tenantId, stage.at("release_id").get<std::string>(), std::to_string(stage.at("ordinal").get<int>()), stage.value("observation_started_at", "")});
    if (!freshReports.empty()) metrics.fresh = freshReports.front().value("count", 0);
    if (!samples.empty()) metrics.fresh = std::max(metrics.fresh, samples.front().value("count", 0));
    const auto healthFailures = storage.query("SELECT COUNT(DISTINCT h.device_id) AS count FROM health_samples h JOIN release_stages s ON s.tenant_id=h.tenant_id AND s.id=h.stage_id WHERE h.tenant_id=? AND h.release_id=? AND s.ordinal<=? AND h.freshness_state='fresh' AND h.observed_at >= ? AND h.observed_at <= datetime('now') AND h.metric_name='health_failure' AND h.metric_value>0", {tenantId, stage.at("release_id").get<std::string>(), std::to_string(stage.at("ordinal").get<int>()), stage.value("observation_started_at", "")});
    metrics.healthFailures = healthFailures.empty() ? 0 : healthFailures.front().value("count", 0);
    const auto crashFree = storage.query("SELECT AVG(h.metric_value) AS value FROM health_samples h JOIN release_stages s ON s.tenant_id=h.tenant_id AND s.id=h.stage_id WHERE h.tenant_id=? AND h.release_id=? AND s.ordinal<=? AND h.metric_name IN ('crash_free_percent','crash_free_rate') AND h.freshness_state='fresh' AND h.observed_at >= ? AND h.observed_at <= datetime('now')", {tenantId, stage.at("release_id").get<std::string>(), std::to_string(stage.at("ordinal").get<int>()), stage.value("observation_started_at", "")});
    if (!crashFree.empty() && !crashFree.front().at("value").is_null()) {
      metrics.crashFreePercent = crashFree.front().at("value").get<double>();
      if (metrics.crashFreePercent <= 1.0) metrics.crashFreePercent *= 100.0;
    }
    const auto release = storage.query("SELECT frozen_policy_json,version FROM releases WHERE tenant_id=? AND id=? AND status='running'", {tenantId, stage.at("release_id").get<std::string>()});
    domain::GateThresholds thresholds;
    bool requiredIotReady = true;
    if (!release.empty() && !release.front().value("frozen_policy_json", "").empty()) {
      try {
        const auto frozenPolicy = shared::Json::parse(release.front().at("frozen_policy_json").get<std::string>());
        thresholds = domain::GateEvaluator::thresholdsFromPolicy(frozenPolicy);
        const auto requireIotValue = frozenPolicy.contains("require_iot_evidence") ? frozenPolicy.at("require_iot_evidence") : shared::Json(false);
        const bool requireIotEvidence = requireIotValue.is_boolean() ? requireIotValue.get<bool>() : requireIotValue.is_number_integer() && requireIotValue.get<int>() != 0;
        if (requireIotEvidence) {
          const auto configured = storage.query("SELECT id FROM integration_configs WHERE tenant_id=? AND adapter_type='iot_rest_v1' AND enabled=1 AND required_for_promotion=1 AND health_status='healthy'", {tenantId});
          const auto freshSamples = storage.query("SELECT COUNT(DISTINCT h.device_id) AS count FROM health_samples h JOIN release_stages s ON s.tenant_id=h.tenant_id AND s.id=h.stage_id WHERE h.tenant_id=? AND h.release_id=? AND s.ordinal<=? AND h.source='iot_rest_v1' AND h.freshness_state='fresh' AND h.observed_at >= COALESCE(?,h.observed_at) AND h.observed_at <= datetime('now')", {tenantId, stage.at("release_id").get<std::string>(), std::to_string(stage.at("ordinal").get<int>()), stage.value("observation_started_at", "")});
          requiredIotReady = !configured.empty() && !freshSamples.empty() && freshSamples.front().value("count", 0) >= metrics.assigned;
          if (!requiredIotReady) metrics.fresh = 0;
        }
      } catch (const std::exception&) {
        requiredIotReady = false;
        metrics.fresh = 0;
      }
    }
    const auto decision = domain::GateEvaluator::evaluate(metrics, thresholds);
    const auto metricsJson = shared::CanonicalJson::serialize({{"assigned", metrics.assigned}, {"fresh", metrics.fresh}, {"converged", metrics.converged}, {"install_failures", metrics.installFailures}, {"health_failures", metrics.healthFailures}, {"rollback_failures", metrics.rollbackFailures}, {"crash_free_percent", metrics.crashFreePercent}, {"offline", metrics.offline}});
    auto failed = shared::Json::array();
    for (const auto& gate : domain::GateEvaluator::failedGates(metrics, thresholds)) failed.push_back(gate);
    if (!requiredIotReady) failed.push_back("required_iot_evidence");
    const auto evidenceDigest = shared::DigestService::sha256Hex(metricsJson + domain::toString(decision));
    const auto evaluationId = shared::Uuid::generate().str();
    const bool committed = storage.transaction([&] {
      const auto releaseId = stage.at("release_id").get<std::string>();
      const auto releaseVersion = stage.at("version").get<int>();
      const auto releaseRow = storage.query("SELECT status FROM releases WHERE tenant_id=? AND id=? AND version=?", {tenantId, releaseId, std::to_string(releaseVersion)});
      if (releaseRow.empty()) return false;
      const auto next = storage.query("SELECT id FROM release_stages WHERE tenant_id=? AND release_id=? AND ordinal=? AND status='pending'", {tenantId, releaseId, std::to_string(stage.at("ordinal").get<int>() + 1)});
      if (!legalGateTransition(releaseRow.front().value("status", ""), decision, !next.empty())) return false;
      if (!storage.execute("INSERT INTO health_gate_evaluations(id,tenant_id,release_id,stage_id,decision,sample_window_start,sample_window_end,sample_count,eligible_device_count,fresh_device_count,metrics_json,failed_gates_json,evidence_digest,evaluated_at) VALUES(?,?,?,?,?,datetime('now','-1 hour'),datetime('now'),?,?,?,?,?,?,datetime('now'))", {evaluationId, tenantId, stage.at("release_id").get<std::string>(), stage.at("id").get<std::string>(), domain::toString(decision), std::to_string(metrics.assigned), std::to_string(stage.at("eligible_count").get<int>()), std::to_string(metrics.fresh), metricsJson, failed.dump(), evidenceDigest})) return false;
      if (!storage.execute("UPDATE release_stages SET gate_decision_json=?,status=CASE WHEN ?='pass' THEN 'passed' WHEN ? IN ('rollback','abort') THEN 'failed' ELSE status END,ended_at=CASE WHEN ? IN ('pass','rollback','abort') THEN datetime('now') ELSE ended_at END,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='active'", {shared::CanonicalJson::serialize({{"evaluation_id", evaluationId}, {"decision", domain::toString(decision)}, {"metrics", shared::Json::parse(metricsJson)}}), domain::toString(decision), domain::toString(decision), domain::toString(decision), tenantId, stage.at("id").get<std::string>()})) return false;
      if (decision == domain::GateDecision::pass) {
        if (next.empty()) {
          if (!storage.updateRelease(tenantId, releaseId, "completed", releaseVersion)) return false;
          if (!storage.execute("UPDATE releases SET ended_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='completed'", {tenantId, releaseId})) return false;
        } else if (activateNextStage(storage, tenantId, releaseId, stage.at("ordinal").get<int>()) < 0) return false;
      } else if (decision == domain::GateDecision::rollback) {
        if (!issueRollbackCommands(storage, tenantId, releaseId) || !storage.updateRelease(tenantId, releaseId, "rolling_back", releaseVersion)) return false;
      } else if (decision == domain::GateDecision::abort) {
        if (!storage.updateRelease(tenantId, releaseId, "aborting", releaseVersion)) return false;
      } else if (decision == domain::GateDecision::pause) {
        if (!storage.updateRelease(tenantId, releaseId, "paused", releaseVersion)) return false;
      }
      if (!settleTerminalRelease(storage, tenantId, releaseId, owner)) return false;
      return storage.appendEvidence(tenantId, "release.gate.evaluated", "release", releaseId, {{"stage_id", stage.at("id")}, {"evaluation_id", evaluationId}, {"decision", domain::toString(decision)}, {"evidence_digest", evidenceDigest}}, "job", owner).has_value();
    });
    if (committed) ++evaluated;
  }
  JobCoordinator::release(storage, tenantId, "stage_gate_evaluator", "default", owner);
  return evaluated;
}

int ApprovalExpiryScanner::run(infrastructure::Storage& storage, const std::string& tenantId) {
  return MaintenanceJobRunner::expireApprovals(storage, tenantId);
}

int CommandExpiryScanner::run(infrastructure::Storage& storage, const std::string& tenantId) { return MaintenanceJobRunner::expireCommands(storage, tenantId); }

int DeviceFreshnessProjector::run(infrastructure::Storage& storage, const std::string& tenantId, int freshnessSeconds) {
  if (freshnessSeconds < 1) return 0;
  const auto rows = storage.query("SELECT id FROM health_samples WHERE tenant_id=? AND freshness_state='fresh' AND observed_at < datetime('now','-" + std::to_string(freshnessSeconds) + " seconds')", {tenantId});
  if (!storage.execute("UPDATE health_samples SET freshness_state='stale' WHERE tenant_id=? AND freshness_state='fresh' AND observed_at < datetime('now','-" + std::to_string(freshnessSeconds) + " seconds')", {tenantId})) return -1;
  return static_cast<int>(rows.size());
}

int IotHealthPoller::run(infrastructure::Storage& storage, const std::string& tenantId) {
  const auto configs = storage.query("SELECT adapter_type,enabled,endpoint_base_url,secret_ref,settings_json,poll_cursor_json FROM integration_configs WHERE tenant_id=? AND adapter_type='iot_rest_v1' AND enabled=1", {tenantId});
  if (configs.empty()) return 0;
  int inserted = 0;
  int stale = 0;
  try {
    const auto settings = shared::Json::parse(configs.front().value("settings_json", "{}"));
    shared::Json readingSettings = settings;
    shared::HttpClientPool clientPool;
    if (!settings.value("fixture_mode", true)) {
      const auto secret = shared::SecretResolver::environment(configs.front().value("secret_ref", ""));
      shared::Json cursor = shared::Json::object();
      try { cursor = shared::Json::parse(configs.front().value("poll_cursor_json", "{}")); } catch (const std::exception&) {}
      const auto external = infrastructure::AdapterContract::liveIotReadings(configs.front().value("endpoint_base_url", ""), secret.value_or(""), cursor.value("upstream_cursor", ""), clientPool);
      if (!external.ok()) {
        storage.execute("UPDATE integration_configs SET health_status='unhealthy',last_error_code=?,last_polled_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND adapter_type='iot_rest_v1'", {external.error->code, tenantId});
        return 0;
      }
      readingSettings = *external.value;
      readingSettings["freshness_seconds"] = settings.value("freshness_seconds", 120);
    }
    const auto readings = infrastructure::IotFixtureParser::parse(readingSettings);
    if (!readings.ok()) {
      storage.execute("UPDATE integration_configs SET health_status='unhealthy',last_error_code=?,last_polled_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND adapter_type='iot_rest_v1'", {readings.error->code, tenantId});
      return 0;
    }
    const bool committed = storage.transaction([&] {
      for (const auto& reading : *readings.value) {
        const auto assignment = storage.query("SELECT release_id,stage_id FROM release_assignments WHERE tenant_id=? AND device_id=? AND state IN ('pending','commanded','acknowledged') ORDER BY desired_generation DESC LIMIT 1", {tenantId, reading.deviceId});
        if (assignment.empty()) continue;
        const auto duplicate = storage.query("SELECT id FROM health_samples WHERE tenant_id=? AND source='iot_rest_v1' AND source_event_id=? AND metric_name=? LIMIT 1", {tenantId, reading.sourceEventId, reading.metricName});
        if (!duplicate.empty()) continue;
        if (!reading.fresh) ++stale;
        if (!storage.execute("INSERT INTO health_samples(id,tenant_id,release_id,stage_id,device_id,source,source_event_id,metric_name,metric_value,unit,observed_at,received_at,freshness_state,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,datetime('now'),?,datetime('now'))", {shared::Uuid::generate().str(), tenantId, assignment.front().at("release_id").get<std::string>(), assignment.front().at("stage_id").get<std::string>(), reading.deviceId, "iot_rest_v1", reading.sourceEventId, reading.metricName, std::to_string(reading.value), reading.unit, reading.observedAt, reading.fresh ? "fresh" : "stale"})) return false;
        ++inserted;
      }
      const auto cursor = shared::CanonicalJson::serialize({{"last_poll_at", shared::TenantClock::nowIso8601()}, {"accepted_samples", inserted}});
      return storage.execute("UPDATE integration_configs SET poll_cursor_json=?,last_polled_at=datetime('now'),last_success_at=datetime('now'),health_status=?,last_error_code=NULL,updated_at=datetime('now') WHERE tenant_id=? AND adapter_type='iot_rest_v1'", {cursor, stale > 0 ? "degraded" : "healthy", tenantId});
    });
    if (!committed) return 0;
  } catch (const std::exception&) {
    storage.execute("UPDATE integration_configs SET health_status='unhealthy',last_error_code='IOT_FIXTURE_INVALID',last_polled_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND adapter_type='iot_rest_v1'", {tenantId});
    return 0;
  }
  return inserted;
}

std::pair<int, int> OutboxPublisher::run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner) {
  if (!JobCoordinator::acquire(storage, tenantId, "outbox_publisher", "default", owner, 60)) return {0, 0};
  const auto rows = storage.query("SELECT o.id,o.adapter_type,o.attempt_count,o.payload_json,c.enabled,c.endpoint_base_url,c.secret_ref,c.settings_json FROM outbox_deliveries o LEFT JOIN integration_configs c ON c.tenant_id=o.tenant_id AND c.adapter_type=o.adapter_type WHERE o.tenant_id=? AND o.status='pending' AND o.next_attempt_at <= datetime('now') AND (o.lease_expires_at IS NULL OR o.lease_expires_at <= datetime('now') OR o.lease_owner=?) ORDER BY o.created_at LIMIT 100", {tenantId, owner});
  shared::HttpClientPool clientPool;
  int published = 0;
  int deadLettered = 0;
  for (const auto& row : rows) {
    if (!storage.execute("UPDATE outbox_deliveries SET lease_owner=?,lease_expires_at=datetime('now','+60 seconds'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='pending' AND (lease_expires_at IS NULL OR lease_expires_at <= datetime('now') OR lease_owner=?)", {owner, tenantId, row.at("id").get<std::string>(), owner})) continue;
    const bool enabled = row.value("enabled", 0) != 0;
    shared::Json settings = shared::Json::object();
    try { settings = shared::Json::parse(row.value("settings_json", "{}")); } catch (const std::exception&) {}
    const auto attemptCount = row.value("attempt_count", 0);
    infrastructure::DeliveryResult delivery{infrastructure::DeliveryDisposition::permanent_failure, 0, "ADAPTER_DISABLED", {}, {}};
    if (enabled) {
      if (settings.value("fixture_mode", true)) delivery = infrastructure::AdapterContract::fixtureDelivery(row.at("adapter_type").get<std::string>(), settings, row.at("id").get<std::string>(), attemptCount);
      else {
        const auto secret = shared::SecretResolver::environment(row.value("secret_ref", ""));
        shared::Json payload = shared::Json::object();
        bool payloadValid = true;
        try { payload = shared::Json::parse(row.value("payload_json", "{}")); } catch (const std::exception&) { payloadValid = false; delivery = {infrastructure::DeliveryDisposition::permanent_failure, 0, "OUTBOX_PAYLOAD_INVALID", {}, {}}; }
        if (payloadValid) delivery = infrastructure::AdapterContract::liveDelivery(row.at("adapter_type").get<std::string>(), row.value("endpoint_base_url", ""), secret.value_or(""), payload, row.at("id").get<std::string>(), settings, clientPool);
      }
    }
    if (delivery.disposition == infrastructure::DeliveryDisposition::published) {
      if (storage.execute("UPDATE outbox_deliveries SET status='published',attempt_count=attempt_count+1,last_status_code=?,external_reference=NULLIF(?,'') ,external_status=NULLIF(?,'') ,published_at=datetime('now'),lease_owner=NULL,lease_expires_at=NULL,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='pending' AND lease_owner=?", {std::to_string(delivery.statusCode), delivery.externalReference, delivery.externalStatus, tenantId, row.at("id").get<std::string>(), owner})) ++published;
    } else if (delivery.disposition == infrastructure::DeliveryDisposition::retryable_failure) {
      const bool changed = storage.execute("UPDATE outbox_deliveries SET status=CASE WHEN attempt_count >= 4 THEN 'dead_letter' ELSE 'pending' END,attempt_count=attempt_count+1,last_status_code=?,last_error_code=CASE WHEN attempt_count >= 4 THEN ? ELSE ? END,next_attempt_at=datetime('now','+5 minutes'),lease_owner=NULL,lease_expires_at=NULL,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='pending' AND lease_owner=?", {std::to_string(delivery.statusCode), delivery.errorCode.empty() ? "RETRY_EXHAUSTED" : delivery.errorCode, delivery.errorCode.empty() ? "RETRYABLE_ADAPTER_FAILURE" : delivery.errorCode, tenantId, row.at("id").get<std::string>(), owner});
      if (changed && attemptCount >= 4) ++deadLettered;
    } else {
      if (storage.execute("UPDATE outbox_deliveries SET status='dead_letter',attempt_count=attempt_count+1,last_status_code=?,last_error_code=?,lease_owner=NULL,lease_expires_at=NULL,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='pending' AND lease_owner=?", {std::to_string(delivery.statusCode), delivery.errorCode.empty() ? "ADAPTER_REJECTED" : delivery.errorCode, tenantId, row.at("id").get<std::string>(), owner})) ++deadLettered;
    }
  }
  JobCoordinator::release(storage, tenantId, "outbox_publisher", "default", owner);
  return {published, deadLettered};
}

int WorkflowExecutionObserver::run(infrastructure::Storage& storage, const std::string& tenantId) {
  const auto rows = storage.query("SELECT o.id,o.evidence_event_id,o.external_reference,c.endpoint_base_url,c.secret_ref,c.settings_json FROM outbox_deliveries o LEFT JOIN integration_configs c ON c.tenant_id=o.tenant_id AND c.adapter_type=o.adapter_type WHERE o.tenant_id=? AND o.adapter_type='workflow_manual_v1' AND o.status='published' AND (o.external_status IS NULL OR o.external_status IN ('accepted','queued','running','in_progress'))", {tenantId});
  shared::HttpClientPool clientPool;
  int observed = 0;
  for (const auto& row : rows) {
    shared::Json settings = shared::Json::object();
    try { settings = shared::Json::parse(row.value("settings_json", "{}")); } catch (const std::exception&) { settings = shared::Json::object(); }
    std::string status = settings.value("fixture_observed_status", settings.value("fixture_execution_status", "running"));
    if (!settings.value("fixture_mode", true)) {
      const auto secret = shared::SecretResolver::environment(row.value("secret_ref", ""));
      const auto workflowStatus = infrastructure::AdapterContract::liveWorkflowStatus(row.value("endpoint_base_url", ""), secret.value_or(""), row.value("external_reference", ""), clientPool);
      if (!workflowStatus.ok()) {
        storage.execute("UPDATE integration_configs SET health_status='unhealthy',last_error_code=?,updated_at=datetime('now') WHERE tenant_id=? AND adapter_type='workflow_manual_v1'", {workflowStatus.error->code, tenantId});
        continue;
      }
      status = *workflowStatus.value;
    }
    const bool terminal = status == "completed" || status == "failed" || status == "cancelled";
    if (!terminal) continue;
    const auto updated = storage.transaction([&] {
      if (!storage.execute("UPDATE outbox_deliveries SET external_status=?,external_last_checked_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='published' AND (external_status IS NULL OR external_status IN ('accepted','queued','running','in_progress'))", {status, tenantId, row.at("id").get<std::string>()})) return false;
      const auto event = storage.appendEvidence(tenantId, status == "completed" ? "integration.workflow_execution_completed" : "integration.workflow_execution_failed", "outbox", row.at("id").get<std::string>(), {{"execution_id", row.value("external_reference", "")}, {"status", status}}, "job", "workflow_execution_observer");
      if (!event.has_value()) return false;
      if (status != "completed" && storage.query("SELECT id FROM operator_notices WHERE tenant_id=? AND event_id=?", {tenantId, event->at("id").get<std::string>()}).empty()) {
        if (!storage.execute("INSERT INTO operator_notices(id,tenant_id,event_id,severity,title,body,created_at) VALUES(?,?,?,?,?,?,datetime('now'))", {shared::Uuid::generate().str(), tenantId, event->at("id").get<std::string>(), "high", "Workflow execution failed", shared::CanonicalJson::serialize({{"execution_id", row.value("external_reference", "")}, {"status", status}})})) return false;
      }
      return true;
    });
    if (updated) ++observed;
  }
  return observed;
}

int SimulationJobRunner::run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& traceStorePath, const std::string& owner) {
  if (!JobCoordinator::acquire(storage, tenantId, "simulation_worker", "default", owner, 300)) return 0;
  reclaimExpiredWork(storage, tenantId);
  const auto rows = storage.query("SELECT id,input_json,seed FROM simulation_runs WHERE tenant_id=? AND status='queued' ORDER BY created_at LIMIT 1", {tenantId});
  int completed = 0;
  for (const auto& row : rows) {
    const auto id = row.at("id").get<std::string>();
    if (!storage.execute("UPDATE simulation_runs SET status='running',attempt_count=attempt_count+1,lease_owner=?,lease_expires_at=datetime('now','+5 minutes'),started_at=datetime('now') WHERE tenant_id=? AND id=? AND status='queued'", {owner, tenantId, id})) continue;
    try {
      const auto result = domain::Simulator::run(shared::Json::parse(row.at("input_json").get<std::string>()), row.at("seed").get<std::uint64_t>(), [&storage, &tenantId, &id] {
        const auto state = storage.query("SELECT status FROM simulation_runs WHERE tenant_id=? AND id=?", {tenantId, id});
        return !state.empty() && state.front().value("status", "") == "cancelled";
      });
      if (!result.ok()) {
        storage.execute("UPDATE simulation_runs SET status=?,failure_message=?,completed_at=datetime('now'),lease_owner=NULL,lease_expires_at=NULL WHERE tenant_id=? AND id=? AND status='running'", {result.error->code == "SIMULATION_CANCELLED" ? "cancelled" : "failed", result.error->code, tenantId, id});
        continue;
      }
      const auto path = traceStorePath / tenantId / (id + ".json");
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      if (error) { storage.execute("UPDATE simulation_runs SET status='failed',failure_message='TRACE_STORE_UNAVAILABLE',completed_at=datetime('now'),lease_owner=NULL WHERE tenant_id=? AND id=?", {tenantId, id}); continue; }
      std::ofstream output(path, std::ios::binary);
      output << shared::CanonicalJson::serialize(result.value->trace);
      output.close();
      if (!output) { storage.execute("UPDATE simulation_runs SET status='failed',failure_message='TRACE_WRITE_FAILED',completed_at=datetime('now'),lease_owner=NULL WHERE tenant_id=? AND id=?", {tenantId, id}); continue; }
      const auto saved = storage.execute("UPDATE simulation_runs SET status='completed',result_json=?,result_digest=?,trace_storage_key=?,trace_digest=?,completed_at=datetime('now'),lease_owner=NULL,lease_expires_at=NULL WHERE tenant_id=? AND id=? AND status='running'", {result.value->metrics.dump(), result.value->resultDigest, path.string(), result.value->traceDigest, tenantId, id});
      if (saved) ++completed;
    } catch (const std::exception& error) { storage.execute("UPDATE simulation_runs SET status='failed',failure_message=?,completed_at=datetime('now'),lease_owner=NULL WHERE tenant_id=? AND id=?", {std::string("INVALID_FROZEN_INPUT:") + error.what(), tenantId, id}); }
  }
  JobCoordinator::release(storage, tenantId, "simulation_worker", "default", owner);
  return completed;
}

int ReplayJobRunner::run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner) {
  if (!JobCoordinator::acquire(storage, tenantId, "replay_worker", "default", owner, 300)) return 0;
  reclaimExpiredWork(storage, tenantId);
  const auto rows = storage.query("SELECT id,simulation_run_id,release_id,source_kind,source_event_from,source_event_to,source_snapshot_json,expected_decision_digest FROM replay_runs WHERE tenant_id=? AND status='queued' ORDER BY created_at LIMIT 10", {tenantId});
  int completed = 0;
  for (const auto& row : rows) {
    const auto id = row.at("id").get<std::string>();
    if (!storage.execute("UPDATE replay_runs SET status='running',attempt_count=attempt_count+1,lease_owner=?,lease_expires_at=datetime('now','+5 minutes'),started_at=datetime('now') WHERE tenant_id=? AND id=? AND status='queued'", {owner, tenantId, id})) continue;
    shared::Result<domain::ReplayResult> replay;
    try {
      if (row.value("source_kind", "") == "simulation") {
        const auto source = storage.query("SELECT input_json,input_digest,seed,result_digest FROM simulation_runs WHERE tenant_id=? AND id=? AND status='completed'", {tenantId, row.value("simulation_run_id", "")});
        if (source.empty()) {
          storage.execute("UPDATE replay_runs SET status='failed',divergence_json=?,completed_at=datetime('now'),lease_owner=NULL,lease_expires_at=NULL WHERE tenant_id=? AND id=? AND status='running'", {shared::Json{{"code", "SOURCE_MISSING"}}.dump(), tenantId, id});
          continue;
        }
        if (shared::DigestService::sha256Hex(source.front().at("input_json").get<std::string>()) != source.front().value("input_digest", "")) throw std::runtime_error("SOURCE_DIGEST_MISMATCH");
        replay = domain::ReplayEngine::simulation(shared::Json::parse(source.front().at("input_json").get<std::string>()), source.front().at("seed").get<std::uint64_t>(), source.front().value("result_digest", ""));
      } else {
        if (!storage.verifyEvidence(tenantId).value("valid", false)) throw std::runtime_error("EVIDENCE_CHAIN_BROKEN");
        const auto events = storage.query("SELECT id,sequence_no,tenant_id,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash FROM evidence_events WHERE tenant_id=? AND sequence_no BETWEEN ? AND ? ORDER BY sequence_no", {tenantId, std::to_string(row.value("source_event_from", 0LL)), std::to_string(row.value("source_event_to", 0LL))});
        const auto from = row.value("source_event_from", 0LL);
        const auto to = row.value("source_event_to", 0LL);
        if (events.empty() || from < 1 || to < from || events.front().value("sequence_no", 0LL) != from || events.back().value("sequence_no", 0LL) != to || static_cast<long long>(events.size()) != to - from + 1) throw std::runtime_error("INVALID_REPLAY_RANGE");
        shared::Json normalized = shared::Json::array();
        for (auto event : events) { event["payload"] = shared::Json::parse(event.at("payload_json").get<std::string>()); event.erase("payload_json"); normalized.push_back(std::move(event)); }
        shared::Json expectedEvents;
        if (!row.value("source_snapshot_json", "").empty()) expectedEvents = shared::Json::parse(row.at("source_snapshot_json").get<std::string>());
        replay = expectedEvents.is_array() ? domain::ReplayEngine::evidence(normalized, expectedEvents, row.value("expected_decision_digest", "")) : domain::ReplayEngine::evidence(normalized, row.value("expected_decision_digest", ""));
      }
      if (replay.ok()) {
         storage.execute("UPDATE replay_runs SET status=?,actual_decision_digest=?,divergence_json=?,completed_at=datetime('now'),lease_owner=NULL,lease_expires_at=NULL WHERE tenant_id=? AND id=? AND status='running'", {replay.value->status, replay.value->actualDigest, replay.value->divergence.dump(), tenantId, id});
        ++completed;
       } else storage.execute("UPDATE replay_runs SET status='failed',divergence_json=?,completed_at=datetime('now'),lease_owner=NULL,lease_expires_at=NULL WHERE tenant_id=? AND id=?", {shared::Json{{"code", replay.error->code}}.dump(), tenantId, id});
     } catch (const std::exception& error) { storage.execute("UPDATE replay_runs SET status='failed',divergence_json=?,completed_at=datetime('now'),lease_owner=NULL,lease_expires_at=NULL WHERE tenant_id=? AND id=?", {shared::Json{{"code", error.what()}}.dump(), tenantId, id}); }
  }
  JobCoordinator::release(storage, tenantId, "replay_worker", "default", owner);
  return completed;
}

int BenchmarkJobRunner::run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& exportStorePath, const std::string& owner) {
  if (!JobCoordinator::acquire(storage, tenantId, "benchmark_worker", "default", owner, 900)) return 0;
  reclaimExpiredWork(storage, tenantId);
  const auto rows = storage.query("SELECT id,corpus_version,corpus_manifest_json FROM benchmark_runs WHERE tenant_id=? AND status='queued' ORDER BY created_at LIMIT 1", {tenantId});
  int completed = 0;
  for (const auto& row : rows) {
    const auto id = row.at("id").get<std::string>();
    if (!storage.execute("UPDATE benchmark_runs SET status='running',attempt_count=attempt_count+1,lease_owner=?,lease_expires_at=datetime('now','+15 minutes'),started_at=datetime('now') WHERE tenant_id=? AND id=? AND status='queued'", {owner, tenantId, id})) continue;
    shared::Result<domain::BenchmarkReport> report;
    try {
      const auto manifest = shared::Json::parse(row.value("corpus_manifest_json", "{}"));
      report = manifest.contains("scenarios") ? domain::BenchmarkRunner::runManifest(manifest) : domain::BenchmarkRunner::run(row.value("corpus_version", "v1"));
    } catch (const std::exception&) {
      report = shared::Result<domain::BenchmarkReport>::failure({"INVALID_BENCHMARK_MANIFEST", "The benchmark manifest is not valid JSON.", 422});
    }
    if (!report.ok()) { storage.execute("UPDATE benchmark_runs SET status='failed',failure_message=?,lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {report.error->code, tenantId, id}); continue; }
    const auto directory = exportStorePath / "benchmarks" / id;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { storage.execute("UPDATE benchmark_runs SET status='failed',failure_message='REPORT_STORE_UNAVAILABLE',lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {tenantId, id}); continue; }
    shared::Json cells = shared::Json::array();
    for (const auto& cell : report.value->cells) cells.push_back({{"scenario", cell.scenario}, {"seed", cell.seed}, {"strategy", cell.strategy}, {"metrics", cell.metrics}, {"digest", cell.digest}});
    const auto jsonReport = shared::CanonicalJson::serialize({{"corpus_version", report.value->corpusVersion}, {"result_digest", report.value->digest}, {"expected_case_count", 108}, {"cells", cells}}) + "\n";
    std::ostringstream markdown;
    markdown << "# Edge Fleet benchmark " << report.value->corpusVersion << "\n\n| Scenario | Seed | Strategy | Failure rate | Exposure fraction | Rollback | Healthy convergence | Stranded devices | False rollback |\n|---|---:|---|---:|---:|---|---:|---:|---:|\n";
    double maxExposure = 0.0;
    int maxStranded = 0;
    int falseRollbacks = 0;
    for (const auto& cell : report.value->cells) {
      const auto exposure = cell.metrics.value("exposure_fraction", 0.0);
      const auto stranded = cell.metrics.value("stranded_devices", 0);
      const auto falseRollback = cell.metrics.value("false_rollback", 0);
      maxExposure = std::max(maxExposure, exposure);
      maxStranded = std::max(maxStranded, stranded);
      falseRollbacks += falseRollback;
      markdown << "| " << cell.scenario << " | " << cell.seed << " | " << cell.strategy << " | " << cell.metrics.value("failure_rate", 0.0) << " | " << exposure << " | " << (cell.metrics.value("rollback_triggered", false) ? "yes" : "no") << " | " << cell.metrics.value("converged_devices", 0) << " | " << stranded << " | " << falseRollback << " |\n";
    }
    const auto markdownReport = markdown.str();
    {
      std::ofstream output(directory / "report.json", std::ios::binary);
      output << jsonReport;
      output.close();
    }
    {
      std::ofstream output(directory / "report.md", std::ios::binary);
      output << markdownReport;
      output.close();
    }
    const auto duckdbPath = directory / "report.duckdb";
    if (readFile(directory / "report.json") != jsonReport || readFile(directory / "report.md") != markdownReport || !writeDuckDbReport(duckdbPath, *report.value)) {
      storage.execute("UPDATE benchmark_runs SET status='failed',failure_message='DUCKDB_REPORT_FAILED',lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {tenantId, id});
      continue;
    }
    const auto manifest = shared::CanonicalJson::serialize({{"format", "edgefleet-benchmark-bundle-v1"}, {"json_sha256", shared::DigestService::sha256Hex(jsonReport)}, {"markdown_sha256", shared::DigestService::sha256Hex(markdownReport)}, {"duckdb_sha256", shared::DigestService::sha256Hex(readFile(duckdbPath))}, {"cell_count", report.value->cells.size()}});
    const auto bundleDigest = shared::DigestService::sha256Hex(manifest);
    const auto aggregate = shared::CanonicalJson::serialize({{"cell_count", report.value->cells.size()}, {"max_exposure_fraction", maxExposure}, {"max_stranded_devices", maxStranded}, {"false_rollback_count", falseRollbacks}});
    const bool saved = storage.transaction([&] {
      for (const auto& cell : report.value->cells) if (!storage.execute("INSERT INTO benchmark_results(id,tenant_id,benchmark_run_id,corpus_version,scenario_name,seed,strategy,metrics_json,passed,result_digest,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,datetime('now'))", {shared::Uuid::generate().str(), tenantId, id, report.value->corpusVersion, cell.scenario, std::to_string(cell.seed), cell.strategy, cell.metrics.dump(), "1", cell.digest})) return false;
      const auto resultCount = storage.query("SELECT COUNT(*) AS count FROM benchmark_results WHERE tenant_id=? AND benchmark_run_id=?", {tenantId, id});
      if (resultCount.empty() || resultCount.front().value("count", 0) != 108) return false;
      return storage.execute("UPDATE benchmark_runs SET status='completed',completed_case_count=?,aggregate_metrics_json=?,result_digest=?,duckdb_storage_key=?,json_report_storage_key=?,markdown_report_storage_key=?,report_bundle_sha256=?,lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=? AND status='running'", {std::to_string(report.value->cells.size()), aggregate, report.value->digest, duckdbPath.string(), (directory / "report.json").string(), (directory / "report.md").string(), bundleDigest, tenantId, id});
    });
    if (!saved) continue;
    ++completed;
  }
  JobCoordinator::release(storage, tenantId, "benchmark_worker", "default", owner);
  return completed;
}

int EvidenceExportJobRunner::run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& exportStorePath, const std::string& owner) {
  if (!JobCoordinator::acquire(storage, tenantId, "evidence_export_worker", "default", owner, 300)) return 0;
  reclaimExpiredWork(storage, tenantId);
  const auto rows = storage.query("SELECT id,source_event_from,source_event_to,source_chain_head_hash,tenant_snapshot_json FROM evidence_exports WHERE tenant_id=? AND status='queued' ORDER BY created_at LIMIT 10", {tenantId});
  int completed = 0;
  for (const auto& row : rows) {
    const auto id = row.at("id").get<std::string>();
    if (!storage.execute("UPDATE evidence_exports SET status='running',attempt_count=attempt_count+1,lease_owner=?,lease_expires_at=datetime('now','+5 minutes'),started_at=datetime('now') WHERE tenant_id=? AND id=? AND status='queued'", {owner, tenantId, id})) continue;
    const auto rangeHead = storage.query("SELECT event_hash FROM evidence_events WHERE tenant_id=? AND sequence_no=?", {tenantId, std::to_string(row.at("source_event_to").get<long long>())});
    if (rangeHead.empty() || rangeHead.front().at("event_hash") != row.at("source_chain_head_hash")) { storage.execute("UPDATE evidence_exports SET status='failed',failure_message='CHAIN_HEAD_CHANGED',lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {tenantId, id}); continue; }
    const auto events = storage.query("SELECT id,sequence_no,tenant_id,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash FROM evidence_events WHERE tenant_id=? AND sequence_no BETWEEN ? AND ? ORDER BY sequence_no", {tenantId, std::to_string(row.at("source_event_from").get<long long>()), std::to_string(row.at("source_event_to").get<long long>())});
    const auto from = row.at("source_event_from").get<long long>();
    const auto to = row.at("source_event_to").get<long long>();
    if (events.empty() || from < 1 || to < from || events.front().value("sequence_no", 0LL) != from || events.back().value("sequence_no", 0LL) != to || static_cast<long long>(events.size()) != to - from + 1) { storage.execute("UPDATE evidence_exports SET status='failed',failure_message='INVALID_EXPORT_RANGE',lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {tenantId, id}); continue; }
    if (storage.verifyEvidence(tenantId).value("valid", false)) {
      try {
        const auto exportData = EvidenceExporter::build(shared::Json::parse(row.at("tenant_snapshot_json").get<std::string>()), events);
        const auto path = exportStorePath / (id + ".ndjson");
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::binary); output << exportData.ndjson; output.close();
        if (!error && output) {
           storage.execute("UPDATE evidence_exports SET status='completed',chain_manifest_json=?,output_storage_key=?,output_sha256=?,lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=? AND status='running'", {exportData.manifest, path.string(), exportData.sha256, tenantId, id});
           ++completed;
        } else storage.execute("UPDATE evidence_exports SET status='failed',failure_message='EXPORT_WRITE_FAILED',lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {tenantId, id});
      } catch (const std::exception&) { storage.execute("UPDATE evidence_exports SET status='failed',failure_message='SNAPSHOT_INVALID',lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {tenantId, id}); }
    } else storage.execute("UPDATE evidence_exports SET status='failed',failure_message='EVIDENCE_CHAIN_BROKEN',lease_owner=NULL,lease_expires_at=NULL,completed_at=datetime('now') WHERE tenant_id=? AND id=?", {tenantId, id});
  }
  JobCoordinator::release(storage, tenantId, "evidence_export_worker", "default", owner);
  return completed;
}

WorkerSummary WorkerCoordinator::run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& traceStorePath,
                                     const std::filesystem::path& exportStorePath, const std::filesystem::path& artifactTempPath, const std::string& owner) {
  WorkerSummary summary;
  const auto maintenance = MaintenanceJobRunner::run(storage, tenantId, owner, artifactTempPath);
  summary.expiredApprovals = ApprovalExpiryScanner::run(storage, tenantId) + maintenance.expiredApprovals;
  summary.expiredCommands = CommandExpiryScanner::run(storage, tenantId) + maintenance.expiredCommands;
  summary.freshnessUpdates = DeviceFreshnessProjector::run(storage, tenantId);
  summary.scheduledReleases = ScheduledReleaseStarter::run(storage, tenantId, owner);
  summary.gateEvaluations = StageGateEvaluatorJob::run(storage, tenantId, owner);
  summary.iotSamples = IotHealthPoller::run(storage, tenantId);
  const auto outbox = OutboxPublisher::run(storage, tenantId, owner);
  summary.outboxPublished = outbox.first;
  summary.outboxDeadLettered = outbox.second;
  WorkflowExecutionObserver::run(storage, tenantId);
  summary.simulationsCompleted = SimulationJobRunner::run(storage, tenantId, traceStorePath, owner);
  summary.replaysCompleted = ReplayJobRunner::run(storage, tenantId, owner);
  summary.benchmarksCompleted = BenchmarkJobRunner::run(storage, tenantId, exportStorePath, owner);
  summary.exportsCompleted = EvidenceExportJobRunner::run(storage, tenantId, exportStorePath, owner);
  summary.temporaryUploadsRemoved = MaintenanceJobRunner::cleanupTemporaryUploads(artifactTempPath);
  shared::Logger::event("info", "worker.coordinator.completed", {{"tenant_id", tenantId}, {"owner", owner}, {"expired_approvals", summary.expiredApprovals},
                                                                   {"expired_commands", summary.expiredCommands}, {"scheduled_releases", summary.scheduledReleases},
                                                                   {"gate_evaluations", summary.gateEvaluations}, {"outbox_published", summary.outboxPublished},
                                                                   {"outbox_dead_lettered", summary.outboxDeadLettered}, {"simulations_completed", summary.simulationsCompleted},
                                                                   {"replays_completed", summary.replaysCompleted}, {"benchmarks_completed", summary.benchmarksCompleted},
                                                                   {"exports_completed", summary.exportsCompleted}});
  return summary;
}

}  // namespace edgefleet::application
