#include "infrastructure/sqlite_storage.hpp"

#include <chrono>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

namespace edgefleet::infrastructure {
namespace {

void bindParams(sqlite3_stmt* statement, const std::vector<std::string>& params) {
  for (std::size_t index = 0; index < params.size(); ++index) sqlite3_bind_text(statement, static_cast<int>(index + 1), params[index].c_str(), -1, SQLITE_TRANSIENT);
}

shared::Json readColumn(sqlite3_stmt* statement, int column) {
  switch (sqlite3_column_type(statement, column)) {
    case SQLITE_INTEGER: return sqlite3_column_int64(statement, column);
    case SQLITE_FLOAT: return sqlite3_column_double(statement, column);
    case SQLITE_NULL: return nullptr;
    default: return reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
  }
}

}  // namespace

SqliteStorage::SqliteStorage(std::string path, int busyTimeoutMs) : path_(std::move(path)), busyTimeoutMs_(std::clamp(busyTimeoutMs, 0, 120000)) {}

SqliteStorage::~SqliteStorage() { close(); }

void SqliteStorage::close() {
  std::lock_guard lock(mutex_);
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool SqliteStorage::open() {
  std::lock_guard lock(mutex_);
  if (db_ != nullptr) return true;
  const auto parent = std::filesystem::path(path_).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent);
  if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) { setError(sqlite3_errmsg(db_)); return false; }
  if (sqlite3_busy_timeout(db_, busyTimeoutMs_) != SQLITE_OK) { setError(sqlite3_errmsg(db_)); return false; }
  return executeUnlocked("PRAGMA foreign_keys = ON", {}) && executeUnlocked("PRAGMA journal_mode = WAL", {});
}

bool SqliteStorage::migrate(const std::string& directory) {
  std::lock_guard lock(mutex_);
  if (db_ == nullptr) { setError("database is not open"); return false; }
  std::vector<std::filesystem::path> files;
  std::error_code directoryError;
  if (!std::filesystem::is_directory(directory, directoryError) || directoryError) { setError("migration directory is unavailable"); return false; }
  for (const auto& entry : std::filesystem::directory_iterator(directory, directoryError)) if (entry.path().extension() == ".sql") files.push_back(entry.path());
  if (directoryError) { setError("migration directory could not be read"); return false; }
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    std::ifstream stream(file);
    std::stringstream content;
    content << stream.rdbuf();
    char* error = nullptr;
    if (sqlite3_exec(db_, content.str().c_str(), nullptr, nullptr, &error) != SQLITE_OK) { setError(error == nullptr ? "migration failed" : error); sqlite3_free(error); return false; }
  }
  return true;
}

bool SqliteStorage::healthy() const { std::lock_guard lock(mutex_); return db_ != nullptr && queryUnlocked("SELECT 1 AS ok", {}).size() == 1; }

int SqliteStorage::schemaVersion() const {
  const auto row = first("SELECT COALESCE(MAX(version),0) AS version FROM schema_version", {});
  return row ? row->value("version", 0) : 0;
}

std::string SqliteStorage::now() const {
  const auto time = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(time);
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(time - seconds).count();
  std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &raw);
#else
  gmtime_r(&raw, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(6) << std::setfill('0') << micros << "Z";
  return output.str();
}

void SqliteStorage::setError(std::string message) const { lastError_ = std::move(message); }

bool SqliteStorage::executeUnlocked(const std::string& sql, const std::vector<std::string>& params) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) { setError(sqlite3_errmsg(db_)); return false; }
  bindParams(statement, params);
  const auto result = sqlite3_step(statement);
  if (result != SQLITE_DONE && result != SQLITE_ROW) setError(sqlite3_errmsg(db_));
  sqlite3_finalize(statement);
  return result == SQLITE_DONE || result == SQLITE_ROW;
}

std::vector<shared::Json> SqliteStorage::queryUnlocked(const std::string& sql, const std::vector<std::string>& params) const {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) { setError(sqlite3_errmsg(db_)); return {}; }
  bindParams(statement, params);
  std::vector<shared::Json> rows;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    shared::Json row = shared::Json::object();
    for (int column = 0; column < sqlite3_column_count(statement); ++column) row[sqlite3_column_name(statement, column)] = readColumn(statement, column);
    rows.push_back(std::move(row));
  }
  sqlite3_finalize(statement);
  return rows;
}

std::optional<shared::Json> SqliteStorage::first(const std::string& sql, const std::vector<std::string>& params) const {
  const auto rows = queryUnlocked(sql, params);
  return rows.empty() ? std::nullopt : std::optional<shared::Json>(rows.front());
}

bool SqliteStorage::execute(const std::string& sql, const std::vector<std::string>& params) { std::lock_guard lock(mutex_); return db_ != nullptr && executeUnlocked(sql, params); }
std::vector<shared::Json> SqliteStorage::query(const std::string& sql, const std::vector<std::string>& params) const { std::lock_guard lock(mutex_); return db_ == nullptr ? std::vector<shared::Json>{} : queryUnlocked(sql, params); }

bool SqliteStorage::transaction(const std::function<bool()>& operation) {
  std::lock_guard lock(mutex_);
  if (db_ == nullptr || !operation) return false;
  if (inTransaction_) return operation();
  if (!executeUnlocked("BEGIN IMMEDIATE", {})) return false;
  inTransaction_ = true;
  bool succeeded = false;
  try { succeeded = operation(); } catch (const std::exception& error) { setError(error.what()); succeeded = false; } catch (...) { setError("transaction callback failed"); succeeded = false; }
  inTransaction_ = false;
  if (succeeded && executeUnlocked("COMMIT", {})) return true;
  executeUnlocked("ROLLBACK", {});
  return false;
}

std::optional<shared::Json> SqliteStorage::createTenant(const std::string& name, const std::string& legalName, const std::string& displayName, const std::string& timezone, const std::string& apiPrefix, const std::string& apiHash) {
  std::lock_guard lock(mutex_);
  const auto id = shared::Uuid::generate().str();
  const auto timestamp = now();
  const auto sql = "INSERT INTO tenants(id,name,legal_name,full_legal_name,display_name,default_timezone,api_key_prefix,api_key_hash,cohort_secret_ciphertext,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)";
  if (!executeUnlocked(sql, {id, name, legalName, legalName, displayName, timezone, apiPrefix, apiHash, "", timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,name,legal_name,full_legal_name,display_name,default_timezone,api_key_prefix,is_active,created_at,updated_at FROM tenants WHERE id=?", {id});
}

std::optional<shared::Json> SqliteStorage::findPrincipal(const std::string& apiPrefix, const std::string& apiHash) {
  std::lock_guard lock(mutex_);
  (void)apiHash;
  auto result = first("SELECT id AS tenant_id, 'tenant:' || id || ':bootstrap' AS actor_id, 'admin' AS role, api_key_hash AS credential_hash, name, legal_name, full_legal_name, display_name, address, registration, contact, wordmark, brand_color, default_timezone FROM tenants WHERE api_key_prefix=? AND is_active=1", {apiPrefix});
  if (result) return result;
  return first("SELECT c.tenant_id, c.id AS actor_id, c.role, c.key_hash AS credential_hash, t.name, t.legal_name, t.full_legal_name, t.display_name, t.address, t.registration, t.contact, t.wordmark, t.brand_color, t.default_timezone FROM operator_credentials c JOIN tenants t ON t.id=c.tenant_id WHERE c.key_prefix=? AND c.revoked_at IS NULL AND (c.expires_at IS NULL OR c.expires_at > datetime('now')) AND t.is_active=1", {apiPrefix});
}

std::optional<shared::Json> SqliteStorage::getTenant(const std::string& tenantId) { std::lock_guard lock(mutex_); return first("SELECT id,name,legal_name,full_legal_name,display_name,address,registration,contact,wordmark,brand_color,default_timezone,is_active FROM tenants WHERE id=?", {tenantId}); }

bool SqliteStorage::updateTenant(const std::string& tenantId, const shared::Json& fields) {
  const auto displayName = fields.value("display_name", "");
  const auto legalName = fields.value("legal_name", "");
  if (displayName.empty() || legalName.empty()) return false;
  return execute("UPDATE tenants SET display_name=?,legal_name=?,full_legal_name=?,updated_at=? WHERE id=?", {displayName, legalName, fields.value("full_legal_name", legalName), now(), tenantId});
}

std::optional<shared::Json> SqliteStorage::createFleet(const std::string& tenantId, const std::string& slug, const std::string& name, const std::string& environment, const shared::Json& labelSchema) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO fleets(id,tenant_id,name,slug,environment,label_schema_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)", {id, tenantId, name, slug, environment, shared::CanonicalJson::serialize(labelSchema), timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,name,slug,description,environment,status,label_schema_json,version,created_at,updated_at FROM fleets WHERE tenant_id=? AND id=?", {tenantId, id});
}

std::vector<shared::Json> SqliteStorage::listFleets(const std::string& tenantId) { std::lock_guard lock(mutex_); return queryUnlocked("SELECT id,tenant_id,name,slug,description,environment,status,label_schema_json,version,created_at,updated_at FROM fleets WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}); }
std::optional<shared::Json> SqliteStorage::getFleet(const std::string& tenantId, const std::string& fleetId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,name,slug,description,environment,status,label_schema_json,version,created_at,updated_at FROM fleets WHERE tenant_id=? AND id=?", {tenantId, fleetId}); }

std::optional<shared::Json> SqliteStorage::createDevice(const std::string& tenantId, const std::string& fleetId, const shared::Json& fields, const std::string& secretHash) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO devices(id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,labels_json,lifecycle_status,device_secret_hash,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", {id, tenantId, fleetId, fields.value("stable_key", ""), fields.value("display_name", fields.value("stable_key", "")), fields.value("hardware_model", "unknown"), fields.value("architecture", "unknown"), shared::CanonicalJson::serialize(fields.value("labels", shared::Json::object())), "registered", secretHash, timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,labels_json,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at,device_key_version,created_at,updated_at FROM devices WHERE tenant_id=? AND id=?", {tenantId, id});
}

std::vector<shared::Json> SqliteStorage::listDevices(const std::string& tenantId, const std::string& fleetId) { std::lock_guard lock(mutex_); return fleetId.empty() ? queryUnlocked("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at FROM devices WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}) : queryUnlocked("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at FROM devices WHERE tenant_id=? AND fleet_id=? ORDER BY created_at DESC,id DESC", {tenantId, fleetId}); }
std::optional<shared::Json> SqliteStorage::getDevice(const std::string& tenantId, const std::string& deviceId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,labels_json,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at,device_key_version FROM devices WHERE tenant_id=? AND id=?", {tenantId, deviceId}); }

std::optional<shared::Json> SqliteStorage::createPolicy(const std::string& tenantId, const shared::Json& fields) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO rollout_policies(id,tenant_id,name,version,status,selector_json,stage_plan_json,health_gates_json,max_offline_fraction,telemetry_freshness_sec,min_observation_sec,two_person_approval,require_iot_evidence,rollback_requirement,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", {id, tenantId, fields.value("name", "Policy"), std::to_string(fields.value("version", 1)), "draft", shared::CanonicalJson::serialize(fields.value("selector", shared::Json::object())), shared::CanonicalJson::serialize(fields.value("stage_plan", shared::Json::array({1,5,20,50,100}))), shared::CanonicalJson::serialize(fields.value("health_gates", shared::Json::object())), std::to_string(fields.value("max_offline_fraction", 0.20)), std::to_string(fields.value("telemetry_freshness_sec", 120)), std::to_string(fields.value("min_observation_sec", 900)), fields.value("two_person_approval", true) ? "1" : "0", fields.value("require_iot_evidence", false) ? "1" : "0", fields.value("rollback_requirement", "required"), fields.value("created_by_actor_id", "system"), timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,name,version,status,selector_json,stage_plan_json,health_gates_json,max_offline_fraction,telemetry_freshness_sec,min_observation_sec,two_person_approval,require_iot_evidence,rollback_requirement,created_by_actor_id,created_at,updated_at FROM rollout_policies WHERE tenant_id=? AND id=?", {tenantId, id});
}

std::vector<shared::Json> SqliteStorage::listPolicies(const std::string& tenantId) { std::lock_guard lock(mutex_); return queryUnlocked("SELECT id,tenant_id,name,version,status,stage_plan_json,rollback_requirement,created_at FROM rollout_policies WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}); }
std::optional<shared::Json> SqliteStorage::getPolicy(const std::string& tenantId, const std::string& policyId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,name,version,status,selector_json,stage_plan_json,health_gates_json,max_offline_fraction,telemetry_freshness_sec,min_observation_sec,two_person_approval,require_iot_evidence,rollback_requirement FROM rollout_policies WHERE tenant_id=? AND id=?", {tenantId, policyId}); }

std::optional<shared::Json> SqliteStorage::createRelease(const std::string& tenantId, const shared::Json& fields) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO releases(id,tenant_id,fleet_id,artifact_id,rollback_artifact_id,policy_id,name,target_selector_json,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,NULLIF(?,''),?,?,?,?,?,?)", {id, tenantId, fields.value("fleet_id", ""), fields.value("artifact_id", ""), fields.value("rollback_artifact_id", ""), fields.value("policy_id", ""), fields.value("name", "Release"), shared::CanonicalJson::serialize(fields.value("selector", shared::Json::object())), fields.value("created_by_actor_id", "system"), timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,fleet_id,artifact_id,rollback_artifact_id,policy_id,name,status,status_reason_code,target_selector_json,current_stage_ordinal,version,created_at,updated_at FROM releases WHERE tenant_id=? AND id=?", {tenantId, id});
}
std::vector<shared::Json> SqliteStorage::listReleases(const std::string& tenantId) { std::lock_guard lock(mutex_); return queryUnlocked("SELECT id,tenant_id,fleet_id,artifact_id,policy_id,name,status,status_reason_code,current_stage_ordinal,version,created_at,updated_at FROM releases WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}); }
std::optional<shared::Json> SqliteStorage::getRelease(const std::string& tenantId, const std::string& releaseId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,fleet_id,artifact_id,rollback_artifact_id,policy_id,name,status,status_reason_code,status_reason_text,target_selector_json,frozen_policy_json,frozen_manifest_json,frozen_rollback_json,membership_digest,eligible_device_count,current_stage_ordinal,scheduled_for,started_at,ended_at,version,created_at,updated_at FROM releases WHERE tenant_id=? AND id=?", {tenantId, releaseId}); }
bool SqliteStorage::updateRelease(const std::string& tenantId, const std::string& releaseId, const std::string& status, int expectedVersion) {
  std::lock_guard lock(mutex_);
  if (db_ == nullptr || !executeUnlocked("UPDATE releases SET status=?,version=version+1,updated_at=? WHERE tenant_id=? AND id=? AND version=?", {status, now(), tenantId, releaseId, std::to_string(expectedVersion)})) return false;
  return sqlite3_changes(db_) == 1;
}

std::optional<shared::Json> SqliteStorage::appendEvidence(const std::string& tenantId, const std::string& eventType, const std::string& aggregateType, const std::string& aggregateId, const shared::Json& payload, const std::string& actorType, const std::string& actorId) {
  std::lock_guard lock(mutex_);
  const bool ownsTransaction = !inTransaction_;
  if (ownsTransaction && !executeUnlocked("BEGIN IMMEDIATE", {})) return std::nullopt;
  const auto previous = first("SELECT sequence_no,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no DESC LIMIT 1", {tenantId});
  const auto sequence = previous ? previous->at("sequence_no").get<long long>() + 1 : 1;
  const auto previousHash = previous ? previous->at("event_hash").get<std::string>() : std::string(64, '0');
  const auto id = shared::Uuid::generate().str(); const auto timestamp = now(); const auto trace = shared::Uuid::generate().str();
  const shared::Json event = {{"id", id}, {"sequence_no", sequence}, {"tenant_id", tenantId}, {"aggregate_type", aggregateType}, {"aggregate_id", aggregateId}, {"event_type", eventType}, {"actor_type", actorType}, {"actor_id", actorId}, {"payload", payload}, {"occurred_at", timestamp}, {"trace_id", trace}, {"previous_hash", previousHash}};
  const auto hash = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(event));
  if (!executeUnlocked("INSERT INTO evidence_events(id,sequence_no,tenant_id,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", {id, std::to_string(sequence), tenantId, aggregateType, aggregateId, eventType, actorType, actorId, shared::CanonicalJson::serialize(payload), timestamp, trace, previousHash, hash})) { if (ownsTransaction) executeUnlocked("ROLLBACK", {}); return std::nullopt; }
  std::string severity;
  std::string title;
  if (eventType == "release.gate.evaluated") {
    const auto decision = payload.is_object() ? payload.value("decision", std::string{}) : std::string{};
    if (decision == "rollback" || decision == "abort") { severity = "critical"; title = "Release gate requires terminal control"; }
    else if (decision == "pause" || decision == "insufficient_evidence") { severity = "high"; title = "Release gate blocked promotion"; }
  } else if (eventType.ends_with("approval_requested")) {
    severity = "high";
    title = "Release approval required";
  } else if (eventType == "release.pause" || eventType == "release.rollback" || eventType == "release.abort") {
    severity = "critical";
    title = "Release control action recorded";
  }
  if (!title.empty()) {
    const auto body = shared::CanonicalJson::serialize({{"event_type", eventType}, {"aggregate_id", aggregateId}, {"payload", payload}});
    if (!executeUnlocked("INSERT INTO operator_notices(id,tenant_id,release_id,event_id,severity,title,body,created_at) VALUES(?,?,NULLIF(?,''),?,?,?,?,datetime('now'))", {shared::Uuid::generate().str(), tenantId, aggregateType == "release" ? aggregateId : "", id, severity, title, body})) { if (ownsTransaction) executeUnlocked("ROLLBACK", {}); return std::nullopt; }
  }
  if (ownsTransaction && !executeUnlocked("COMMIT", {})) { executeUnlocked("ROLLBACK", {}); return std::nullopt; }
  return shared::Json{{"id", id}, {"sequence_no", sequence}, {"event_hash", hash}, {"trace_id", trace}};
}

shared::Json SqliteStorage::verifyEvidence(const std::string& tenantId) const {
  std::lock_guard lock(mutex_); const auto rows = queryUnlocked("SELECT id,sequence_no,tenant_id,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no", {tenantId});
  std::string previous(64, '0'); int broken = 0; long long checked = 0;
  for (const auto& row : rows) {
    const auto event = shared::Json{{"id", row.at("id")}, {"sequence_no", row.at("sequence_no")}, {"tenant_id", row.at("tenant_id")}, {"aggregate_type", row.at("aggregate_type")}, {"aggregate_id", row.at("aggregate_id")}, {"event_type", row.at("event_type")}, {"actor_type", row.at("actor_type")}, {"actor_id", row.at("actor_id")}, {"payload", shared::Json::parse(row.at("payload_json").get<std::string>())}, {"occurred_at", row.at("occurred_at")}, {"trace_id", row.at("trace_id")}, {"previous_hash", row.at("previous_hash")}};
    const auto expected = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(event));
    ++checked;
    if (row.at("previous_hash") != previous || row.at("event_hash") != expected) { broken = static_cast<int>(row.at("sequence_no")); break; }
    previous = row.at("event_hash");
  }
  return {{"valid", broken == 0}, {"checked_events", checked}, {"first_broken_sequence", broken}};
}

bool SqliteStorage::corruptFirstEvidenceForTest(const std::string& tenantId) { return execute("UPDATE evidence_events SET event_hash='corrupted' WHERE tenant_id=? AND sequence_no=(SELECT MIN(sequence_no) FROM evidence_events WHERE tenant_id=?)", {tenantId, tenantId}); }

}  // namespace edgefleet::infrastructure
