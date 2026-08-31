#include "infrastructure/postgres_storage.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

namespace edgefleet::infrastructure {
namespace {

void replaceAll(std::string& value, const std::string& from, const std::string& to) {
  std::size_t position = 0;
  while ((position = value.find(from, position)) != std::string::npos) {
    value.replace(position, from.size(), to);
    position += to.size();
  }
}

std::string canonicalPostgresTimestamp(std::string timestamp) {
  const auto fractional = timestamp.find('.');
  auto timezone = timestamp.find('+', fractional == std::string::npos ? 0 : fractional);
  if (fractional == std::string::npos || timezone == std::string::npos) return timestamp;
  while (timezone > fractional + 1 && timestamp[timezone - 1] == '0') {
    timestamp.erase(timezone - 1, 1);
    --timezone;
  }
  if (timezone == fractional + 1) timestamp.erase(fractional, 1);
  return timestamp;
}

std::string postgresSql(std::string sql) {
  const bool ignoreConflict = sql.find("INSERT OR IGNORE INTO") != std::string::npos;
  replaceAll(sql, "COALESCE(MAX((julianday('now')-julianday(issued_at))*86400),0)", "COALESCE(MAX(EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - issued_at))),0)");
  replaceAll(sql, "INSERT OR IGNORE INTO", "INSERT INTO");
  replaceAll(sql, "BEGIN IMMEDIATE", "BEGIN");
  replaceAll(sql, "datetime('now','+24 hours')", "(CURRENT_TIMESTAMP + INTERVAL '24 hours')");
  replaceAll(sql, "datetime('now','+30 minutes')", "(CURRENT_TIMESTAMP + INTERVAL '30 minutes')");
  replaceAll(sql, "datetime('now','+7 days')", "(CURRENT_TIMESTAMP + INTERVAL '7 days')");
  replaceAll(sql, "datetime('now')", "CURRENT_TIMESTAMP");
  const std::regex relativeDate("datetime\\('now','([+-])([0-9]+) ([a-zA-Z]+)'\\)");
  sql = std::regex_replace(sql, relativeDate, "(CURRENT_TIMESTAMP $1 INTERVAL '$2 $3')");

  std::string result;
  result.reserve(sql.size() + 16);
  bool quoted = false;
  int parameter = 0;
  for (const char character : sql) {
    if (character == '\'') quoted = !quoted;
    if (character == '?' && !quoted) result += "$" + std::to_string(++parameter);
    else result += character;
  }
  if (ignoreConflict) {
    const auto end = result.find_last_not_of(" \t\r\n;");
    if (end != std::string::npos) result.insert(end + 1, " ON CONFLICT DO NOTHING");
  }
  return result;
}

shared::Json readValue(const PGresult* result, int row, int column) {
  if (PQgetisnull(result, row, column)) return nullptr;
  const std::string value = PQgetvalue(result, row, column);
  switch (PQftype(result, column)) {
    case 16: return value == "t";
    case 21:
    case 23:
    case 20:
      try { return std::stoll(value); } catch (const std::exception&) { return value; }
    case 700:
    case 701:
    case 1700:
      try { return std::stod(value); } catch (const std::exception&) { return value; }
    default: return value;
  }
}

}  // namespace

PostgresStorage::PostgresStorage(std::string connectionString) : connectionString_(std::move(connectionString)) {}

PostgresStorage::~PostgresStorage() {
  std::lock_guard lock(mutex_);
  if (connection_ != nullptr) PQfinish(connection_);
}

void PostgresStorage::setError(std::string message) const { lastError_ = std::move(message); }

bool PostgresStorage::open() {
  std::lock_guard lock(mutex_);
  if (connection_ != nullptr && PQstatus(connection_) == CONNECTION_OK) return true;
  if (connection_ != nullptr) { PQfinish(connection_); connection_ = nullptr; }
  connection_ = PQconnectdb(connectionString_.c_str());
  if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
    setError(connection_ == nullptr ? "PostgreSQL connection allocation failed" : PQerrorMessage(connection_));
    if (connection_ != nullptr) { PQfinish(connection_); connection_ = nullptr; }
    return false;
  }
  return executeUnlocked("SET TIME ZONE 'UTC'", {});
}

bool PostgresStorage::migrate(const std::string& directory) {
  std::lock_guard lock(mutex_);
  if (connection_ == nullptr) { setError("database is not open"); return false; }
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
    auto* result = PQexec(connection_, content.str().c_str());
    if (result == nullptr || PQresultStatus(result) != PGRES_COMMAND_OK) {
      setError(result == nullptr ? PQerrorMessage(connection_) : PQresultErrorMessage(result));
      if (result != nullptr) PQclear(result);
      return false;
    }
    PQclear(result);
  }
  return true;
}

bool PostgresStorage::healthy() const {
  std::lock_guard lock(mutex_);
  return connection_ != nullptr && PQstatus(connection_) == CONNECTION_OK && !queryUnlocked("SELECT 1 AS ok", {}).empty();
}

int PostgresStorage::schemaVersion() const {
  std::lock_guard lock(mutex_);
  const auto row = first("SELECT COALESCE(MAX(version),0) AS version FROM schema_version", {});
  return row ? row->value("version", 0) : 0;
}

std::string PostgresStorage::now() const {
  const auto current = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(current);
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(current - seconds).count();
  const auto raw = std::chrono::system_clock::to_time_t(seconds);
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

std::vector<shared::Json> PostgresStorage::queryUnlocked(const std::string& sql, const std::vector<std::string>& params) const {
  if (connection_ == nullptr) { setError("database is not open"); return {}; }
  const auto translated = postgresSql(sql);
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& parameter : params) values.push_back(parameter.c_str());
  auto* result = PQexecParams(connection_, translated.c_str(), static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0);
  if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
    setError(result == nullptr ? PQerrorMessage(connection_) : PQresultErrorMessage(result));
    if (result != nullptr) PQclear(result);
    return {};
  }
  std::vector<shared::Json> rows;
  for (int row = 0; row < PQntuples(result); ++row) {
    shared::Json value = shared::Json::object();
    for (int column = 0; column < PQnfields(result); ++column) value[PQfname(result, column)] = readValue(result, row, column);
    rows.push_back(std::move(value));
  }
  PQclear(result);
  return rows;
}

bool PostgresStorage::executeUnlocked(const std::string& sql, const std::vector<std::string>& params) {
  if (connection_ == nullptr) { setError("database is not open"); return false; }
  const auto translated = postgresSql(sql);
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& parameter : params) values.push_back(parameter.c_str());
  auto* result = PQexecParams(connection_, translated.c_str(), static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0);
  if (result == nullptr || PQresultStatus(result) != PGRES_COMMAND_OK) {
    setError(result == nullptr ? PQerrorMessage(connection_) : PQresultErrorMessage(result));
    if (result != nullptr) PQclear(result);
    return false;
  }
  PQclear(result);
  return true;
}

std::optional<shared::Json> PostgresStorage::first(const std::string& sql, const std::vector<std::string>& params) const {
  const auto rows = queryUnlocked(sql, params);
  return rows.empty() ? std::nullopt : std::optional<shared::Json>(rows.front());
}

std::vector<shared::Json> PostgresStorage::query(const std::string& sql, const std::vector<std::string>& params) const {
  std::lock_guard lock(mutex_);
  return queryUnlocked(sql, params);
}

bool PostgresStorage::execute(const std::string& sql, const std::vector<std::string>& params) {
  std::lock_guard lock(mutex_);
  return executeUnlocked(sql, params);
}

bool PostgresStorage::transaction(const std::function<bool()>& operation) {
  std::lock_guard lock(mutex_);
  if (connection_ == nullptr || !operation) return false;
  if (inTransaction_) return operation();
  if (!executeUnlocked("BEGIN", {})) return false;
  inTransaction_ = true;
  bool succeeded = false;
  try { succeeded = operation(); } catch (const std::exception& error) { setError(error.what()); succeeded = false; } catch (...) { setError("transaction callback failed"); succeeded = false; }
  inTransaction_ = false;
  if (succeeded && executeUnlocked("COMMIT", {})) return true;
  executeUnlocked("ROLLBACK", {});
  return false;
}

std::optional<shared::Json> PostgresStorage::createTenant(const std::string& name, const std::string& legalName, const std::string& displayName, const std::string& timezone, const std::string& apiPrefix, const std::string& apiHash) {
  std::lock_guard lock(mutex_);
  const auto id = shared::Uuid::generate().str();
  const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO tenants(id,name,legal_name,full_legal_name,display_name,default_timezone,api_key_prefix,api_key_hash,cohort_secret_ciphertext,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)", {id, name, legalName, legalName, displayName, timezone, apiPrefix, apiHash, "", timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,name,legal_name,full_legal_name,display_name,default_timezone,api_key_prefix,is_active,created_at,updated_at FROM tenants WHERE id=?", {id});
}

std::optional<shared::Json> PostgresStorage::findPrincipal(const std::string& apiPrefix, const std::string& apiHash) {
  std::lock_guard lock(mutex_);
  (void)apiHash;
  auto result = first("SELECT id AS tenant_id,'tenant:' || id || ':bootstrap' AS actor_id,'admin' AS role,api_key_hash AS credential_hash,name,legal_name,full_legal_name,display_name,address,registration,contact,wordmark,brand_color,default_timezone FROM tenants WHERE api_key_prefix=? AND is_active=1", {apiPrefix});
  if (result) return result;
  return first("SELECT c.tenant_id,c.id AS actor_id,c.role,c.key_hash AS credential_hash,t.name,t.legal_name,t.full_legal_name,t.display_name,t.address,t.registration,t.contact,t.wordmark,t.brand_color,t.default_timezone FROM operator_credentials c JOIN tenants t ON t.id=c.tenant_id WHERE c.key_prefix=? AND c.revoked_at IS NULL AND (c.expires_at IS NULL OR c.expires_at > CURRENT_TIMESTAMP) AND t.is_active=1", {apiPrefix});
}

std::optional<shared::Json> PostgresStorage::getTenant(const std::string& tenantId) { std::lock_guard lock(mutex_); return first("SELECT id,name,legal_name,full_legal_name,display_name,address,registration,contact,wordmark,brand_color,default_timezone,is_active FROM tenants WHERE id=?", {tenantId}); }

bool PostgresStorage::updateTenant(const std::string& tenantId, const shared::Json& fields) {
  const auto displayName = fields.value("display_name", "");
  const auto legalName = fields.value("legal_name", "");
  if (displayName.empty() || legalName.empty()) return false;
  return execute("UPDATE tenants SET display_name=?,legal_name=?,full_legal_name=?,updated_at=? WHERE id=?", {displayName, legalName, fields.value("full_legal_name", legalName), now(), tenantId});
}

std::optional<shared::Json> PostgresStorage::createFleet(const std::string& tenantId, const std::string& slug, const std::string& name, const std::string& environment, const shared::Json& labelSchema) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO fleets(id,tenant_id,name,slug,environment,label_schema_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)", {id, tenantId, name, slug, environment, shared::CanonicalJson::serialize(labelSchema), timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,name,slug,description,environment,status,label_schema_json,version,created_at,updated_at FROM fleets WHERE tenant_id=? AND id=?", {tenantId, id});
}

std::vector<shared::Json> PostgresStorage::listFleets(const std::string& tenantId) { std::lock_guard lock(mutex_); return queryUnlocked("SELECT id,tenant_id,name,slug,description,environment,status,label_schema_json,version,created_at,updated_at FROM fleets WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}); }
std::optional<shared::Json> PostgresStorage::getFleet(const std::string& tenantId, const std::string& fleetId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,name,slug,description,environment,status,label_schema_json,version,created_at,updated_at FROM fleets WHERE tenant_id=? AND id=?", {tenantId, fleetId}); }

std::optional<shared::Json> PostgresStorage::createDevice(const std::string& tenantId, const std::string& fleetId, const shared::Json& fields, const std::string& secretHash) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO devices(id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,labels_json,lifecycle_status,device_secret_hash,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", {id, tenantId, fleetId, fields.value("stable_key", ""), fields.value("display_name", fields.value("stable_key", "")), fields.value("hardware_model", "unknown"), fields.value("architecture", "unknown"), shared::CanonicalJson::serialize(fields.value("labels", shared::Json::object())), "registered", secretHash, timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,labels_json,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at,device_key_version,created_at,updated_at FROM devices WHERE tenant_id=? AND id=?", {tenantId, id});
}

std::vector<shared::Json> PostgresStorage::listDevices(const std::string& tenantId, const std::string& fleetId) { std::lock_guard lock(mutex_); return fleetId.empty() ? queryUnlocked("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at FROM devices WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}) : queryUnlocked("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at FROM devices WHERE tenant_id=? AND fleet_id=? ORDER BY created_at DESC,id DESC", {tenantId, fleetId}); }
std::optional<shared::Json> PostgresStorage::getDevice(const std::string& tenantId, const std::string& deviceId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,labels_json,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at,device_key_version FROM devices WHERE tenant_id=? AND id=?", {tenantId, deviceId}); }

std::optional<shared::Json> PostgresStorage::createPolicy(const std::string& tenantId, const shared::Json& fields) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO rollout_policies(id,tenant_id,name,version,status,selector_json,stage_plan_json,health_gates_json,max_offline_fraction,telemetry_freshness_sec,min_observation_sec,two_person_approval,require_iot_evidence,rollback_requirement,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", {id, tenantId, fields.value("name", "Policy"), std::to_string(fields.value("version", 1)), "draft", shared::CanonicalJson::serialize(fields.value("selector", shared::Json::object())), shared::CanonicalJson::serialize(fields.value("stage_plan", shared::Json::array({1,5,20,50,100}))), shared::CanonicalJson::serialize(fields.value("health_gates", shared::Json::object())), std::to_string(fields.value("max_offline_fraction", 0.20)), std::to_string(fields.value("telemetry_freshness_sec", 120)), std::to_string(fields.value("min_observation_sec", 900)), fields.value("two_person_approval", true) ? "1" : "0", fields.value("require_iot_evidence", false) ? "1" : "0", fields.value("rollback_requirement", "required"), fields.value("created_by_actor_id", "system"), timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,name,version,status,selector_json,stage_plan_json,health_gates_json,max_offline_fraction,telemetry_freshness_sec,min_observation_sec,two_person_approval,require_iot_evidence,rollback_requirement,created_by_actor_id,created_at,updated_at FROM rollout_policies WHERE tenant_id=? AND id=?", {tenantId, id});
}

std::vector<shared::Json> PostgresStorage::listPolicies(const std::string& tenantId) { std::lock_guard lock(mutex_); return queryUnlocked("SELECT id,tenant_id,name,version,status,stage_plan_json,rollback_requirement,created_at FROM rollout_policies WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}); }
std::optional<shared::Json> PostgresStorage::getPolicy(const std::string& tenantId, const std::string& policyId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,name,version,status,selector_json,stage_plan_json,health_gates_json,max_offline_fraction,telemetry_freshness_sec,min_observation_sec,two_person_approval,require_iot_evidence,rollback_requirement FROM rollout_policies WHERE tenant_id=? AND id=?", {tenantId, policyId}); }

std::optional<shared::Json> PostgresStorage::createRelease(const std::string& tenantId, const shared::Json& fields) {
  std::lock_guard lock(mutex_); const auto id = shared::Uuid::generate().str(); const auto timestamp = now();
  if (!executeUnlocked("INSERT INTO releases(id,tenant_id,fleet_id,artifact_id,rollback_artifact_id,policy_id,name,target_selector_json,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,NULLIF(?,''),?,?,?,?,?,?)", {id, tenantId, fields.value("fleet_id", ""), fields.value("artifact_id", ""), fields.value("rollback_artifact_id", ""), fields.value("policy_id", ""), fields.value("name", "Release"), shared::CanonicalJson::serialize(fields.value("selector", shared::Json::object())), fields.value("created_by_actor_id", "system"), timestamp, timestamp})) return std::nullopt;
  return first("SELECT id,tenant_id,fleet_id,artifact_id,rollback_artifact_id,policy_id,name,status,status_reason_code,target_selector_json,current_stage_ordinal,version,created_at,updated_at FROM releases WHERE tenant_id=? AND id=?", {tenantId, id});
}
std::vector<shared::Json> PostgresStorage::listReleases(const std::string& tenantId) { std::lock_guard lock(mutex_); return queryUnlocked("SELECT id,tenant_id,fleet_id,artifact_id,policy_id,name,status,status_reason_code,current_stage_ordinal,version,created_at,updated_at FROM releases WHERE tenant_id=? ORDER BY created_at DESC,id DESC", {tenantId}); }
std::optional<shared::Json> PostgresStorage::getRelease(const std::string& tenantId, const std::string& releaseId) { std::lock_guard lock(mutex_); return first("SELECT id,tenant_id,fleet_id,artifact_id,rollback_artifact_id,policy_id,name,status,status_reason_code,status_reason_text,target_selector_json,frozen_policy_json,frozen_manifest_json,frozen_rollback_json,membership_digest,eligible_device_count,current_stage_ordinal,scheduled_for,started_at,ended_at,version,created_at,updated_at FROM releases WHERE tenant_id=? AND id=?", {tenantId, releaseId}); }
bool PostgresStorage::updateRelease(const std::string& tenantId, const std::string& releaseId, const std::string& status, int expectedVersion) {
  const auto changed = query("UPDATE releases SET status=?,version=version+1,updated_at=? WHERE tenant_id=? AND id=? AND version=? RETURNING id", {status, now(), tenantId, releaseId, std::to_string(expectedVersion)});
  return changed.size() == 1;
}

std::optional<shared::Json> PostgresStorage::appendEvidence(const std::string& tenantId, const std::string& eventType, const std::string& aggregateType, const std::string& aggregateId, const shared::Json& payload, const std::string& actorType, const std::string& actorId) {
  std::lock_guard lock(mutex_);
  const bool ownsTransaction = !inTransaction_;
  if (ownsTransaction && !executeUnlocked("BEGIN", {})) return std::nullopt;
  if (queryUnlocked("SELECT pg_advisory_xact_lock(hashtext(?))", {tenantId}).empty()) { if (ownsTransaction) executeUnlocked("ROLLBACK", {}); return std::nullopt; }
  const auto previous = first("SELECT sequence_no,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no DESC LIMIT 1", {tenantId});
  const auto sequence = previous ? previous->at("sequence_no").get<long long>() + 1 : 1;
  const auto previousHash = previous ? previous->at("event_hash").get<std::string>() : std::string(64, '0');
  const auto id = shared::Uuid::generate().str();
  auto timestamp = now();
  if (timestamp.size() >= 1) timestamp.replace(timestamp.size() - 1, 1, "+00");
  if (timestamp.size() >= 11) timestamp[10] = ' ';
  timestamp = canonicalPostgresTimestamp(std::move(timestamp));
  const auto trace = shared::Uuid::generate().str();
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
    if (!executeUnlocked("INSERT INTO operator_notices(id,tenant_id,release_id,event_id,severity,title,body,created_at) VALUES(?,?,NULLIF(?,''),?,?,?,?,CURRENT_TIMESTAMP)", {shared::Uuid::generate().str(), tenantId, aggregateType == "release" ? aggregateId : "", id, severity, title, body})) { if (ownsTransaction) executeUnlocked("ROLLBACK", {}); return std::nullopt; }
  }
  if (ownsTransaction && !executeUnlocked("COMMIT", {})) { executeUnlocked("ROLLBACK", {}); return std::nullopt; }
  return shared::Json{{"id", id}, {"sequence_no", sequence}, {"event_hash", hash}, {"trace_id", trace}};
}

shared::Json PostgresStorage::verifyEvidence(const std::string& tenantId) const {
  std::lock_guard lock(mutex_);
  const auto rows = queryUnlocked("SELECT id,sequence_no,tenant_id,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no", {tenantId});
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

bool PostgresStorage::corruptFirstEvidenceForTest(const std::string& tenantId) { return execute("UPDATE evidence_events SET event_hash='corrupted' WHERE tenant_id=? AND sequence_no=(SELECT MIN(sequence_no) FROM evidence_events WHERE tenant_id=?)", {tenantId, tenantId}); }

}  // namespace edgefleet::infrastructure
