#include "application/control_plane.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "domain/safety.hpp"
#include "domain/artifact.hpp"
#include "domain/benchmark.hpp"
#include "domain/replay.hpp"
#include "application/evidence_export.hpp"
#if defined(EDGEFLEET_HAS_POSTGRES)
#include "infrastructure/postgres_storage.hpp"
#endif
#include "infrastructure/sqlite_storage.hpp"
#include "infrastructure/integrations.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"
#include "shared/http_client_pool.hpp"
#include "shared/logger.hpp"
#include "shared/secret_resolver.hpp"
#include "shared/tenant_clock.hpp"
#include "web/policy_registry.hpp"

namespace edgefleet::application {
namespace {

using shared::Json;
using web::HttpRequest;
using web::HttpResponse;

HttpResponse jsonResponse(int status, Json body) { return {status, "application/json", body.dump(), {{"Cache-Control", "no-store"}}}; }

HttpResponse errorResponse(int status, std::string code, std::string message, std::string traceId, Json details = Json::array()) {
  auto response = jsonResponse(status, { {"error", { {"code", std::move(code)}, {"message", std::move(message)}, {"details", std::move(details)}, {"trace_id", traceId} } } });
  response.headers["X-Trace-ID"] = std::move(traceId);
  return response;
}

std::string pathOnly(std::string target) {
  const auto query = target.find('?');
  return query == std::string::npos ? std::move(target) : target.substr(0, query);
}

std::map<std::string, std::string> queryParameters(const std::string& target) {
  std::map<std::string, std::string> result;
  const auto marker = target.find('?');
  if (marker == std::string::npos) return result;
  std::stringstream stream(target.substr(marker + 1));
  std::string pair;
  while (std::getline(stream, pair, '&')) {
    const auto equals = pair.find('=');
    if (equals == std::string::npos) result[pair] = "";
    else result[pair.substr(0, equals)] = pair.substr(equals + 1);
  }
  return result;
}

std::string base64UrlEncode(std::string_view value) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  int bits = -6;
  unsigned int buffer = 0;
  for (const auto character : value) {
    buffer = (buffer << 8U) | static_cast<unsigned char>(character);
    bits += 8;
    while (bits >= 0) {
      output.push_back(alphabet[(buffer >> bits) & 0x3fU]);
      bits -= 6;
    }
  }
  if (bits > -6) output.push_back(alphabet[((buffer << 8U) >> (bits + 8)) & 0x3fU]);
  return output;
}

std::optional<std::string> base64UrlDecode(std::string_view value) {
  static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  int bits = -8;
  unsigned int buffer = 0;
  for (const auto character : value) {
    const auto position = alphabet.find(character);
    if (position == std::string_view::npos) return std::nullopt;
    buffer = (buffer << 6U) | static_cast<unsigned int>(position);
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<char>((buffer >> bits) & 0xffU));
      bits -= 8;
    }
  }
  return output;
}

std::string cursorFilterDigest(const std::map<std::string, std::string>& filters) {
  Json normalized = Json::object();
  for (const auto& [key, value] : filters) if (key != "cursor") normalized[key] = value;
  return shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(normalized));
}

std::string makeCursor(const shared::Config& config, std::string kind, std::string sortValue, std::string id,
                       const std::map<std::string, std::string>& filters) {
  const Json unsignedCursor{{"kind", std::move(kind)}, {"sort", std::move(sortValue)}, {"id", std::move(id)}, {"filter_digest", cursorFilterDigest(filters)}};
  const auto payload = shared::CanonicalJson::serialize(unsignedCursor);
  const auto signedCursor = Json{{"payload", unsignedCursor}, {"signature", shared::DigestService::hmacSha256Hex(config.cursorHmacKey, payload)}};
  return base64UrlEncode(shared::CanonicalJson::serialize(signedCursor));
}

std::optional<Json> readCursor(const shared::Config& config, const std::map<std::string, std::string>& filters, std::string_view token,
                               std::string_view expectedKind) {
  if (token.empty()) return std::nullopt;
  const auto decoded = base64UrlDecode(token);
  if (!decoded.has_value()) return std::optional<Json>(Json{{"error", "invalid"}});
  try {
    const auto signedCursor = Json::parse(*decoded);
    const auto payload = signedCursor.at("payload");
    const auto signature = signedCursor.at("signature").get<std::string>();
    if (payload.value("kind", "") != expectedKind || payload.value("filter_digest", "") != cursorFilterDigest(filters) ||
        !shared::DigestService::constantTimeEqual(signature, shared::DigestService::hmacSha256Hex(config.cursorHmacKey, shared::CanonicalJson::serialize(payload)))) return std::optional<Json>(Json{{"error", "invalid"}});
    if (payload.value("sort", "").empty() || payload.value("id", "").empty()) return std::optional<Json>(Json{{"error", "invalid"}});
    return std::optional<Json>(payload);
  } catch (const std::exception&) { return std::optional<Json>(Json{{"error", "invalid"}}); }
}

int pageLimit(const std::map<std::string, std::string>& filters) {
  try { return std::clamp(std::stoi(filters.contains("limit") ? filters.at("limit") : "50"), 1, 200); }
  catch (const std::exception&) { return 50; }
}

std::vector<std::string> segments(const std::string& path) {
  std::vector<std::string> result;
  std::stringstream stream(path);
  std::string segment;
  while (std::getline(stream, segment, '/')) if (!segment.empty()) result.push_back(segment);
  return result;
}

bool isMutation(const std::string& method) { return method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE"; }

std::string stringValue(const Json& value, const std::string& key, std::string fallback = {}) {
  const auto found = value.find(key);
  return found == value.end() || found->is_null() || !found->is_string() ? std::move(fallback) : found->get<std::string>();
}

bool boolValue(const Json& value, const std::string& key, bool fallback) {
  const auto found = value.find(key);
  if (found == value.end() || found->is_null()) return fallback;
  if (found->is_boolean()) return found->get<bool>();
  if (found->is_number_integer()) return found->get<long long>() != 0;
  if (found->is_string()) {
    const auto text = found->get<std::string>();
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
  }
  return fallback;
}

std::string header(const HttpRequest& request, std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  const auto found = request.headers.find(key);
  return found == request.headers.end() ? std::string{} : found->second;
}

std::string cookie(const HttpRequest& request, std::string_view name) {
  const auto cookies = header(request, "cookie");
  const std::string marker = std::string(name) + "=";
  const auto position = cookies.find(marker);
  if (position == std::string::npos) return {};
  const auto start = position + marker.size();
  const auto end = cookies.find(';', start);
  return cookies.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string htmlEscape(std::string value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    if (character == '&') escaped += "&amp;";
    else if (character == '<') escaped += "&lt;";
    else if (character == '>') escaped += "&gt;";
    else if (character == '\"') escaped += "&quot;";
    else if (character == '\'') escaped += "&#39;";
    else escaped += character;
  }
  return escaped;
}

Json parseBody(const HttpRequest& request) {
  if (!request.body.empty()) return Json::parse(request.body);
  if (request.bodyFilePath.empty()) return Json::object();
  std::ifstream input(request.bodyFilePath, std::ios::binary);
  if (!input.is_open()) throw std::runtime_error("request body spool is unavailable");
  return Json::parse(std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()});
}

std::string requestBodyDigest(const HttpRequest& request) {
  return request.bodyFilePath.empty() ? shared::DigestService::sha256Hex(request.body) : shared::DigestService::sha256File(request.bodyFilePath);
}

std::string secretFor(const std::string& prefix) { (void)prefix; return shared::Uuid::generate().str() + shared::Uuid::generate().str(); }

std::string apiKey(const std::string& prefix, const std::string& secret) { return prefix + "." + secret; }

std::filesystem::path artifactPath(const shared::Config& config, const std::string& tenantId, const std::string& digest) {
  return std::filesystem::path(config.artifactStorePath) / tenantId / digest;
}

bool validSelector(const Json& labels, const Json& selector) {
  if (!selector.is_object()) return false;
  for (const auto& [key, expected] : selector.items()) if (!labels.contains(key) || labels.at(key) != expected) return false;
  return true;
}

bool validLabels(const Json& labels) {
  if (!labels.is_object() || labels.size() > 50) return false;
  for (const auto& [key, value] : labels.items()) {
    if (key.empty() || key.size() > 64 || !value.is_string() || value.get<std::string>().size() > 256) return false;
  }
  return true;
}

bool validLabelSchema(const Json& schema) {
  if (!schema.is_object() || schema.size() > 50) return false;
  for (const auto& [key, rule] : schema.items()) {
    if (key.empty() || key.size() > 64 || (!rule.is_string() && !rule.is_array())) return false;
    if (rule.is_array()) for (const auto& value : rule) if (!value.is_string() || value.get<std::string>().size() > 256) return false;
  }
  return true;
}

bool labelsFitSchema(const Json& labels, const Json& schema) {
  if (schema.empty()) return true;
  for (const auto& [key, value] : labels.items()) {
    if (!schema.contains(key)) return false;
    const auto& rule = schema.at(key);
    if (rule.is_array() && std::find(rule.begin(), rule.end(), value) == rule.end()) return false;
  }
  return true;
}

bool artifactBytesMatch(const Json& artifact) {
  const auto storageKey = stringValue(artifact, "storage_key");
  const auto expected = stringValue(artifact, "sha256_digest");
  if (storageKey.empty() || expected.empty()) return false;
  return shared::DigestService::constantTimeEqual(shared::DigestService::sha256File(storageKey), expected);
}

bool settleReleaseAfterDeviceReport(infrastructure::Storage& storage, const std::string& tenantId, const std::string& releaseId, const std::string& actorId) {
  if (releaseId.empty()) return true;
  const auto release = storage.query("SELECT status,version FROM releases WHERE tenant_id=? AND id=?", {tenantId, releaseId});
  if (release.empty()) return true;
  const auto status = release.front().value("status", "");
  if (status != "aborting" && status != "rolling_back") return true;
  if (!storage.query("SELECT id FROM release_assignments WHERE tenant_id=? AND release_id=? AND state IN ('pending','commanded','acknowledged','cancelling')", {tenantId, releaseId}).empty()) return true;
  const auto assignments = storage.query("SELECT state,failure_code FROM release_assignments WHERE tenant_id=? AND release_id=?", {tenantId, releaseId});
  if (assignments.empty()) return true;
  const bool rollbackFailed = std::any_of(assignments.begin(), assignments.end(), [](const auto& row) { return row.value("state", "") == "failed" && row.value("failure_code", "") == "ROLLBACK_FAILED"; });
  const auto next = status == "aborting" ? std::string("aborted") : rollbackFailed ? std::string("failed") : std::string("rolled_back");
  const auto currentState = domain::releaseStateFromString(status);
  const auto terminalAction = status == "aborting" ? domain::ReleaseAction::abort : domain::ReleaseAction::rollback;
  if (!currentState.has_value() || !domain::ReleaseStateMachine::transition(*currentState, terminalAction).has_value()) return false;
  if (!storage.updateRelease(tenantId, releaseId, next, release.front().value("version", 0))) {
    const auto after = storage.query("SELECT status FROM releases WHERE tenant_id=? AND id=?", {tenantId, releaseId});
    return !after.empty() && after.front().value("status", "") != status;
  }
  if (!storage.execute("UPDATE releases SET ended_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status=?", {tenantId, releaseId, next})) return false;
  return storage.appendEvidence(tenantId, "release." + next, "release", releaseId, {{"from", status}, {"to", next}}, "device", actorId).has_value();
}

bool isJsonObjectBody(const HttpRequest& request) {
  if (!request.body.empty()) {
    const auto first = request.body.find_first_not_of(" \t\r\n");
    return first != std::string::npos && request.body[first] == '{';
  }
  if (request.bodyFilePath.empty()) return false;
  std::ifstream input(request.bodyFilePath, std::ios::binary);
  char character = 0;
  while (input.get(character)) {
    if (character != ' ' && character != '\t' && character != '\r' && character != '\n') return character == '{';
  }
  return false;
}

std::optional<std::uint32_t> ipv4Address(std::string_view value) {
  std::uint32_t result = 0;
  std::size_t offset = 0;
  for (int index = 0; index < 4; ++index) {
    const auto dot = value.find('.', offset);
    const auto part = value.substr(offset, dot == std::string_view::npos ? std::string_view::npos : dot - offset);
    unsigned int octet = 0;
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), octet);
    if (part.empty() || parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || octet > 255 || (dot == std::string_view::npos && index != 3) || (dot != std::string_view::npos && index == 3)) return std::nullopt;
    result = (result << 8U) | octet;
    if (dot == std::string_view::npos) break;
    offset = dot + 1;
  }
  return result;
}

std::string adapterHost(std::string_view value) {
  const auto scheme = value.find("://");
  if (scheme == std::string_view::npos) return {};
  const auto start = scheme + 3;
  const auto end = value.find_first_of("/?!#", start);
  auto authority = value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
  if (!authority.empty() && authority.front() == '[') {
    const auto closing = authority.find(']');
    return closing == std::string_view::npos ? std::string{} : std::string(authority.substr(1, closing - 1));
  }
  const auto colon = authority.find(':');
  return std::string(authority.substr(0, colon));
}

bool adapterHostInConfiguredCidr(const std::string& host, const std::string& cidrs) {
  const auto address = ipv4Address(host);
  if (!address.has_value()) return false;
  std::stringstream stream(cidrs);
  std::string entry;
  while (std::getline(stream, entry, ',')) {
    const auto slash = entry.find('/');
    if (slash == std::string::npos) continue;
    const auto network = ipv4Address(entry.substr(0, slash));
    unsigned int prefix = 0;
    const auto prefixText = entry.substr(slash + 1);
    const auto parsed = std::from_chars(prefixText.data(), prefixText.data() + prefixText.size(), prefix);
    if (!network.has_value() || parsed.ec != std::errc{} || parsed.ptr != prefixText.data() + prefixText.size() || prefix > 32) continue;
    const auto mask = prefix == 0 ? 0U : 0xffffffffU << (32U - prefix);
    if (((*address) & mask) == ((*network) & mask)) return true;
  }
  return false;
}

bool safeAdapterUrl(const shared::Config& config, const std::string& value) {
  if (!shared::isSafePrivateHttpUrl(value)) return false;
  const auto host = adapterHost(value);
  if (host == "localhost" || host == "::1" || host.starts_with("127.")) return true;
  return adapterHostInConfiguredCidr(host, config.privateAdapterCidrs);
}

std::string traceIdFromRequest(const HttpRequest& request, std::string fallback) {
  const auto parent = header(request, "traceparent");
  if (parent.size() != 55 || parent[2] != '-' || parent[35] != '-' || parent[52] != '-') return fallback;
  const auto traceId = parent.substr(3, 32);
  if (traceId == std::string(32, '0')) return fallback;
  for (const auto character : traceId) if (!std::isxdigit(static_cast<unsigned char>(character))) return fallback;
  return traceId;
}

bool finiteNumberInRange(const Json& value, double minimum, double maximum) {
  return value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() >= minimum && value.get<double>() <= maximum;
}

std::string safeFileName(std::string value) {
  if (value.empty() || value.size() > 160) return "artifact.bin";
  for (const auto character : value) if (character == '/' || character == '\\' || character == '"' || character == '\r' || character == '\n' || character == '\0') return "artifact.bin";
  return value;
}

std::optional<std::size_t> parseByteOffset(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<std::string> policyValidationError(const Json& policy) {
  static const std::vector<std::string> fields{"name", "version", "selector", "stage_plan", "health_gates", "max_offline_fraction", "telemetry_freshness_sec", "min_observation_sec", "two_person_approval", "require_iot_evidence", "rollback_requirement", "strategy", "created_by_actor_id"};
  for (const auto& [key, value] : policy.items()) if (std::find(fields.begin(), fields.end(), key) == fields.end()) return "UNKNOWN_POLICY_FIELD:" + key;
  if (policy.contains("strategy") && policy.at("strategy") != "fixed_wave") return "UNKNOWN_STRATEGY";
  const auto stagePlan = policy.value("stage_plan", Json::array({1, 5, 20, 50, 100}));
  if (!stagePlan.is_array() || stagePlan.empty() || stagePlan.size() > 5 || !stagePlan.back().is_number_integer() || stagePlan.back() != 100) return "INVALID_STAGE_PLAN";
  for (std::size_t index = 0; index < stagePlan.size(); ++index) if (!stagePlan[index].is_number_integer() || stagePlan[index].get<int>() <= 0 || (index > 0 && stagePlan[index].get<int>() <= stagePlan[index - 1].get<int>())) return "INVALID_STAGE_PLAN";
  if (policy.contains("max_offline_fraction") && !finiteNumberInRange(policy.at("max_offline_fraction"), 0.0, 1.0)) return "INVALID_MAX_OFFLINE_FRACTION";
  if (policy.contains("telemetry_freshness_sec") && (!policy.at("telemetry_freshness_sec").is_number_integer() || policy.at("telemetry_freshness_sec").get<long long>() < 1 || policy.at("telemetry_freshness_sec").get<long long>() > 604800)) return "INVALID_TELEMETRY_FRESHNESS";
  if (policy.contains("min_observation_sec") && (!policy.at("min_observation_sec").is_number_integer() || policy.at("min_observation_sec").get<long long>() < 1 || policy.at("min_observation_sec").get<long long>() > 604800)) return "INVALID_MIN_OBSERVATION";
  for (const auto& key : {std::string("two_person_approval"), std::string("require_iot_evidence")}) if (policy.contains(key) && !policy.at(key).is_boolean()) return "INVALID_BOOLEAN_FIELD:" + key;
  const auto rollback = policy.value("rollback_requirement", "required");
  if (rollback != "required" && rollback != "allow_first_install") return "INVALID_ROLLBACK_REQUIREMENT";
  const auto selector = policy.value("selector", Json::object());
  if (!selector.is_object()) return "INVALID_SELECTOR";
  const auto gates = policy.value("health_gates", Json::object());
  if (!gates.is_object()) return "INVALID_HEALTH_GATES";
  static const std::vector<std::string> gateNames{"fresh_device_coverage", "install_failure_rate", "crash_free_rate", "health_failure_rate", "convergence_rate", "offline_fraction", "rollback_failure_rate"};
  for (const auto& [key, value] : gates.items()) {
    if (std::find(gateNames.begin(), gateNames.end(), key) == gateNames.end()) return "UNKNOWN_GATE_FIELD:" + key;
    if (value.is_boolean()) continue;
    if (!finiteNumberInRange(value, 0.0, 100.0)) return "INVALID_GATE_VALUE:" + key;
  }
  return std::nullopt;
}

std::optional<domain::ReleaseAction> releaseAction(const std::string& value) {
  static const std::vector<std::pair<std::string, domain::ReleaseAction>> actions{
      {"validate", domain::ReleaseAction::validate}, {"submit", domain::ReleaseAction::submit}, {"approve", domain::ReleaseAction::approve},
      {"schedule", domain::ReleaseAction::schedule}, {"start", domain::ReleaseAction::start}, {"cancel", domain::ReleaseAction::cancel},
      {"pause", domain::ReleaseAction::pause}, {"resume", domain::ReleaseAction::resume}, {"abort", domain::ReleaseAction::abort},
      {"rollback", domain::ReleaseAction::rollback}};
  const auto found = std::find_if(actions.begin(), actions.end(), [&](const auto& item) { return item.first == value; });
  return found == actions.end() ? std::nullopt : std::optional<domain::ReleaseAction>(found->second);
}

bool hasReason(const Json& body) {
  return body.contains("reason") && body.at("reason").is_string() && !body.at("reason").get<std::string>().empty();
}

bool matchesDeviceSecret(const shared::Config& config, std::string_view stored, std::string_view presented) {
  if (stored.starts_with("v1:")) {
    const auto decrypted = shared::DigestService::decryptSecret(config.credentialEncryptionKey, stored);
    return decrypted.has_value() && shared::DigestService::constantTimeEqual(*decrypted, presented);
  }
  return shared::DigestService::constantTimeEqual(stored, shared::DigestService::sha256Hex(presented));
}

}  // namespace

ControlPlane::ControlPlane(shared::Config config) : config_(std::move(config)) {}

bool ControlPlane::initialize(const std::string& migrationDirectory) {
  if (const auto error = shared::Config::validate(config_); error.has_value()) return false;
  shared::Logger::configure(config_.logLevel, config_.logFormat);
  std::error_code directoryError;
  std::filesystem::create_directories(config_.artifactStorePath, directoryError);
  std::filesystem::create_directories(config_.artifactTempPath, directoryError);
  std::filesystem::create_directories(config_.traceStorePath, directoryError);
  std::filesystem::create_directories(config_.exportStorePath, directoryError);
  if (config_.storageBackend == "postgres") {
#if defined(EDGEFLEET_HAS_POSTGRES)
    storage_ = std::make_unique<infrastructure::PostgresStorage>(config_.databaseUrl);
#else
    return false;
#endif
  } else {
    storage_ = std::make_unique<infrastructure::SqliteStorage>(config_.sqlitePath, config_.sqliteBusyTimeoutMs);
  }
  initialized_ = storage_->open() && storage_->migrate(migrationDirectory);
  shared::Logger::event(initialized_ ? "info" : "error", "control_plane.initialization", { {"storage_backend", config_.storageBackend}, {"status", initialized_ ? "ready" : "failed"} });
  return initialized_;
}

ControlPlane::AuthResult ControlPlane::authenticate(const HttpRequest& request) {
  AuthResult result;
  result.traceId = traceIdFromRequest(request, shared::Uuid::generate().str());
  if (pathOnly(request.target).starts_with("/api/agent/v1")) {
    const auto deviceId = header(request, "x-device-id");
    const auto deviceSecret = header(request, "x-device-secret");
    const auto keyVersion = header(request, "x-device-key-version");
    const auto deviceSequence = header(request, "x-device-sequence");
    if (!deviceId.empty() && !deviceSecret.empty() && !keyVersion.empty() && !deviceSequence.empty()) {
      const auto device = storage_->query("SELECT tenant_id,device_secret_hash,device_key_version,lifecycle_status FROM devices WHERE id=?", {deviceId});
      const auto expectedSignature = shared::DigestService::hmacSha256Hex(deviceSecret, request.method + " " + pathOnly(request.target) + " " + deviceSequence + " " + requestBodyDigest(request));
      const auto lifecycle = device.empty() ? std::string{} : device.front().value("lifecycle_status", std::string{});
      int presentedVersion = 0;
      try { presentedVersion = std::stoi(keyVersion); } catch (const std::exception&) { presentedVersion = -1; }
      std::string storedHash;
      if (!device.empty() && device.front().value("device_key_version", 0) == presentedVersion) storedHash = device.front().value("device_secret_hash", std::string{});
      if (storedHash.empty() && !device.empty()) {
        const auto successor = storage_->query("SELECT secret_ciphertext FROM device_credentials WHERE tenant_id=? AND device_id=? AND key_version=? AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > datetime('now'))", {device.front().at("tenant_id").get<std::string>(), deviceId, std::to_string(presentedVersion)});
        if (!successor.empty()) storedHash = successor.front().value("secret_ciphertext", std::string{});
      }
      if (!device.empty() && lifecycle != "decommissioned" && !storedHash.empty() && matchesDeviceSecret(config_, storedHash, deviceSecret) && shared::DigestService::constantTimeEqual(header(request, "x-device-signature"), expectedSignature)) {
        result.context.tenantId = device.front().at("tenant_id").get<std::string>();
        result.context.actorId = deviceId;
        result.context.role = shared::Role::device;
        result.context.authenticated = true;
        return result;
      }
    }
  }
  auto credential = header(request, "authorization");
  if (credential.starts_with("Bearer ")) credential = credential.substr(7);
  if (credential.empty()) credential = header(request, "x-api-key");
  if (credential.empty() && pathOnly(request.target) == "/auth/session" && request.method == "POST") {
    try {
      credential = Json::parse(request.body).value("api_key", "");
    } catch (const std::exception&) {
      const auto marker = request.body.find("api_key=");
      if (marker != std::string::npos) { const auto start = marker + 8; const auto end = request.body.find('&', start); credential = request.body.substr(start, end == std::string::npos ? std::string::npos : end - start); }
    }
  }
  if (credential.empty()) {
    const auto sessionCookie = cookie(request, "edgefleet_session");
    if (!sessionCookie.empty()) {
      credential = sessionCookie;
      const auto session = storage_->query("SELECT tenant_id,actor_id FROM browser_sessions WHERE token_hash=? AND revoked_at IS NULL AND expires_at > datetime('now')", {shared::DigestService::sha256Hex(credential)});
      if (!session.empty()) {
        result.context.tenantId = session.front().at("tenant_id").get<std::string>();
        result.context.actorId = session.front().at("actor_id").get<std::string>();
        result.context.role = shared::Role::viewer;
        const auto principal = storage_->query("SELECT t.display_name,COALESCE(c.role,'admin') AS role FROM browser_sessions s JOIN tenants t ON t.id=s.tenant_id LEFT JOIN operator_credentials c ON c.id=s.actor_id WHERE s.token_hash=?", {shared::DigestService::sha256Hex(credential)});
        if (!principal.empty()) { result.principal = principal.front(); result.context.role = shared::roleFromString(principal.front().value("role", "viewer")).value_or(shared::Role::viewer); }
        else { result.principal = storage_->query("SELECT display_name,'admin' AS role FROM tenants WHERE id=?", {result.context.tenantId}).empty() ? Json::object() : storage_->query("SELECT display_name,'admin' AS role FROM tenants WHERE id=?", {result.context.tenantId}).front(); result.context.role = shared::Role::admin; }
        result.context.authenticated = true;
        return result;
      }
    }
  }
  if (!credential.starts_with("edge_live_")) return result;
  const auto prefixStart = std::size_t{0};
  const auto dot = credential.find('.', prefixStart);
  if (dot == std::string::npos || dot == credential.size() - 1) return result;
  const auto prefix = credential.substr(prefixStart, dot - prefixStart);
  const auto secret = credential.substr(dot + 1);
  const auto principal = storage_->findPrincipal(prefix, secret);
  if (!principal.has_value() || !shared::DigestService::argon2idVerify(secret, principal->value("credential_hash", ""))) return result;
  result.principal = *principal;
  result.principal.erase("credential_hash");
  result.context.tenantId = principal->at("tenant_id").get<std::string>();
  result.context.actorId = principal->at("actor_id").get<std::string>();
  result.context.role = shared::roleFromString(principal->value("role", "admin")).value_or(shared::Role::viewer);
  result.context.authenticated = true;
  storage_->execute("UPDATE operator_credentials SET last_used_at=datetime('now') WHERE id=?", {result.context.actorId});
  return result;
}

HttpResponse ControlPlane::health(const HttpRequest& request) const {
  const auto path = pathOnly(request.target);
  if (path == "/health" || path == "/health/live") return jsonResponse(200, {{"status", "ok"}, {"service", "edgefleet"}});
  if (path == "/health/db") return jsonResponse(storage_ != nullptr && storage_->healthy() ? 200 : 503, {{"status", storage_ != nullptr && storage_->healthy() ? "ok" : "failed"}, {"schema_version", storage_ == nullptr ? 0 : storage_->schemaVersion()}});
  if (path == "/health/artifacts") {
    std::error_code error;
    const bool exists = std::filesystem::exists(config_.artifactStorePath) || std::filesystem::create_directories(config_.artifactStorePath, error);
    const auto available = exists && !error ? std::filesystem::space(config_.artifactStorePath, error).available : 0;
    const bool healthy = exists && !error && available >= static_cast<std::uintmax_t>(config_.artifactMinFreeBytes);
    return jsonResponse(healthy ? 200 : 503, {{"status", healthy ? "ok" : "failed"}, {"available_bytes", available}, {"minimum_free_bytes", config_.artifactMinFreeBytes}});
  }
  if (path == "/health/evidence") {
    if (storage_ == nullptr) return jsonResponse(503, {{"status", "failed"}});
    const auto tenants = storage_->query("SELECT id FROM tenants WHERE is_active=1");
    for (const auto& tenant : tenants) {
      const auto verification = storage_->verifyEvidence(tenant.at("id").get<std::string>());
      if (!verification.value("valid", false)) return jsonResponse(503, {{"status", "failed"}, {"verification", {{"first_broken_sequence", verification.value("first_broken_sequence", 0)}}}});
    }
    const auto heads = storage_->query("SELECT tenant_id,MAX(sequence_no) AS last_sequence FROM evidence_events GROUP BY tenant_id");
    return jsonResponse(200, {{"status", "ok"}, {"tenants_checked", tenants.size()}, {"head_count", heads.size()}});
  }
  if (path == "/health/ready") {
    std::error_code error;
    const bool artifactsExist = std::filesystem::exists(config_.artifactStorePath) || std::filesystem::create_directories(config_.artifactStorePath, error);
    const auto artifactSpace = artifactsExist && !error ? std::filesystem::space(config_.artifactStorePath, error).available : 0;
    const bool artifacts = artifactsExist && !error && artifactSpace >= static_cast<std::uintmax_t>(config_.artifactMinFreeBytes);
    const bool database = initialized_ && storage_ != nullptr && storage_->healthy() && storage_->schemaVersion() >= 1;
    const bool runtimeSecrets = !config_.sessionEncryptionKey.empty() && !config_.credentialEncryptionKey.empty() && !config_.cursorHmacKey.empty();
    bool evidence = false;
    if (database) {
      evidence = true;
      for (const auto& tenant : storage_->query("SELECT id FROM tenants WHERE is_active=1")) evidence = evidence && storage_->verifyEvidence(tenant.at("id").get<std::string>()).value("valid", false);
    }
    const bool ready = database && artifacts && evidence && runtimeSecrets && !error;
    return jsonResponse(ready ? 200 : 503, {{"status", ready ? "ready" : "not_ready"}, {"database", database}, {"artifact_store", artifacts}, {"artifact_available_bytes", artifactSpace}, {"evidence_store", evidence}, {"runtime_secrets", runtimeSecrets}, {"optional_adapters", "non_blocking"}});
  }
  return jsonResponse(initialized_ && storage_ != nullptr && storage_->healthy() ? 200 : 503, {{"status", initialized_ ? "ok" : "starting"}, {"db", storage_ != nullptr && storage_->healthy()}, {"schema_version", storage_ == nullptr ? 0 : storage_->schemaVersion()}});
}

std::optional<HttpResponse> ControlPlane::rateLimit(const HttpRequest& request, const AuthResult& auth) {
  const auto path = pathOnly(request.target);
  int limit = request.method == "GET" ? 120 : 30;
  int windowSeconds = 60;
  if (path == "/api/tenants/register") limit = 5;
  else if (path == "/auth/session") limit = 10;
  else if (path == "/api/benchmarks" && request.method == "POST") { limit = 2; windowSeconds = 60 * 60; }
  else if (path == "/api/agent/v1/desired-state") limit = 12;
  else if (path == "/api/agent/v1/credential-rotation/ack") limit = 10;
  else if (path == "/api/agent/v1/reports") limit = 120;
  else if (path.starts_with("/api/agent/v1/artifacts/")) limit = 240;
  else if (path == "/api/evidence/verify") limit = 5;
  else if (path.starts_with("/api/releases/") && path.ends_with("/gates") && request.method == "GET") limit = 120;

  const auto client = auth.context.role == shared::Role::device ? auth.context.actorId : (header(request, "x-peer-address").empty() ? "local" : header(request, "x-peer-address"));
  const auto key = auth.context.tenantId + "|" + (client.empty() ? "local" : client) + "|" + request.method + "|" + path;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(rateLimitMutex_);
  auto& bucket = rateLimits_[key];
  if (bucket.windowStarted.time_since_epoch().count() == 0 || now - bucket.windowStarted >= std::chrono::seconds(windowSeconds)) {
    bucket.windowStarted = now;
    bucket.requests = 0;
  }
  if (bucket.requests >= limit) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - bucket.windowStarted).count();
    const auto retryAfter = std::max<long long>(1, windowSeconds - elapsed);
    return HttpResponse{429, "application/json", jsonResponse(429, {{"error", {{"code", "RATE_LIMITED"}, {"message", "Request rate exceeded for this route."}, {"details", Json::array()}, {"trace_id", auth.traceId}}}}).body,
                        {{"Retry-After", std::to_string(retryAfter)}, {"X-RateLimit-Limit", std::to_string(limit)}, {"X-RateLimit-Remaining", "0"}}};
  }
  ++bucket.requests;
  if (rateLimits_.size() > 4096) {
    for (auto iterator = rateLimits_.begin(); iterator != rateLimits_.end();) {
      if (now - iterator->second.windowStarted >= std::chrono::hours(1)) iterator = rateLimits_.erase(iterator);
      else ++iterator;
    }
  }
  return std::nullopt;
}

HttpResponse ControlPlane::metrics() const {
  std::ostringstream output;
  output << "# HELP edgefleet_up Whether the control plane is initialized.\n# TYPE edgefleet_up gauge\nedgefleet_up " << (initialized_ ? 1 : 0) << "\n";
  output << "# HELP edgefleet_http_requests_total HTTP requests accepted by the process.\n# TYPE edgefleet_http_requests_total counter\nedgefleet_http_requests_total " << requestCount_.load() << "\n";
  output << "# HELP edgefleet_evidence_append_failures_total Evidence append attempts that failed.\n# TYPE edgefleet_evidence_append_failures_total counter\nedgefleet_evidence_append_failures_total " << evidenceAppendFailures_.load() << "\n";
  output << "# HELP edgefleet_desired_state_requests_total Device desired-state requests.\n# TYPE edgefleet_desired_state_requests_total counter\nedgefleet_desired_state_requests_total " << desiredStateRequests_.load() << "\n";
  output << "# HELP edgefleet_device_reports_total Device reports received.\n# TYPE edgefleet_device_reports_total counter\nedgefleet_device_reports_total " << deviceReportRequests_.load() << "\n";
  if (storage_ == nullptr) return {200, "text/plain; version=0.0.4", output.str(), {}};
  const auto scalar = [this](const std::string& sql) -> long long {
    const auto rows = storage_->query(sql);
    return rows.empty() ? 0LL : rows.front().value("count", 0LL);
  };
  output << "# HELP edgefleet_active_releases Active non-terminal releases.\n# TYPE edgefleet_active_releases gauge\nedgefleet_active_releases " << scalar("SELECT COUNT(*) AS count FROM releases WHERE status IN ('scheduled','running','paused','aborting','rolling_back')") << "\n";
  output << "# HELP edgefleet_devices Devices in active tenants.\n# TYPE edgefleet_devices gauge\nedgefleet_devices " << scalar("SELECT COUNT(*) AS count FROM devices d JOIN tenants t ON t.id=d.tenant_id WHERE t.is_active=1") << "\n";
  output << "# HELP edgefleet_evidence_events_total Hash-chained evidence events.\n# TYPE edgefleet_evidence_events_total counter\nedgefleet_evidence_events_total " << scalar("SELECT COUNT(*) AS count FROM evidence_events") << "\n";
  for (const auto& row : storage_->query("SELECT state,COUNT(*) AS count FROM release_assignments GROUP BY state")) output << "edgefleet_release_assignments{state=\"" << row.value("state", "unknown") << "\"} " << row.value("count", 0LL) << "\n";
  output << "# HELP edgefleet_outbox_rows Outbox rows by adapter and status.\n# TYPE edgefleet_outbox_rows gauge\n";
  for (const auto& row : storage_->query("SELECT adapter_type,status,COUNT(*) AS count FROM outbox_deliveries GROUP BY adapter_type,status")) output << "edgefleet_outbox_rows{adapter=\"" << row.value("adapter_type", "unknown") << "\",status=\"" << row.value("status", "unknown") << "\"} " << row.value("count", 0LL) << "\n";
  output << "# HELP edgefleet_adapter_requests_total Persisted adapter delivery attempts by terminal result.\n# TYPE edgefleet_adapter_requests_total counter\n";
  for (const auto& row : storage_->query("SELECT adapter_type,CASE WHEN status='published' THEN 'published' WHEN status='dead_letter' THEN 'dead_letter' ELSE 'pending' END AS result,SUM(attempt_count) AS count FROM outbox_deliveries GROUP BY adapter_type,result")) output << "edgefleet_adapter_requests_total{adapter=\"" << row.value("adapter_type", "unknown") << "\",result=\"" << row.value("result", "unknown") << "\"} " << row.value("count", 0LL) << "\n";
  output << "# HELP edgefleet_release_stage_decisions_total Immutable gate decisions by result.\n# TYPE edgefleet_release_stage_decisions_total counter\n";
  for (const auto& row : storage_->query("SELECT decision,COUNT(*) AS count FROM health_gate_evaluations GROUP BY decision")) output << "edgefleet_release_stage_decisions_total{decision=\"" << row.value("decision", "unknown") << "\"} " << row.value("count", 0LL) << "\n";
  output << "# HELP edgefleet_device_reports_by_type_total Device reports grouped by type.\n# TYPE edgefleet_device_reports_by_type_total counter\n";
  for (const auto& row : storage_->query("SELECT report_type,COUNT(*) AS count FROM device_reports GROUP BY report_type")) output << "edgefleet_device_reports_by_type_total{type=\"" << row.value("report_type", "unknown") << "\"} " << row.value("count", 0LL) << "\n";
  const auto assignmentTotals = storage_->query("SELECT COUNT(*) AS total,COALESCE(SUM(CASE WHEN state='converged' THEN 1 ELSE 0 END),0) AS converged,COALESCE(SUM(CASE WHEN state IN ('failed','rollback_failed') THEN 1 ELSE 0 END),0) AS unhealthy FROM release_assignments");
  const auto totalAssignments = assignmentTotals.empty() ? 0LL : assignmentTotals.front().value("total", 0LL);
  const auto convergedAssignments = assignmentTotals.empty() ? 0LL : assignmentTotals.front().value("converged", 0LL);
  const auto unhealthyAssignments = assignmentTotals.empty() ? 0LL : assignmentTotals.front().value("unhealthy", 0LL);
  output << "# HELP edgefleet_rollout_convergence_ratio Current assignment convergence ratio.\n# TYPE edgefleet_rollout_convergence_ratio gauge\nedgefleet_rollout_convergence_ratio " << (totalAssignments == 0 ? 1.0 : static_cast<double>(convergedAssignments) / totalAssignments) << "\n";
  output << "# HELP edgefleet_release_unhealthy_exposure_devices Devices with failed assignments.\n# TYPE edgefleet_release_unhealthy_exposure_devices gauge\nedgefleet_release_unhealthy_exposure_devices " << unhealthyAssignments << "\n";
  const auto commandAge = storage_->query("SELECT COALESCE(MAX((julianday('now')-julianday(issued_at))*86400),0) AS age FROM rollout_commands WHERE expires_at > datetime('now')");
  output << "# HELP edgefleet_command_age_seconds Age of the oldest unexpired rollout command.\n# TYPE edgefleet_command_age_seconds gauge\nedgefleet_command_age_seconds " << (commandAge.empty() ? 0.0 : commandAge.front().value("age", 0.0)) << "\n";
  const auto evidenceHead = storage_->query("SELECT COALESCE(MAX(sequence_no),0) AS sequence_no FROM evidence_events");
  output << "# HELP edgefleet_evidence_chain_verified_sequence Highest evidence sequence present for verification.\n# TYPE edgefleet_evidence_chain_verified_sequence gauge\nedgefleet_evidence_chain_verified_sequence " << (evidenceHead.empty() ? 0LL : evidenceHead.front().value("sequence_no", 0LL)) << "\n";
  return {200, "text/plain; version=0.0.4", output.str(), {}};
}

HttpResponse ControlPlane::handle(const HttpRequest& request) {
  ++requestCount_;
  const auto path = pathOnly(request.target);
  if (path == "/health" || path.starts_with("/health/")) return health(request);
  if (path == "/") {
    const auto authenticated = initialized_ && storage_ != nullptr && authenticate(request).context.authenticated;
    return {302, "text/html; charset=utf-8", "", {{"Location", authenticated ? "/app" : "/login"}}};
  }
  if (path == "/static/app.css") {
    std::ifstream css("web/static/app.css", std::ios::binary);
    std::ostringstream content;
    content << css.rdbuf();
    return {200, "text/css; charset=utf-8", css.is_open() ? content.str() : ":root{color-scheme:dark}body{font-family:sans-serif}", {{"Cache-Control", "public, max-age=31536000, immutable"}}};
  }
  if (path == "/static/app.js") {
    std::ifstream javascript("web/static/app.js", std::ios::binary);
    std::ostringstream content;
    content << javascript.rdbuf();
    return {200, "text/javascript; charset=utf-8", javascript.is_open() ? content.str() : "", {{"Cache-Control", "public, max-age=31536000, immutable"}}};
  }
  if (path == "/static/htmx.min.js") {
    std::ifstream htmx("web/vendor/htmx.min.js", std::ios::binary);
    std::ostringstream content;
    content << htmx.rdbuf();
    return {200, "text/javascript; charset=utf-8", htmx.is_open() ? content.str() : "", {{"Cache-Control", "public, max-age=31536000, immutable"}}};
  }
  if (path == "/metrics") return metrics();
  if (path == "/login") { AuthResult anonymous; anonymous.traceId = shared::Uuid::generate().str(); return htmlRoute(request, anonymous); }
  if (!initialized_ || storage_ == nullptr) return errorResponse(503, "NOT_READY", "The control plane has not completed setup.", shared::Uuid::generate().str());
  const auto auth = authenticate(request);
  if (const auto limited = rateLimit(request, auth); limited.has_value()) return *limited;
  if (path.starts_with("/app") && !auth.context.authenticated) return htmlRoute(request, auth);
  const auto sessionToken = cookie(request, "edgefleet_session");
  if (!sessionToken.empty() && isMutation(request.method) && header(request, "x-csrf-token") != shared::DigestService::hmacSha256Hex(config_.sessionEncryptionKey, sessionToken)) return errorResponse(403, "CSRF_REQUIRED", "A valid CSRF token is required for browser mutations.", auth.traceId);
  if (path == "/auth/session" && (request.method == "POST" || request.method == "DELETE")) {
    if (!auth.context.authenticated) return errorResponse(401, "UNAUTHENTICATED", "A valid API key or browser session is required.", auth.traceId);
    if (request.method == "DELETE") {
      if (!sessionToken.empty()) storage_->execute("UPDATE browser_sessions SET revoked_at=datetime('now') WHERE token_hash=?", {shared::DigestService::sha256Hex(sessionToken)});
      return {204, "application/json", "", {{"Set-Cookie", "edgefleet_session=; Max-Age=0; HttpOnly;" + std::string(config_.environment == "production" ? " Secure;" : "") + " SameSite=Strict; Path=/\r\nSet-Cookie: edgefleet_csrf=; Max-Age=0;" + std::string(config_.environment == "production" ? " Secure;" : "") + " SameSite=Strict; Path=/"}}};
    }
    const auto token = shared::DigestService::randomToken(32);
    const auto csrfToken = shared::DigestService::hmacSha256Hex(config_.sessionEncryptionKey, token);
    if (!storage_->execute("INSERT INTO browser_sessions(id,tenant_id,actor_id,token_hash,expires_at,created_at) VALUES(?,?,?, ?,datetime('now','+8 hours'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, auth.context.actorId, shared::DigestService::sha256Hex(token)})) return errorResponse(409, "SESSION_CREATE_FAILED", storage_->lastError(), auth.traceId);
    return {204, "application/json", "", {{"Set-Cookie", "edgefleet_session=" + token + "; Max-Age=28800; HttpOnly;" + std::string(config_.environment == "production" ? " Secure;" : "") + " SameSite=Strict; Path=/\r\nSet-Cookie: edgefleet_csrf=" + csrfToken + "; Max-Age=28800;" + std::string(config_.environment == "production" ? " Secure;" : "") + " SameSite=Strict; Path=/"}, {"X-CSRF-Token", csrfToken}}};
  }
  if ((path == "/api/setup" || path == "/api/tenants/register") && request.method == "POST") {
    if (path == "/api/tenants/register" && !config_.selfRegistrationEnabled) return errorResponse(403, "REGISTRATION_DISABLED", "Tenant self-registration is disabled.", shared::Uuid::generate().str());
    try {
      const auto body = parseBody(request);
      const auto name = body.value("name", config_.defaultTenantName);
      const auto existing = path == "/api/setup" ? storage_->query("SELECT id,name,legal_name,full_legal_name,display_name,default_timezone FROM tenants ORDER BY created_at LIMIT 1") : storage_->query("SELECT id,name,legal_name,full_legal_name,display_name,default_timezone FROM tenants WHERE name=? LIMIT 1", {name});
      if (!existing.empty()) return jsonResponse(200, {{"tenant", existing.front()}, {"idempotent", true}, {"message", "Already initialized; no existing credential is returned."}});
      const auto prefix = "edge_live_" + shared::Uuid::generate().str().substr(0, 24);
      const auto secret = secretFor(prefix);
      const auto credentialHash = shared::DigestService::argon2idHash(secret);
      if (credentialHash.empty()) return errorResponse(500, "CREDENTIAL_HASH_FAILED", "The credential could not be stored safely.", shared::Uuid::generate().str());
      std::optional<Json> tenant;
      const bool created = storage_->transaction([&] {
        tenant = storage_->createTenant(name, body.value("legal_name", config_.defaultTenantLegalName), body.value("display_name", name), body.value("timezone", config_.defaultTenantTimezone), prefix, credentialHash);
        if (!tenant.has_value()) return false;
        if (path == "/api/setup") {
          const auto policy = storage_->createPolicy(tenant->at("id").get<std::string>(), {{"name", "Default fixed-wave safety"}, {"stage_plan", config_.stagePercentages}, {"selector", Json::object()}, {"health_gates", {{"fresh_device_coverage", 0.80}, {"install_failure_rate", 0.01}, {"convergence_rate", 0.98}}}, {"max_offline_fraction", config_.maxOfflineFraction}, {"telemetry_freshness_sec", config_.telemetryFreshnessSeconds}, {"min_observation_sec", config_.minObservationSeconds}, {"two_person_approval", true}, {"require_iot_evidence", false}, {"rollback_requirement", "required"}, {"created_by_actor_id", "system"}});
          if (!policy.has_value() || !storage_->execute("UPDATE rollout_policies SET status='active',updated_at=datetime('now') WHERE tenant_id=? AND id=?", {tenant->at("id").get<std::string>(), policy->at("id").get<std::string>()})) return false;
        }
        if (!storage_->appendEvidence(tenant->at("id").get<std::string>(), "tenant.registered", "tenant", tenant->at("id").get<std::string>(), {{"name", tenant->at("name")}, {"setup", path == "/api/setup"}}, "operator", "bootstrap").has_value()) return false;
        return true;
      });
      if (!created) tenant.reset();
      if (!tenant.has_value()) return errorResponse(409, "TENANT_CREATE_FAILED", storage_->lastError(), shared::Uuid::generate().str());
      return jsonResponse(201, {{"tenant", *tenant}, {"api_key", apiKey(prefix, secret)}, {"warning", "Store this API key now. The secret is not shown again."}});
    } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", shared::Uuid::generate().str()); }
  }
  if (!auth.context.authenticated) return errorResponse(401, "UNAUTHENTICATED", "A valid tenant API key is required.", auth.traceId);
  if (isMutation(request.method) && !path.starts_with("/api/agent/v1/reports")) {
    const auto idempotency = header(request, "idempotency-key");
    if (idempotency.empty()) return errorResponse(428, "IDEMPOTENCY_KEY_REQUIRED", "Mutating requests require Idempotency-Key.", auth.traceId);
    const auto routeKey = request.method + " " + path;
    const auto digest = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize({{"method", request.method}, {"target", request.target}, {"body_digest", requestBodyDigest(request)}, {"body_size", request.bodySize == 0 ? request.body.size() : request.bodySize}}));
    const auto prior = storage_->query("SELECT request_digest,response_status,response_json FROM idempotency_records WHERE tenant_id=? AND actor_id=? AND route_key=? AND idempotency_key=?", {auth.context.tenantId, auth.context.actorId, routeKey, idempotency});
    if (!prior.empty()) {
      if (prior.front().at("request_digest") != digest) return errorResponse(409, "IDEMPOTENCY_KEY_REUSED", "The key was already used for a different request.", auth.traceId);
      return {prior.front().at("response_status").get<int>(), "application/json", prior.front().at("response_json").get<std::string>(), {}};
    }
    HttpResponse response;
    const bool committed = storage_->transaction([&] {
      response = dispatch(request, auth);
      if (response.status >= 500 && response.status != 507) return false;
      if (!storage_->execute("INSERT INTO idempotency_records(id,tenant_id,actor_id,route_key,idempotency_key,request_digest,response_status,response_json,expires_at,created_at) VALUES(?,?,?,?,?,?,?,?,datetime('now','+24 hours'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, auth.context.actorId, routeKey, idempotency, digest, std::to_string(response.status), response.body})) return false;
      if (response.status >= 400) return true;
      if (!storage_->appendEvidence(auth.context.tenantId, "api.mutation.accepted", "http", routeKey, {{"method", request.method}, {"target", path}, {"status", response.status}}, "operator", auth.context.actorId).has_value()) return false;
      const auto event = storage_->query("SELECT id,event_type,payload_json,trace_id FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no DESC LIMIT 1", {auth.context.tenantId});
      const auto tenant = storage_->getTenant(auth.context.tenantId);
      if (!event.empty() && tenant.has_value()) {
        const auto payload = Json{{"tenant", *tenant}, {"event", event.front()}, {"trace_id", auth.traceId}};
        const auto integrations = storage_->query("SELECT adapter_type FROM integration_configs WHERE tenant_id=? AND enabled=1 AND adapter_type IN ('notification_hub_v1','workflow_manual_v1')", {auth.context.tenantId});
        for (const auto& integration : integrations) if (!storage_->execute("INSERT OR IGNORE INTO outbox_deliveries(id,tenant_id,evidence_event_id,adapter_type,status,payload_json,idempotency_key,next_attempt_at,created_at,updated_at) VALUES(?,?,?,?,'pending',?,?,datetime('now'),datetime('now'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, event.front().at("id").get<std::string>(), integration.at("adapter_type").get<std::string>(), payload.dump(), event.front().at("id").get<std::string>() + ":" + integration.at("adapter_type").get<std::string>()})) return false;
      }
      return true;
    });
    return committed ? response : errorResponse(500, "MUTATION_ROLLED_BACK", "The mutation was not committed.", auth.traceId);
  }
  if (isMutation(request.method)) {
    HttpResponse response;
    const bool committed = storage_->transaction([&] {
      response = dispatch(request, auth);
      if (response.status >= 500) return false;
      if (response.status >= 400) return true;
      return storage_->appendEvidence(auth.context.tenantId, "api.mutation.accepted", "http", request.method + " " + path, {{"method", request.method}, {"target", path}, {"status", response.status}}, "operator", auth.context.actorId).has_value();
    });
    return committed ? response : errorResponse(500, "MUTATION_ROLLED_BACK", "The mutation was not committed.", auth.traceId);
  }
  return dispatch(request, auth);
}

HttpResponse ControlPlane::dispatch(const HttpRequest& request, const AuthResult& auth) {
  const auto path = pathOnly(request.target);
  if (path == "/api/agent/v1/desired-state") ++desiredStateRequests_;
  if (path == "/api/agent/v1/reports") ++deviceReportRequests_;
  const auto parts = segments(path);
  const auto traceId = auth.traceId;
  if (path == "/api/tenants/me") {
    if (!web::requireRole(auth.context, "tenant", request.method == "GET" ? "read" : "write")) return errorResponse(403, "FORBIDDEN", "The role cannot access tenant settings.", traceId);
    if (request.method == "GET") return jsonResponse(200, storage_->getTenant(auth.context.tenantId).value_or(Json::object()));
    if (request.method == "PATCH") {
      try { const auto body = parseBody(request); if (!storage_->updateTenant(auth.context.tenantId, body)) return errorResponse(422, "INVALID_TENANT", "legal_name and display_name are required.", traceId); return jsonResponse(200, storage_->getTenant(auth.context.tenantId).value_or(Json::object())); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "fleets") {
    if (parts.size() == 2 && request.method == "GET") {
      if (!web::requireRole(auth.context, "fleets", "read")) return errorResponse(403, "FORBIDDEN", "Fleet read permission is required.", traceId);
      const auto filters = queryParameters(request.target);
      const auto limit = pageLimit(filters);
      const auto cursor = filters.contains("cursor") ? readCursor(config_, filters, filters.at("cursor"), "fleets") : std::nullopt;
      if (cursor.has_value() && cursor->contains("error")) return errorResponse(400, "CURSOR_INVALID", "The cursor is invalid or does not match the request filters.", traceId);
      std::string sql = "SELECT id,tenant_id,name,slug,description,environment,status,version,created_at,updated_at FROM fleets WHERE tenant_id=? AND (?='' OR environment=?) AND (?='' OR status=?)";
      std::vector<std::string> params{auth.context.tenantId, filters.contains("environment") ? filters.at("environment") : "", filters.contains("environment") ? filters.at("environment") : "", filters.contains("status") ? filters.at("status") : "", filters.contains("status") ? filters.at("status") : ""};
      if (cursor.has_value()) { sql += " AND (created_at < ? OR (created_at=? AND id<?))"; params.insert(params.end(), {cursor->at("sort").get<std::string>(), cursor->at("sort").get<std::string>(), cursor->at("id").get<std::string>()}); }
      sql += " ORDER BY created_at DESC,id DESC LIMIT ?";
      params.push_back(std::to_string(limit + 1));
      auto items = storage_->query(sql, params);
      std::string nextCursor;
      if (items.size() > static_cast<std::size_t>(limit)) { const auto last = items[limit - 1]; nextCursor = makeCursor(config_, "fleets", last.at("created_at").get<std::string>(), last.at("id").get<std::string>(), filters); items.resize(limit); }
      return jsonResponse(200, {{"items", items}, {"next_cursor", nextCursor.empty() ? Json(nullptr) : Json(nextCursor)}});
    }
    if (parts.size() == 2 && request.method == "POST") {
      if (!web::requireRole(auth.context, "fleets", "write")) return errorResponse(403, "FORBIDDEN", "Fleet write permission is required.", traceId);
      try { const auto body = parseBody(request); const auto schema = body.value("label_schema", Json::object()); if (!validLabelSchema(schema)) return errorResponse(422, "INVALID_LABEL_SCHEMA", "label_schema must be an object with bounded string keys and string or enum rules.", traceId); const auto fleet = storage_->createFleet(auth.context.tenantId, body.value("slug", "fleet-" + shared::Uuid::generate().str().substr(0, 8)), body.value("name", "Fleet"), body.value("environment", "development"), schema); if (!fleet.has_value()) return errorResponse(409, "FLEET_CREATE_FAILED", storage_->lastError(), traceId); return jsonResponse(201, *fleet); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    if (parts.size() == 3) {
      const auto fleet = storage_->getFleet(auth.context.tenantId, parts[2]);
      if (!fleet.has_value()) return errorResponse(404, "NOT_FOUND", "Fleet not found.", traceId);
      if (request.method == "GET") { if (!web::requireRole(auth.context, "fleets", "read")) return errorResponse(403, "FORBIDDEN", "Fleet read permission is required.", traceId); return jsonResponse(200, *fleet); }
      if (request.method == "PATCH") {
        if (!web::requireRole(auth.context, "fleets", "write")) return errorResponse(403, "FORBIDDEN", "Fleet write permission is required.", traceId);
        try { const auto body = parseBody(request); const auto current = storage_->getFleet(auth.context.tenantId, parts[2]); if (!current.has_value()) return errorResponse(404, "NOT_FOUND", "Fleet not found.", traceId); if (!body.contains("expected_version") || !body.at("expected_version").is_number_integer()) return errorResponse(422, "EXPECTED_VERSION_REQUIRED", "Fleet updates require expected_version.", traceId); const auto expectedVersion = body.at("expected_version").get<int>(); const bool changed = storage_->execute("UPDATE fleets SET name=COALESCE(NULLIF(?,''),name),description=COALESCE(NULLIF(?,''),description),environment=COALESCE(NULLIF(?,''),environment),version=version+1,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND version=?", {body.value("name", ""), body.value("description", ""), body.value("environment", ""), auth.context.tenantId, parts[2], std::to_string(expectedVersion)}); return changed ? jsonResponse(200, storage_->getFleet(auth.context.tenantId, parts[2]).value_or(Json::object())) : errorResponse(409, "FLEET_VERSION_CONFLICT", "The fleet changed after this control was loaded.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      }
      if (request.method == "DELETE") return errorResponse(405, "METHOD_NOT_ALLOWED", "Use the reason-bearing retire action for fleet lifecycle changes.", traceId);
    }
    if (parts.size() == 4 && (parts[3] == "pause" || parts[3] == "retire") && request.method == "POST") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can pause or retire fleets.", traceId);
      try { if (!hasReason(parseBody(request))) return errorResponse(422, "REASON_REQUIRED", "A fleet lifecycle action requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      const auto status = parts[3] == "pause" ? "paused" : "retired";
      const auto fleet = storage_->getFleet(auth.context.tenantId, parts[2]);
      if (!fleet.has_value()) return errorResponse(404, "NOT_FOUND", "Fleet not found.", traceId);
      Json body;
      try { body = parseBody(request); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      const bool changed = storage_->transaction([&] {
        if (!storage_->execute("UPDATE fleets SET status=?,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status<>?", {status, auth.context.tenantId, parts[2], status})) return false;
        return storage_->appendEvidence(auth.context.tenantId, "fleet." + std::string(parts[3]) + "d", "fleet", parts[2], {{"from", fleet->value("status", "")}, {"to", status}, {"reason", body.value("reason", "")}}, "operator", auth.context.actorId).has_value();
      });
      return changed ? jsonResponse(200, {{"id", parts[2]}, {"status", status}}) : errorResponse(409, "FLEET_UPDATE_FAILED", "The fleet lifecycle change could not be committed.", traceId);
    }
    if (parts.size() == 4 && parts[3] == "devices" && request.method == "GET") { if (!web::requireRole(auth.context, "devices", "read")) return errorResponse(403, "FORBIDDEN", "Device read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->listDevices(auth.context.tenantId, parts[2])}}); }
    if (parts.size() == 4 && parts[3] == "devices" && request.method == "POST") {
      if (!web::requireRole(auth.context, "devices", "write")) return errorResponse(403, "FORBIDDEN", "Device write permission is required.", traceId);
      try { const auto body = parseBody(request); const auto fleet = storage_->getFleet(auth.context.tenantId, parts[2]); if (!fleet.has_value()) return errorResponse(404, "NOT_FOUND", "Fleet not found.", traceId); const auto labels = body.value("labels", Json::object()); const auto schema = Json::parse(fleet->value("label_schema_json", "{}")); if (!validLabels(labels)) return errorResponse(422, "INVALID_LABELS", "Device labels must be a string-valued object with at most 50 entries.", traceId); if (!labelsFitSchema(labels, schema)) return errorResponse(422, "LABEL_SCHEMA_MISMATCH", "Device labels do not match the fleet label schema.", traceId); if (body.value("stable_key", "").empty() || body.value("hardware_model", "").empty() || body.value("architecture", "").empty()) return errorResponse(422, "INVALID_DEVICE", "stable_key, hardware_model, and architecture are required.", traceId); const auto secret = body.value("device_secret", secretFor("device")); const auto device = storage_->createDevice(auth.context.tenantId, parts[2], body, shared::DigestService::sha256Hex(secret)); if (!device.has_value()) return errorResponse(409, "DEVICE_CREATE_FAILED", storage_->lastError(), traceId); auto result = *device; result["device_secret"] = secret; return jsonResponse(201, result); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && (parts[1] == "operator-credentials" || parts[1] == "credentials")) {
    if (parts.size() == 2 && request.method == "GET") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only tenant administrators can inspect credentials.", traceId);
      return jsonResponse(200, {{"items", storage_->query("SELECT id,label,role,key_prefix,last_used_at,expires_at,revoked_at,created_at FROM operator_credentials WHERE tenant_id=? ORDER BY created_at DESC", {auth.context.tenantId})}});
    }
    if (parts.size() == 2 && request.method == "POST") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only tenant administrators can create credentials.", traceId);
      try {
        const auto body = parseBody(request);
        const auto role = body.value("role", "viewer");
        if (!shared::roleFromString(role).has_value() || role == "device") return errorResponse(422, "INVALID_ROLE", "Operator credentials require an operator role.", traceId);
        const auto prefix = "edge_live_" + shared::Uuid::generate().str().substr(0, 24);
        const auto secret = secretFor(prefix);
        const auto credentialHash = shared::DigestService::argon2idHash(secret);
        const bool created = !credentialHash.empty() && storage_->execute("INSERT INTO operator_credentials(id,tenant_id,label,role,key_prefix,key_hash,expires_at,created_at,updated_at) VALUES(?,?,?,?,?,?,NULLIF(?,'') ,datetime('now'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, body.value("label", role), role, prefix, credentialHash, body.value("expires_at", "")});
        if (!created) return errorResponse(409, "CREDENTIAL_CREATE_FAILED", storage_->lastError(), traceId);
        return jsonResponse(201, {{"label", body.value("label", role)}, {"role", role}, {"api_key", apiKey(prefix, secret)}, {"warning", "Store this API key now. The secret is not shown again."}});
      } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    if (parts.size() == 4 && request.method == "POST" && (parts[3] == "revoke" || parts[3] == "rotate")) {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only tenant administrators can change credentials.", traceId);
      if (parts[3] == "revoke") {
        try { if (!hasReason(parseBody(request))) return errorResponse(422, "REASON_REQUIRED", "Credential revocation requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
        return storage_->execute("UPDATE operator_credentials SET revoked_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND revoked_at IS NULL", {auth.context.tenantId, parts[2]}) ? jsonResponse(200, {{"id", parts[2]}, {"status", "revoked"}}) : errorResponse(404, "NOT_FOUND", "Credential not found.", traceId);
      }
      const auto prefix = "operator-" + shared::Uuid::generate().str().substr(0, 12);
      const auto secret = secretFor(prefix);
        const auto credentialHash = shared::DigestService::argon2idHash(secret);
        const bool rotated = !credentialHash.empty() && storage_->execute("INSERT INTO operator_credentials(id,tenant_id,label,role,key_prefix,key_hash,created_at,updated_at) SELECT ?,tenant_id,label,role,?,?,datetime('now'),datetime('now') FROM operator_credentials WHERE tenant_id=? AND id=?", {shared::Uuid::generate().str(), prefix, credentialHash, auth.context.tenantId, parts[2]});
      return rotated ? jsonResponse(201, {{"api_key", apiKey(prefix, secret)}, {"supersedes", parts[2]}, {"warning", "Store this API key now. The secret is not shown again."}}) : errorResponse(404, "NOT_FOUND", "Credential not found.", traceId);
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "artifact-signing-keys") {
    if (parts.size() == 2 && request.method == "GET") {
      if (!web::requireRole(auth.context, "artifacts", "read")) return errorResponse(403, "FORBIDDEN", "Artifact key read permission is required.", traceId);
      return jsonResponse(200, {{"items", storage_->query("SELECT id,name,algorithm,fingerprint_sha256,status,created_at,retired_at FROM artifact_signing_keys WHERE tenant_id=? ORDER BY created_at DESC", {auth.context.tenantId})}});
    }
    if (parts.size() == 2 && request.method == "POST") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can register artifact signing keys.", traceId);
      const auto key = domain::ArtifactSigner::generateKeyPair();
      if (!key.ok()) return errorResponse(key.error->status, key.error->code, key.error->message, traceId);
      const auto id = shared::Uuid::generate().str();
      if (!storage_->execute("INSERT INTO artifact_signing_keys(id,tenant_id,name,algorithm,public_key_pem,fingerprint_sha256,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,?,?,?,datetime('now'),datetime('now'))", {id, auth.context.tenantId, "key-" + id.substr(0, 8), "ed25519", key.value->publicKeyPem, key.value->fingerprintSha256, auth.context.actorId})) return errorResponse(409, "KEY_CREATE_FAILED", storage_->lastError(), traceId);
      return jsonResponse(201, {{"id", id}, {"algorithm", "ed25519"}, {"fingerprint_sha256", key.value->fingerprintSha256}, {"public_key_pem", key.value->publicKeyPem}, {"private_key_pem", key.value->privateKeyPem}, {"warning", "Store the private key outside the control plane. It is shown once."}});
    }
    if (parts.size() == 4 && parts[3] == "retire" && request.method == "POST") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can retire signing keys.", traceId);
      try { if (!hasReason(parseBody(request))) return errorResponse(422, "REASON_REQUIRED", "Signing-key retirement requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      const bool retired = storage_->execute("UPDATE artifact_signing_keys SET status='retired',retired_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='active'", {auth.context.tenantId, parts[2]});
      return retired ? jsonResponse(200, {{"id", parts[2]}, {"status", "retired"}}) : errorResponse(404, "NOT_FOUND", "Active signing key not found.", traceId);
    }
    if (parts.size() == 4 && parts[3] == "compromise" && request.method == "POST") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can compromise signing keys.", traceId);
      Json body;
      try { body = parseBody(request); if (!hasReason(body)) return errorResponse(422, "REASON_REQUIRED", "Signing-key compromise requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      std::size_t affectedArtifacts = 0;
      const bool compromised = storage_->transaction([&] {
        if (!storage_->execute("UPDATE artifact_signing_keys SET status='compromised',updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='active'", {auth.context.tenantId, parts[2]})) return false;
        const auto artifacts = storage_->query("SELECT id FROM artifacts WHERE tenant_id=? AND signature_key_id=?", {auth.context.tenantId, parts[2]});
        affectedArtifacts = artifacts.size();
        if (!storage_->execute("UPDATE artifacts SET status='blocked',validation_error='SIGNING_KEY_COMPROMISED',updated_at=datetime('now') WHERE tenant_id=? AND signature_key_id=? AND status IN ('uploading','validating','ready')", {auth.context.tenantId, parts[2]})) return false;
        const auto releases = storage_->query("SELECT DISTINCT r.id,r.status,r.version FROM releases r JOIN artifacts a ON a.tenant_id=r.tenant_id AND (a.id=r.artifact_id OR a.id=r.rollback_artifact_id) WHERE r.tenant_id=? AND a.signature_key_id=? AND r.status IN ('ready','awaiting_approval','scheduled','running','paused')", {auth.context.tenantId, parts[2]});
        for (const auto& release : releases) {
          const auto currentState = domain::releaseStateFromString(release.value("status", ""));
          const auto action = release.value("status", "") == "running" ? domain::ReleaseAction::pause : domain::ReleaseAction::security_block;
          if (!currentState.has_value() || !domain::ReleaseStateMachine::transition(*currentState, action).has_value()) return false;
          const auto next = release.value("status", "") == "running" ? "paused" : "blocked";
          if (!storage_->updateRelease(auth.context.tenantId, release.at("id").get<std::string>(), next, release.at("version").get<int>()) || !storage_->execute("UPDATE releases SET status_reason_code='SIGNING_KEY_COMPROMISED',status_reason_text=?,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {body.value("reason", ""), auth.context.tenantId, release.at("id").get<std::string>()})) return false;
        }
        return storage_->appendEvidence(auth.context.tenantId, "artifact_signing_key.compromised", "artifact_signing_key", parts[2], {{"reason", body.value("reason", "")}, {"affected_artifacts", affectedArtifacts}, {"affected_releases", releases.size()}}, "operator", auth.context.actorId).has_value();
      });
      if (!compromised) return errorResponse(404, "NOT_FOUND", "Active signing key not found or compromise could not be committed.", traceId);
      return jsonResponse(200, {{"id", parts[2]}, {"status", "compromised"}, {"affected_artifacts", affectedArtifacts}});
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "artifacts") {
    if (parts.size() == 2 && request.method == "GET") {
      if (!web::requireRole(auth.context, "artifacts", "read")) return errorResponse(403, "FORBIDDEN", "Artifact read permission is required.", traceId);
      const auto filters = queryParameters(request.target);
      const auto limit = pageLimit(filters);
      const auto cursor = filters.contains("cursor") ? readCursor(config_, filters, filters.at("cursor"), "artifacts") : std::nullopt;
      if (cursor.has_value() && cursor->contains("error")) return errorResponse(400, "CURSOR_INVALID", "The cursor is invalid or does not match the request filters.", traceId);
      const auto hardware = filters.contains("hardware_model") ? filters.at("hardware_model") : "";
      const auto architecture = filters.contains("architecture") ? filters.at("architecture") : "";
      const auto status = filters.contains("status") ? filters.at("status") : "";
      std::string sql = "SELECT id,name,version,hardware_model,architecture,status,file_name,size_bytes,sha256_digest,signature_key_id,validation_error,created_at,updated_at FROM artifacts WHERE tenant_id=? AND (?='' OR hardware_model=?) AND (?='' OR architecture=?) AND (?='' OR status=?)";
      std::vector<std::string> params{auth.context.tenantId, hardware, hardware, architecture, architecture, status, status};
      if (cursor.has_value()) { sql += " AND (created_at < ? OR (created_at=? AND id<?))"; params.insert(params.end(), {cursor->at("sort").get<std::string>(), cursor->at("sort").get<std::string>(), cursor->at("id").get<std::string>()}); }
      sql += " ORDER BY created_at DESC,id DESC LIMIT ?";
      params.push_back(std::to_string(limit + 1));
      auto items = storage_->query(sql, params);
      std::string nextCursor;
      if (items.size() > static_cast<std::size_t>(limit)) { const auto last = items[limit - 1]; nextCursor = makeCursor(config_, "artifacts", last.at("created_at").get<std::string>(), last.at("id").get<std::string>(), filters); items.resize(limit); }
      return jsonResponse(200, {{"items", items}, {"next_cursor", nextCursor.empty() ? Json(nullptr) : Json(nextCursor)}});
    }
    if (parts.size() == 2 && request.method == "POST") {
      if (!web::requireRole(auth.context, "artifacts", "write")) return errorResponse(403, "FORBIDDEN", "Artifact write permission is required.", traceId);
      try {
        Json body = Json::object();
        if (isJsonObjectBody(request)) body = parseBody(request);
        else {
          body["name"] = header(request, "x-artifact-name");
          body["version"] = header(request, "x-artifact-version");
          body["hardware_model"] = header(request, "x-artifact-hardware-model");
          body["architecture"] = header(request, "x-artifact-architecture");
          body["file_name"] = header(request, "x-artifact-file-name");
          body["signing_key_id"] = header(request, "x-artifact-signing-key-id");
          body["signature"] = header(request, "x-artifact-signature");
          body["manifest"] = Json::parse(header(request, "x-artifact-manifest"));
        }
        const auto keyId = body.value("signing_key_id", "");
        const auto keys = storage_->query("SELECT public_key_pem,status FROM artifact_signing_keys WHERE tenant_id=? AND id=?", {auth.context.tenantId, keyId});
        if (keys.empty()) return errorResponse(422, "SIGNING_KEY_REQUIRED", "A tenant signing key is required.", traceId);
        if (keys.front().value("status", "") != "active") return errorResponse(422, "SIGNING_KEY_NOT_ACTIVE", "Only active signing keys can validate new artifacts.", traceId);
        const auto manifest = body.value("manifest", Json::object());
        if (!manifest.is_object()) return errorResponse(422, "INVALID_ARTIFACT_MANIFEST", "The artifact manifest must be a JSON object.", traceId);
        const auto name = body.value("name", "");
        const auto version = body.value("version", "");
        const auto hardwareModel = body.value("hardware_model", "");
        const auto architecture = body.value("architecture", "");
        if (name.empty() || version.empty() || hardwareModel.empty() || architecture.empty()) return errorResponse(422, "INVALID_ARTIFACT_METADATA", "name, version, hardware_model, and architecture are required.", traceId);
        const auto manifestJson = shared::CanonicalJson::serialize(manifest);
        const auto signature = body.value("signature", "");
        const bool uploadedAsFile = !request.bodyFilePath.empty();
        const auto content = uploadedAsFile ? std::string{} : body.value("content", manifestJson);
        const auto contentSize = uploadedAsFile ? std::filesystem::file_size(request.bodyFilePath) : static_cast<std::uintmax_t>(content.size());
        const auto digest = uploadedAsFile ? shared::DigestService::sha256File(request.bodyFilePath) : shared::DigestService::sha256Hex(content);
        if (digest.empty()) return errorResponse(422, "ARTIFACT_BYTES_UNAVAILABLE", "The uploaded artifact bytes could not be read safely.", traceId);
        if (body.contains("sha256_digest") && body.at("sha256_digest").is_string() && body.at("sha256_digest").get<std::string>() != digest) return errorResponse(422, "ARTIFACT_DIGEST_MISMATCH", "The supplied artifact digest does not match its bytes.", traceId);
        if (body.contains("size_bytes") && body.at("size_bytes").is_number_unsigned() && body.at("size_bytes").get<std::uintmax_t>() != contentSize) return errorResponse(422, "ARTIFACT_SIZE_MISMATCH", "The supplied artifact size does not match its bytes.", traceId);
        if (contentSize > static_cast<std::uintmax_t>(config_.artifactMaxBytes)) return errorResponse(413, "ARTIFACT_TOO_LARGE", "The artifact exceeds the configured size limit.", traceId);
        const auto signedPayload = shared::CanonicalJson::serialize({{"digest", digest}, {"size_bytes", contentSize}, {"name", name}, {"version", version}, {"hardware_model", hardwareModel}, {"architecture", architecture}, {"manifest", manifest}});
        if (!domain::ArtifactSigner::verify(signedPayload, signature, keys.front().at("public_key_pem").get<std::string>())) return errorResponse(422, "INVALID_ARTIFACT_SIGNATURE", "The artifact metadata and manifest signature is invalid.", traceId);
        const auto destination = artifactPath(config_, auth.context.tenantId, digest);
        std::error_code filesystemError;
        std::filesystem::create_directories(destination.parent_path(), filesystemError);
        if (filesystemError) return errorResponse(507, "ARTIFACT_STORE_UNAVAILABLE", "The artifact store is unavailable.", traceId);
        const auto space = std::filesystem::space(destination.parent_path(), filesystemError);
        if (filesystemError || space.available < static_cast<std::uintmax_t>(config_.artifactMinFreeBytes)) return errorResponse(507, "ARTIFACT_STORE_LOW_SPACE", "The artifact store does not have the configured free-space reserve.", traceId);
        if (!std::filesystem::exists(destination)) {
          const auto temporary = std::filesystem::path(config_.artifactTempPath) / auth.context.tenantId / shared::Uuid::generate().str();
          std::filesystem::create_directories(temporary.parent_path(), filesystemError);
          std::ofstream output(temporary, std::ios::binary);
          if (uploadedAsFile) {
            std::ifstream input(request.bodyFilePath, std::ios::binary);
            std::vector<char> chunk(1024 * 1024);
            while (input) {
              input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
              const auto count = input.gcount();
              if (count > 0) output.write(chunk.data(), count);
            }
            if (!input.eof()) output.setstate(std::ios::failbit);
          } else {
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
          }
          output.close();
          if (!output || filesystemError) return errorResponse(507, "ARTIFACT_STORE_UNAVAILABLE", "The artifact could not be written safely.", traceId);
          std::filesystem::rename(temporary, destination, filesystemError);
          if (filesystemError) { std::filesystem::remove(temporary); return errorResponse(409, "ARTIFACT_DEDUPLICATED", "An artifact with the same digest was stored concurrently.", traceId); }
        }
        const auto id = shared::Uuid::generate().str();
        const bool created = storage_->execute("INSERT INTO artifacts(id,tenant_id,name,version,hardware_model,architecture,status,file_name,storage_key,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_by_actor_id,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,datetime('now'),datetime('now'))", {id, auth.context.tenantId, name, version, hardwareModel, architecture, "ready", safeFileName(body.value("file_name", "manifest.json")), destination.string(), std::to_string(contentSize), digest, manifestJson, signature, keyId, auth.context.actorId});
        return created ? jsonResponse(201, storage_->query("SELECT id,name,version,hardware_model,architecture,status,file_name,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_at,updated_at FROM artifacts WHERE tenant_id=? AND id=?", {auth.context.tenantId, id}).front()) : errorResponse(409, "ARTIFACT_CREATE_FAILED", storage_->lastError(), traceId);
      } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    if (parts.size() == 3 && request.method == "GET") {
      if (!web::requireRole(auth.context, "artifacts", "read")) return errorResponse(403, "FORBIDDEN", "Artifact read permission is required.", traceId);
      const auto artifact = storage_->query("SELECT id,name,version,hardware_model,architecture,status,file_name,size_bytes,sha256_digest,manifest_json,signature,signature_key_id,created_at,updated_at FROM artifacts WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[2]});
      if (artifact.empty()) return errorResponse(404, "NOT_FOUND", "Artifact not found.", traceId);
      auto result = artifact.front();
      result["references"] = storage_->query("SELECT id,name,status,version FROM releases WHERE tenant_id=? AND (artifact_id=? OR rollback_artifact_id=?) ORDER BY created_at DESC", {auth.context.tenantId, parts[2], parts[2]});
      return jsonResponse(200, result);
    }
    if (parts.size() == 4 && parts[3] == "validate" && request.method == "POST") {
      if (!web::requireRole(auth.context, "artifacts", "write")) return errorResponse(403, "FORBIDDEN", "Artifact validation permission is required.", traceId);
      const auto artifact = storage_->query("SELECT id,storage_key,manifest_json,signature,signature_key_id,sha256_digest,status FROM artifacts WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[2]});
      if (artifact.empty()) return errorResponse(404, "NOT_FOUND", "Artifact not found.", traceId);
      const auto key = storage_->query("SELECT public_key_pem,status FROM artifact_signing_keys WHERE tenant_id=? AND id=?", {auth.context.tenantId, artifact.front().at("signature_key_id").get<std::string>()});
      std::ifstream input(artifact.front().at("storage_key").get<std::string>(), std::ios::binary);
      const auto keyStatus = key.empty() ? std::string{} : key.front().value("status", std::string{});
      const bool valid = input.is_open() && !key.empty() && keyStatus != "compromised" && shared::DigestService::constantTimeEqual(shared::DigestService::sha256File(artifact.front().at("storage_key").get<std::string>()), artifact.front().at("sha256_digest").get<std::string>()) && domain::ArtifactSigner::verify(artifact.front().at("manifest_json").get<std::string>(), artifact.front().at("signature").get<std::string>(), key.front().at("public_key_pem").get<std::string>());
      if (!storage_->execute("UPDATE artifacts SET status=?,validation_error=?,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {valid ? "ready" : "blocked", valid ? "" : "ARTIFACT_VALIDATION_FAILED", auth.context.tenantId, parts[2]})) return errorResponse(409, "ARTIFACT_VALIDATION_FAILED", "The artifact validation result could not be recorded.", traceId);
      return jsonResponse(valid ? 200 : 422, {{"id", parts[2]}, {"status", valid ? "ready" : "blocked"}, {"digest", artifact.front().at("sha256_digest")}});
    }
    if (parts.size() == 4 && parts[3] == "retire" && request.method == "POST") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can retire artifacts.", traceId);
      try { if (!hasReason(parseBody(request))) return errorResponse(422, "REASON_REQUIRED", "Artifact retirement requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      return storage_->execute("UPDATE artifacts SET status='retired',updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='ready'", {auth.context.tenantId, parts[2]}) ? jsonResponse(200, {{"id", parts[2]}, {"status", "retired"}}) : errorResponse(404, "NOT_FOUND", "Ready artifact not found.", traceId);
    }
    if (parts.size() == 4 && parts[3] == "download" && request.method == "GET") {
      if (!web::requireRole(auth.context, "artifacts", "read")) return errorResponse(403, "FORBIDDEN", "Artifact download permission is required.", traceId);
      const auto artifact = storage_->query("SELECT storage_key,sha256_digest,status,file_name FROM artifacts WHERE tenant_id=? AND id=? AND status IN ('ready','retired')", {auth.context.tenantId, parts[2]});
      if (artifact.empty()) return errorResponse(404, "NOT_FOUND", "Downloadable artifact not found.", traceId);
      std::ifstream input(artifact.front().at("storage_key").get<std::string>(), std::ios::binary);
      if (!input.is_open()) return errorResponse(503, "ARTIFACT_UNAVAILABLE", "The artifact bytes are temporarily unavailable.", traceId);
      if (!shared::DigestService::constantTimeEqual(shared::DigestService::sha256File(artifact.front().at("storage_key").get<std::string>()), artifact.front().at("sha256_digest").get<std::string>())) return errorResponse(503, "ARTIFACT_CORRUPT", "The artifact failed its integrity check.", traceId);
      std::error_code fileError;
      const auto fileSize = std::filesystem::file_size(artifact.front().at("storage_key").get<std::string>(), fileError);
      if (fileError) return errorResponse(503, "ARTIFACT_UNAVAILABLE", "The artifact bytes are temporarily unavailable.", traceId);
      std::uintmax_t start = 0;
      std::uintmax_t end = fileSize == 0 ? 0 : fileSize - 1;
      int status = 200;
      std::map<std::string, std::string> responseHeaders{{"ETag", "\"" + artifact.front().at("sha256_digest").get<std::string>() + "\""}, {"X-Artifact-SHA256", artifact.front().at("sha256_digest").get<std::string>()}, {"Content-Disposition", "attachment; filename=\"" + artifact.front().at("file_name").get<std::string>() + "\""}, {"Accept-Ranges", "bytes"}};
      const auto range = header(request, "range");
      if (range.starts_with("bytes=")) {
        try {
          if (fileSize == 0 || range.find(',', 6) != std::string::npos) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
          const auto dash = range.find('-', 6);
          if (dash == std::string::npos) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
          const auto startText = range.substr(6, dash - 6);
          const auto endText = range.substr(dash + 1);
          if (startText.empty()) {
            const auto suffix = std::stoull(endText);
            if (suffix == 0) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
            start = suffix >= fileSize ? 0 : fileSize - suffix;
          } else {
            start = std::stoull(startText);
            if (!endText.empty()) end = std::stoull(endText);
          }
          if (start >= fileSize || end < start) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
          const auto boundedEnd = std::min<std::uintmax_t>(end, fileSize - 1);
          end = boundedEnd;
          status = 206;
          responseHeaders["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(boundedEnd) + "/" + std::to_string(fileSize);
        } catch (const std::exception&) { return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId); }
      }
      return {status, "application/octet-stream", {}, responseHeaders, artifact.front().at("storage_key").get<std::string>(), start, fileSize == 0 ? 0 : end - start + 1};
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "devices") {
    if (parts.size() == 2 && request.method == "GET") {
      if (!web::requireRole(auth.context, "devices", "read")) return errorResponse(403, "FORBIDDEN", "Device read permission is required.", traceId);
      const auto filters = queryParameters(request.target);
      const auto fleet = filters.contains("fleet_id") ? filters.at("fleet_id") : "";
      const auto lifecycle = filters.contains("lifecycle_status") ? filters.at("lifecycle_status") : filters.contains("status") ? filters.at("status") : "";
      const auto limit = pageLimit(filters);
      const auto cursor = filters.contains("cursor") ? readCursor(config_, filters, filters.at("cursor"), "devices") : std::nullopt;
      if (cursor.has_value() && cursor->contains("error")) return errorResponse(400, "CURSOR_INVALID", "The cursor is invalid or does not match the request filters.", traceId);
      std::string sql = "SELECT id,tenant_id,fleet_id,stable_key,display_name,hardware_model,architecture,lifecycle_status,desired_generation,observed_generation,observed_artifact_digest,last_report_sequence,last_seen_at,created_at FROM devices WHERE tenant_id=? AND (?='' OR fleet_id=?) AND (?='' OR lifecycle_status=?)";
      std::vector<std::string> params{auth.context.tenantId, fleet, fleet, lifecycle, lifecycle};
      if (cursor.has_value()) { sql += " AND (created_at < ? OR (created_at=? AND id<?))"; params.insert(params.end(), {cursor->at("sort").get<std::string>(), cursor->at("sort").get<std::string>(), cursor->at("id").get<std::string>()}); }
      sql += " ORDER BY created_at DESC,id DESC LIMIT ?";
      params.push_back(std::to_string(limit + 1));
      auto items = storage_->query(sql, params);
      std::string nextCursor;
      if (items.size() > static_cast<std::size_t>(limit)) { const auto last = items[limit - 1]; nextCursor = makeCursor(config_, "devices", last.at("created_at").get<std::string>(), last.at("id").get<std::string>(), filters); items.resize(limit); }
      return jsonResponse(200, {{"items", items}, {"next_cursor", nextCursor.empty() ? Json(nullptr) : Json(nextCursor)}});
    }
    if (parts.size() == 3 && request.method == "GET") { if (!web::requireRole(auth.context, "devices", "read")) return errorResponse(403, "FORBIDDEN", "Device read permission is required.", traceId); const auto device = storage_->getDevice(auth.context.tenantId, parts[2]); if (!device.has_value()) return errorResponse(404, "NOT_FOUND", "Device not found.", traceId); auto result = *device; result["reports"] = storage_->query("SELECT report_id,report_sequence,report_type,observed_generation,observed_artifact_digest,health_json,result_code,server_received_at FROM device_reports WHERE tenant_id=? AND device_id=? ORDER BY report_sequence DESC LIMIT 50", {auth.context.tenantId, parts[2]}); result["assignments"] = storage_->query("SELECT id,release_id,desired_artifact_id,desired_generation,state,latest_report_sequence,updated_at FROM release_assignments WHERE tenant_id=? AND device_id=? ORDER BY updated_at DESC LIMIT 50", {auth.context.tenantId, parts[2]}); return jsonResponse(200, result); }
    if (parts.size() == 3 && request.method == "PATCH") {
      if (!web::requireRole(auth.context, "devices", "write")) return errorResponse(403, "FORBIDDEN", "Device write permission is required.", traceId);
      try { const auto body = parseBody(request); if (body.contains("labels") && !validLabels(body.at("labels"))) return errorResponse(422, "INVALID_LABELS", "Device labels must be a string-valued object with at most 50 entries.", traceId); const bool changed = storage_->execute("UPDATE devices SET display_name=COALESCE(NULLIF(?,''),display_name),labels_json=CASE WHEN ?='{}' THEN labels_json ELSE ? END,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {body.value("display_name", ""), shared::CanonicalJson::serialize(body.value("labels", Json::object())), shared::CanonicalJson::serialize(body.value("labels", Json::object())), auth.context.tenantId, parts[2]}); return changed ? jsonResponse(200, storage_->getDevice(auth.context.tenantId, parts[2]).value_or(Json::object())) : errorResponse(404, "NOT_FOUND", "Device not found.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    if (parts.size() == 4 && request.method == "POST" && (parts[3] == "quarantine" || parts[3] == "reactivate" || parts[3] == "decommission")) {
      if (parts[3] == "decommission" ? !web::requireRole(auth.context, "tenant", "write") : !web::requireRole(auth.context, "devices", "write")) return errorResponse(403, "FORBIDDEN", "The role cannot change this device state.", traceId);
      try { if (!hasReason(parseBody(request))) return errorResponse(422, "REASON_REQUIRED", "A device lifecycle action requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      const auto status = parts[3] == "quarantine" ? "quarantined" : parts[3] == "reactivate" ? "active" : "decommissioned";
      return storage_->execute("UPDATE devices SET lifecycle_status=?,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {status, auth.context.tenantId, parts[2]}) ? jsonResponse(200, {{"id", parts[2]}, {"lifecycle_status", status}}) : errorResponse(404, "NOT_FOUND", "Device not found.", traceId);
    }
    if (parts.size() == 5 && parts[3] == "credentials" && parts[4] == "rotate" && request.method == "POST") {
      if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can rotate device credentials.", traceId);
      const auto secret = secretFor("device");
      const auto currentDevice = storage_->getDevice(auth.context.tenantId, parts[2]);
      if (!currentDevice.has_value()) return errorResponse(404, "NOT_FOUND", "Device not found.", traceId);
      const auto nextVersion = currentDevice->value("device_key_version", 1) + 1;
      const auto credentialId = shared::Uuid::generate().str();
      const auto encryptedSecret = shared::DigestService::encryptSecret(config_.credentialEncryptionKey, secret);
      if (encryptedSecret.empty()) return errorResponse(500, "CREDENTIAL_ENCRYPTION_FAILED", "The successor credential could not be encrypted.", traceId);
      const auto currentCredentials = storage_->query("SELECT id FROM device_credentials WHERE tenant_id=? AND device_id=? AND key_version=? AND revoked_at IS NULL", {auth.context.tenantId, parts[2], std::to_string(currentDevice->value("device_key_version", 1))});
      const auto currentCredentialId = currentCredentials.empty() ? shared::Uuid::generate().str() : currentCredentials.front().at("id").get<std::string>();
      const bool rotated = storage_->transaction([&] {
        const bool storedCurrent = !currentCredentials.empty() || storage_->execute("INSERT INTO device_credentials(id,tenant_id,device_id,key_version,secret_ciphertext,activated_at,expires_at,created_at) VALUES(?,?,?,?,?,datetime('now'),datetime('now','+24 hours'),datetime('now'))", {currentCredentialId, auth.context.tenantId, parts[2], std::to_string(currentDevice->value("device_key_version", 1)), currentDevice->value("device_secret_hash", std::string{})});
        return storedCurrent && storage_->execute("INSERT INTO device_credentials(id,tenant_id,device_id,key_version,secret_ciphertext,supersedes_credential_id,activated_at,created_at) VALUES(?,?,?,?,?,?,datetime('now'),datetime('now'))", {credentialId, auth.context.tenantId, parts[2], std::to_string(nextVersion), encryptedSecret, currentCredentialId});
      });
      return rotated ? jsonResponse(202, {{"device_id", parts[2]}, {"device_secret", secret}, {"warning", "Store this provisioning secret now."}}) : errorResponse(404, "NOT_FOUND", "Device not found.", traceId);
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "policies") {
    if (parts.size() == 3 && request.method == "PATCH") {
      try {
        const auto body = parseBody(request);
        if (const auto error = policyValidationError(body); error.has_value()) return errorResponse(422, "INVALID_POLICY", "The policy contains unsupported or invalid safety configuration.", traceId, {{"reason", *error}});
      } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    if (parts.size() == 2 && request.method == "GET") { if (!web::requireRole(auth.context, "policies", "read")) return errorResponse(403, "FORBIDDEN", "Policy read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->listPolicies(auth.context.tenantId)}}); }
    if (parts.size() == 2 && request.method == "POST") { if (!web::requireRole(auth.context, "policies", "write")) return errorResponse(403, "FORBIDDEN", "Policy write permission is required.", traceId); try { auto body = parseBody(request); if (const auto error = policyValidationError(body); error.has_value()) return errorResponse(422, error->starts_with("UNKNOWN_") ? "UNKNOWN_POLICY_FIELD" : "INVALID_POLICY", "The policy contains unsupported or invalid safety configuration.", traceId, {{"reason", *error}}); body["created_by_actor_id"] = auth.context.actorId; const auto policy = storage_->createPolicy(auth.context.tenantId, body); return policy.has_value() ? jsonResponse(201, *policy) : errorResponse(409, "POLICY_CREATE_FAILED", storage_->lastError(), traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); } }
    if (parts.size() == 3 && request.method == "GET") { if (!web::requireRole(auth.context, "policies", "read")) return errorResponse(403, "FORBIDDEN", "Policy read permission is required.", traceId); const auto policy = storage_->getPolicy(auth.context.tenantId, parts[2]); return policy.has_value() ? jsonResponse(200, *policy) : errorResponse(404, "NOT_FOUND", "Policy not found.", traceId); }
    if (parts.size() == 3 && request.method == "PATCH") {
      if (!web::requireRole(auth.context, "policies", "write")) return errorResponse(403, "FORBIDDEN", "Policy write permission is required.", traceId);
      try {
        const auto body = parseBody(request);
        const auto current = storage_->getPolicy(auth.context.tenantId, parts[2]);
        if (!current.has_value()) return errorResponse(404, "NOT_FOUND", "Policy not found.", traceId);
        if (current->value("status", "") != "draft") return errorResponse(409, "POLICY_UPDATE_FAILED", "Only draft policies can be edited.", traceId);
        const auto name = body.contains("name") ? body.value("name", "") : current->value("name", "");
        const auto selector = body.contains("selector") ? shared::CanonicalJson::serialize(body.at("selector")) : current->value("selector_json", "{}");
        const auto stagePlan = body.contains("stage_plan") ? shared::CanonicalJson::serialize(body.at("stage_plan")) : current->value("stage_plan_json", "[]");
        const auto gates = body.contains("health_gates") ? shared::CanonicalJson::serialize(body.at("health_gates")) : current->value("health_gates_json", "{}");
        Json candidate{{"name", name}, {"version", current->value("version", 1)}, {"selector", Json::parse(selector)}, {"stage_plan", Json::parse(stagePlan)}, {"health_gates", Json::parse(gates)}, {"max_offline_fraction", body.value("max_offline_fraction", current->value("max_offline_fraction", 0.2))}, {"telemetry_freshness_sec", body.value("telemetry_freshness_sec", current->value("telemetry_freshness_sec", 120))}, {"min_observation_sec", body.value("min_observation_sec", current->value("min_observation_sec", 900))}, {"two_person_approval", boolValue(body, "two_person_approval", boolValue(*current, "two_person_approval", true))}, {"require_iot_evidence", boolValue(body, "require_iot_evidence", boolValue(*current, "require_iot_evidence", false))}, {"rollback_requirement", body.value("rollback_requirement", current->value("rollback_requirement", "required"))}};
        if (const auto error = policyValidationError(candidate); error.has_value()) return errorResponse(422, "INVALID_POLICY", "The policy contains unsupported or invalid safety configuration.", traceId, {{"reason", *error}});
        const bool changed = storage_->execute("UPDATE rollout_policies SET name=?,selector_json=?,stage_plan_json=?,health_gates_json=?,max_offline_fraction=?,telemetry_freshness_sec=?,min_observation_sec=?,two_person_approval=?,require_iot_evidence=?,rollback_requirement=?,updated_at=datetime('now'),version=version+1 WHERE tenant_id=? AND id=? AND status='draft' AND version=?", {name, selector, stagePlan, gates, std::to_string(candidate.value("max_offline_fraction", 0.2)), std::to_string(candidate.value("telemetry_freshness_sec", 120)), std::to_string(candidate.value("min_observation_sec", 900)), boolValue(candidate, "two_person_approval", true) ? "1" : "0", boolValue(candidate, "require_iot_evidence", false) ? "1" : "0", candidate.value("rollback_requirement", "required"), auth.context.tenantId, parts[2], std::to_string(current->value("version", 1))});
        return changed ? jsonResponse(200, storage_->getPolicy(auth.context.tenantId, parts[2]).value_or(Json::object())) : errorResponse(409, "POLICY_VERSION_CONFLICT", "The policy changed after this control was loaded.", traceId);
      } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    if (parts.size() == 4 && request.method == "POST" && (parts[3] == "validate" || parts[3] == "activate" || parts[3] == "archive")) {
      if (parts[3] == "activate" || parts[3] == "archive" ? !web::requireRole(auth.context, "tenant", "write") : !web::requireRole(auth.context, "policies", "write")) return errorResponse(403, "FORBIDDEN", "The role cannot change this policy.", traceId);
      const auto policy = storage_->getPolicy(auth.context.tenantId, parts[2]);
      if (!policy.has_value()) return errorResponse(404, "NOT_FOUND", "Policy not found.", traceId);
      Json controlBody;
      try { controlBody = parseBody(request); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
      if (parts[3] != "validate" && !hasReason(controlBody)) return errorResponse(422, "REASON_REQUIRED", "A policy lifecycle action requires a reason.", traceId);
      if (parts[3] == "validate") {
        try {
          const Json candidate = {{"name", policy->value("name", "")}, {"version", policy->value("version", 1)}, {"selector", Json::parse(policy->value("selector_json", "{}"))}, {"stage_plan", Json::parse(policy->value("stage_plan_json", "[]"))}, {"health_gates", Json::parse(policy->value("health_gates_json", "{}"))}, {"max_offline_fraction", policy->value("max_offline_fraction", 0.2)}, {"telemetry_freshness_sec", policy->value("telemetry_freshness_sec", 120)}, {"min_observation_sec", policy->value("min_observation_sec", 900)}, {"two_person_approval", policy->value("two_person_approval", 1) != 0}, {"require_iot_evidence", policy->value("require_iot_evidence", 0) != 0}, {"rollback_requirement", policy->value("rollback_requirement", "required")}};
          if (const auto error = policyValidationError(candidate); error.has_value()) return errorResponse(422, "INVALID_POLICY", "The policy contains unsupported or invalid safety configuration.", traceId, {{"reason", *error}});
          return jsonResponse(200, {{"id", parts[2]}, {"status", "valid"}, {"stage_count", candidate.at("stage_plan").size()}});
        } catch (const std::exception&) { return errorResponse(422, "INVALID_POLICY", "The stored policy is not valid JSON.", traceId); }
      }
      if (parts[3] == "activate") {
        try {
          const Json candidate = {{"name", policy->value("name", "")}, {"version", policy->value("version", 1)}, {"selector", Json::parse(policy->value("selector_json", "{}"))}, {"stage_plan", Json::parse(policy->value("stage_plan_json", "[]"))}, {"health_gates", Json::parse(policy->value("health_gates_json", "{}"))}, {"max_offline_fraction", policy->value("max_offline_fraction", 0.2)}, {"telemetry_freshness_sec", policy->value("telemetry_freshness_sec", 120)}, {"min_observation_sec", policy->value("min_observation_sec", 900)}, {"two_person_approval", policy->value("two_person_approval", 1) != 0}, {"require_iot_evidence", policy->value("require_iot_evidence", 0) != 0}, {"rollback_requirement", policy->value("rollback_requirement", "required")}};
          if (const auto error = policyValidationError(candidate); error.has_value()) return errorResponse(422, "INVALID_POLICY", "The policy must validate before activation.", traceId, {{"reason", *error}});
        } catch (const std::exception&) { return errorResponse(422, "INVALID_POLICY", "The stored policy is not valid JSON.", traceId); }
        if (!storage_->execute("UPDATE rollout_policies SET status='archived',updated_at=datetime('now') WHERE tenant_id=? AND status='active' AND id<>?", {auth.context.tenantId, parts[2]})) return errorResponse(409, "POLICY_UPDATE_FAILED", storage_->lastError(), traceId);
      }
      const auto status = parts[3] == "activate" ? "active" : parts[3] == "archive" ? "archived" : policy->value("status", "draft");
      const bool changed = parts[3] == "validate" ? true : storage_->execute("UPDATE rollout_policies SET status=?,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {status, auth.context.tenantId, parts[2]});
      return changed ? jsonResponse(200, {{"id", parts[2]}, {"status", status}, {"validation", parts[3] == "validate" ? "passed" : "not_run"}}) : errorResponse(409, "POLICY_UPDATE_FAILED", storage_->lastError(), traceId);
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "benchmarks") {
    if (parts.size() == 2 && request.method == "GET") {
      if (!web::requireRole(auth.context, "simulation", "read")) return errorResponse(403, "FORBIDDEN", "Benchmark read permission is required.", traceId);
      return jsonResponse(200, {{"items", storage_->query("SELECT id,corpus_version,status,expected_case_count,completed_case_count,result_digest,report_bundle_sha256,created_at,completed_at FROM benchmark_runs WHERE tenant_id=? ORDER BY created_at DESC", {auth.context.tenantId})}});
    }
    if (parts.size() == 2 && request.method == "POST") {
      if (!web::requireRole(auth.context, "simulation", "write")) return errorResponse(403, "FORBIDDEN", "Benchmark write permission is required.", traceId);
      try {
        const auto body = parseBody(request);
        const auto id = shared::Uuid::generate().str();
        const auto corpusVersion = body.value("corpus_version", "v1");
        const auto manifest = domain::BenchmarkRunner::frozenManifest();
        const auto manifestJson = shared::CanonicalJson::serialize(manifest);
        if (corpusVersion != "v1") return errorResponse(422, "UNKNOWN_CORPUS", "Only the frozen v1 benchmark corpus is available.", traceId);
        const bool saved = storage_->execute("INSERT INTO benchmark_runs(id,tenant_id,corpus_version,corpus_manifest_json,corpus_manifest_digest,status,expected_case_count,completed_case_count,aggregate_metrics_json,requested_by_actor_id,created_at) VALUES(?,?,?,?,?,'queued',108,0,'{}',?,datetime('now'))", {id, auth.context.tenantId, corpusVersion, manifestJson, shared::DigestService::sha256Hex(manifestJson), auth.context.actorId});
        return saved ? jsonResponse(202, {{"id", id}, {"status", "queued"}, {"corpus_version", corpusVersion}, {"expected_case_count", 108}}) : errorResponse(409, "BENCHMARK_SAVE_FAILED", storage_->lastError(), traceId);
      } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    if (parts.size() == 3 && request.method == "GET") {
      if (!web::requireRole(auth.context, "simulation", "read")) return errorResponse(403, "FORBIDDEN", "Benchmark read permission is required.", traceId);
      const auto run = storage_->query("SELECT id,corpus_version,status,expected_case_count,completed_case_count,aggregate_metrics_json,result_digest,created_at,completed_at FROM benchmark_runs WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[2]});
      if (run.empty()) return errorResponse(404, "NOT_FOUND", "Benchmark run not found.", traceId);
      auto result = run.front(); result["results"] = storage_->query("SELECT scenario_name,seed,strategy,metrics_json,passed,result_digest FROM benchmark_results WHERE tenant_id=? AND benchmark_run_id=? ORDER BY scenario_name,seed,strategy", {auth.context.tenantId, parts[2]}); return jsonResponse(200, result);
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "replays") {
    if (parts.size() == 2 && request.method == "GET") {
      if (!web::requireRole(auth.context, "simulation", "read")) return errorResponse(403, "FORBIDDEN", "Replay read permission is required.", traceId);
      return jsonResponse(200, {{"items", storage_->query("SELECT id,release_id,simulation_run_id,source_kind,status,source_event_from,source_event_to,expected_decision_digest,actual_decision_digest,divergence_json,created_at,completed_at FROM replay_runs WHERE tenant_id=? ORDER BY created_at DESC", {auth.context.tenantId})}});
    }
    if (parts.size() == 3 && request.method == "GET") {
      if (!web::requireRole(auth.context, "simulation", "read")) return errorResponse(403, "FORBIDDEN", "Replay read permission is required.", traceId);
      const auto run = storage_->query("SELECT id,release_id,simulation_run_id,source_kind,status,source_event_from,source_event_to,expected_decision_digest,actual_decision_digest,divergence_json,created_at,completed_at FROM replay_runs WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[2]}); return run.empty() ? errorResponse(404, "NOT_FOUND", "Replay run not found.", traceId) : jsonResponse(200, run.front());
    }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "releases") return releaseRoute(request, auth, parts);
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "simulations") return simulationRoute(request, auth, parts);
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "evidence") return evidenceRoute(request, auth, parts);
  if (parts.size() == 4 && parts[0] == "api" && parts[1] == "approvals" && (parts[3] == "approve" || parts[3] == "reject") && request.method == "POST") {
    if (!web::requireRole(auth.context, "approvals", "approve")) return errorResponse(403, "FORBIDDEN", "Approval permission is required.", traceId);
    const auto approval = storage_->query("SELECT id,release_id,action,status,captured_release_version,gate_evaluation_id,evidence_digest,requested_by_actor_id FROM approval_requests WHERE tenant_id=? AND id=? AND status='requested' AND expires_at > datetime('now')", {auth.context.tenantId, parts[2]});
    if (approval.empty()) return errorResponse(409, "APPROVAL_NOT_PENDING", "The approval is missing, expired, or already decided.", traceId);
    if (approval.front().at("requested_by_actor_id") == auth.context.actorId) return errorResponse(403, "FOUR_EYES_REQUIRED", "The requesting operator cannot approve their own action.", traceId);
    const auto release = storage_->getRelease(auth.context.tenantId, approval.front().at("release_id").get<std::string>());
    if (!release.has_value() || release->value("version", 0) != approval.front().value("captured_release_version", -1)) return errorResponse(409, "APPROVAL_VERSION_STALE", "The approval is bound to an older release version.", traceId);
    if (approval.front().value("action", "") == "gate_override") {
      const auto evaluation = storage_->query("SELECT id,decision,evidence_digest FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? AND id=?", {auth.context.tenantId, approval.front().at("release_id").get<std::string>(), approval.front().value("gate_evaluation_id", "")});
      if (evaluation.empty() || evaluation.front().value("evidence_digest", "") != approval.front().value("evidence_digest", "") || (evaluation.front().value("decision", "") != "pause" && evaluation.front().value("decision", "") != "insufficient_evidence")) return errorResponse(409, "APPROVAL_EVIDENCE_STALE", "The gate evaluation bound to this approval is no longer valid.", traceId);
    }
    Json decisionBody;
    try { decisionBody = parseBody(request); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    const auto decisionReason = decisionBody.value("reason", "");
    if (decisionReason.empty()) return errorResponse(422, "REASON_REQUIRED", "An approval decision requires a reason.", traceId);
    const auto newStatus = parts[3] == "approve" ? "approved" : "rejected";
    const auto approvalAction = approval.front().value("action", "");
    const auto releaseId = approval.front().at("release_id").get<std::string>();
    const auto capturedVersion = approval.front().at("captured_release_version").get<int>();
    const auto releaseState = domain::releaseStateFromString(release->value("status", ""));
    const auto approvalTransition = approvalAction == "start" ? domain::ReleaseAction::approve
                                      : approvalAction == "gate_override" ? domain::ReleaseAction::gate_override
                                      : approvalAction == "resume" ? domain::ReleaseAction::resume
                                      : approvalAction == "abort" ? domain::ReleaseAction::abort
                                      : approvalAction == "rollback" ? domain::ReleaseAction::rollback
                                                                       : domain::ReleaseAction::approve;
    if (!releaseState.has_value() || ((newStatus == std::string("approved") || approvalAction == "start") &&
                                         !domain::ReleaseStateMachine::transition(*releaseState, approvalTransition).has_value())) {
      return errorResponse(409, "APPROVAL_STATE_STALE", "The release can no longer accept this approval decision.", traceId);
    }
    if (newStatus == std::string("approved") && approvalAction == "rollback") {
      if (stringValue(*release, "frozen_rollback_json").empty()) return errorResponse(409, "ROLLBACK_ARTIFACT_NOT_READY", "The release no longer has a trusted frozen rollback artifact.", traceId);
      const auto rollback = storage_->query("SELECT a.id FROM artifacts a JOIN artifact_signing_keys k ON k.tenant_id=a.tenant_id AND k.id=a.signature_key_id WHERE a.tenant_id=? AND a.id=? AND a.status='ready' AND k.status='active'", {auth.context.tenantId, stringValue(*release, "rollback_artifact_id")});
      if (rollback.empty()) return errorResponse(409, "ROLLBACK_ARTIFACT_NOT_READY", "The rollback artifact or its signing key is no longer trusted.", traceId);
    }
    const bool committed = storage_->transaction([&] {
      if (!storage_->execute("UPDATE approval_requests SET status=?,approved_release_version=CASE WHEN ?='approved' AND action='start' THEN captured_release_version+1 WHEN ?='approved' THEN captured_release_version ELSE NULL END,consumed_at=CASE WHEN ?='approved' AND action<>'start' THEN datetime('now') ELSE consumed_at END,decision_reason=NULLIF(?,'') ,decided_by_actor_id=?,decided_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='requested'", {newStatus, newStatus, newStatus, newStatus, decisionReason, auth.context.actorId, auth.context.tenantId, parts[2]})) return false;
      if (newStatus == std::string("approved")) {
        if (approvalAction == "start") {
          if (!storage_->updateRelease(auth.context.tenantId, releaseId, "ready", capturedVersion)) return false;
        } else if (approvalAction == "gate_override") {
          if (!storage_->updateRelease(auth.context.tenantId, releaseId, "running", capturedVersion)) return false;
          if (!storage_->execute("UPDATE release_stages SET observation_started_at=datetime('now'),observation_ends_at=datetime('now','+" + std::to_string(config_.minObservationSeconds) + " seconds'),gate_decision_json=NULL,updated_at=datetime('now') WHERE tenant_id=? AND release_id=? AND status='active'", {auth.context.tenantId, releaseId})) return false;
        } else if (approvalAction == "resume") {
          if (!storage_->updateRelease(auth.context.tenantId, releaseId, "running", capturedVersion)) return false;
        } else if (approvalAction == "abort") {
          if (!storage_->updateRelease(auth.context.tenantId, releaseId, "aborting", capturedVersion)) return false;
        const auto assignments = storage_->query("SELECT id,stage_id,device_id,desired_generation FROM release_assignments WHERE tenant_id=? AND release_id=? AND state IN ('pending','commanded','acknowledged')", {auth.context.tenantId, releaseId});
          for (const auto& assignment : assignments) {
            const auto assignmentId = assignment.at("id").get<std::string>();
            const auto generation = assignment.at("desired_generation").get<long long>();
            if (!storage_->execute("UPDATE release_assignments SET state='cancelling',updated_at=datetime('now') WHERE tenant_id=? AND id=? AND state IN ('pending','commanded','acknowledged')", {auth.context.tenantId, assignmentId})) return false;
            if (!storage_->execute("INSERT OR IGNORE INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) VALUES(?,?,?,?,?,?, 'cancel',?,NULLIF(?,''),?,?,datetime('now'),datetime('now','+7 days'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, releaseId, assignment.at("stage_id").get<std::string>(), assignmentId, assignment.at("device_id").get<std::string>(), std::to_string(generation), "", shared::CanonicalJson::serialize({{"type", "cancel"}, {"assignment_id", assignmentId}, {"generation", generation}}), "cancel-" + assignmentId + "-" + std::to_string(generation)})) return false;
          }
        } else if (approvalAction == "rollback") {
          if (!storage_->updateRelease(auth.context.tenantId, releaseId, "rolling_back", capturedVersion)) return false;
          const auto rollbackArtifact = stringValue(*release, "rollback_artifact_id");
        const auto assignments = storage_->query("SELECT id,stage_id,device_id,desired_generation FROM release_assignments WHERE tenant_id=? AND release_id=? AND state IN ('commanded','acknowledged','converged','failed','stranded')", {auth.context.tenantId, releaseId});
          for (const auto& assignment : assignments) {
            const auto device = storage_->getDevice(auth.context.tenantId, assignment.at("device_id").get<std::string>());
            if (!device.has_value()) return false;
            const auto generation = std::max<long long>(device->value("desired_generation", 0LL) + 1, assignment.at("desired_generation").get<long long>() + 1);
            const auto assignmentId = assignment.at("id").get<std::string>();
            if (!storage_->execute("UPDATE release_assignments SET desired_artifact_id=?,desired_generation=?,state='pending',failure_code=NULL,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {rollbackArtifact, std::to_string(generation), auth.context.tenantId, assignmentId}) ||
                !storage_->execute("UPDATE devices SET desired_generation=CASE WHEN desired_generation<? THEN ? ELSE desired_generation END,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {std::to_string(generation), std::to_string(generation), auth.context.tenantId, assignment.at("device_id").get<std::string>()}) ||
                !storage_->execute("INSERT OR IGNORE INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) VALUES(?,?,?,?,?,?, 'rollback',?,?,?, ?,datetime('now'),datetime('now','+7 days'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, releaseId, assignment.at("stage_id").get<std::string>(), assignmentId, assignment.at("device_id").get<std::string>(), std::to_string(generation), rollbackArtifact, "{}", "rollback-" + assignmentId + "-" + std::to_string(generation)})) return false;
          }
        }
      } else if (approvalAction == "start") {
        if (!storage_->updateRelease(auth.context.tenantId, releaseId, "ready", capturedVersion)) return false;
      }
      return storage_->appendEvidence(auth.context.tenantId, std::string("approval.") + newStatus, "approval", parts[2], {{"release_id", releaseId}, {"action", approvalAction}, {"decided_by_actor_id", auth.context.actorId}, {"reason", decisionReason}}, "operator", auth.context.actorId).has_value();
    });
    if (!committed) return errorResponse(409, "APPROVAL_CONFLICT", "The approval or its release side effects could not be committed.", traceId);
    return jsonResponse(200, {{"id", parts[2]}, {"status", newStatus}});
  }
  if (path == "/api/approvals" && request.method == "GET") { if (!web::requireRole(auth.context, "approvals", "read")) return errorResponse(403, "FORBIDDEN", "Approval read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->query("SELECT id,release_id,action,status,captured_release_version,requested_by_actor_id,created_at,expires_at FROM approval_requests WHERE tenant_id=? ORDER BY CASE WHEN status='requested' THEN 0 ELSE 1 END, created_at DESC, id DESC", {auth.context.tenantId})}}); }
  if (parts.size() == 2 && parts[0] == "api" && parts[1] == "integrations") { if (request.method == "GET") { if (!web::requireRole(auth.context, "integrations", "read")) return errorResponse(403, "FORBIDDEN", "Integration read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->query("SELECT id,adapter_type,enabled,required_for_promotion,endpoint_base_url,health_status,last_error_code,updated_at FROM integration_configs WHERE tenant_id=? ORDER BY adapter_type", {auth.context.tenantId})}}); } }
  if (parts.size() == 3 && parts[0] == "api" && parts[1] == "integrations" && request.method == "PUT") {
    if (auth.context.role != shared::Role::admin) return errorResponse(403, "FORBIDDEN", "Only administrators can configure integrations.", traceId);
    try {
      const auto body = parseBody(request);
      const auto adapter = parts[2];
      if (adapter != "iot_rest_v1" && adapter != "notification_hub_v1" && adapter != "workflow_manual_v1") return errorResponse(422, "UNKNOWN_ADAPTER", "The adapter type is not supported.", traceId);
      const auto endpoint = body.value("endpoint_base_url", "");
      const auto secretRef = body.value("secret_ref", "");
      auto settings = body.value("settings", Json::object());
      if (!settings.is_object()) return errorResponse(422, "INVALID_ADAPTER_SETTINGS", "Adapter settings must be an object.", traceId);
      for (const auto& secretField : {"api_key", "apiKey", "token", "password", "secret"}) if (settings.contains(secretField)) return errorResponse(422, "RAW_SECRET_FORBIDDEN", "Adapter settings must reference an environment secret, not store one.", traceId);
      if (adapter == "workflow_manual_v1" && !settings.contains("workflow_id") && !config_.workflowId.empty()) settings["workflow_id"] = config_.workflowId;
      if (adapter == "workflow_manual_v1" && settings.value("workflow_id", "").empty()) return errorResponse(422, "WORKFLOW_ID_REQUIRED", "Workflow integration requires a workflow identifier.", traceId);
      if (!boolValue(settings, "fixture_mode", true) && endpoint.empty()) return errorResponse(422, "ADAPTER_ENDPOINT_REQUIRED", "Live adapters require an HTTPS endpoint.", traceId);
      if (!endpoint.empty() && !safeAdapterUrl(config_, endpoint)) return errorResponse(422, "UNSAFE_ADAPTER_URL", "Adapter endpoints must resolve to approved private hosts.", traceId);
      if (!boolValue(settings, "fixture_mode", true) && endpoint.starts_with("http://")) return errorResponse(422, "ADAPTER_HTTPS_REQUIRED", "Live adapters require HTTPS endpoints.", traceId);
      if (const auto error = shared::SecretResolver::validateReference(secretRef); error.has_value() || secretRef.starts_with("edge_live_")) return errorResponse(422, "INVALID_SECRET_REFERENCE", "Adapters accept a valid environment secret-reference name, not a raw credential.", traceId);
      const auto id = shared::Uuid::generate().str();
      const bool ok = storage_->transaction([&] {
        if (!storage_->execute("INSERT INTO integration_configs(id,tenant_id,adapter_type,enabled,required_for_promotion,endpoint_base_url,secret_ref,settings_json,health_status,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,datetime('now'),datetime('now')) ON CONFLICT(tenant_id,adapter_type) DO UPDATE SET enabled=0,required_for_promotion=excluded.required_for_promotion,endpoint_base_url=excluded.endpoint_base_url,secret_ref=excluded.secret_ref,settings_json=excluded.settings_json,health_status='disabled',updated_at=datetime('now')", {id, auth.context.tenantId, adapter, "0", body.value("required_for_promotion", false) ? "1" : "0", endpoint, secretRef, shared::CanonicalJson::serialize(settings), "disabled"})) return false;
        return storage_->appendEvidence(auth.context.tenantId, "integration.configured", "integration", adapter, {{"adapter_type", adapter}, {"enabled", false}}, "operator", auth.context.actorId).has_value();
      });
      if (!ok) return errorResponse(409, "INTEGRATION_UPDATE_FAILED", "The integration configuration could not be stored.", traceId);
      return jsonResponse(200, storage_->query("SELECT adapter_type,enabled,required_for_promotion,endpoint_base_url,secret_ref,health_status FROM integration_configs WHERE tenant_id=? AND adapter_type=?", {auth.context.tenantId, adapter}).front());
    } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
  }
  if (parts.size() == 4 && parts[0] == "api" && parts[1] == "integrations" && request.method == "POST" && (parts[3] == "test" || parts[3] == "enable" || parts[3] == "disable")) {
    if (auth.context.role != shared::Role::admin) return errorResponse(403, "FORBIDDEN", "Only administrators can operate integrations.", traceId);
    const auto adapter = parts[2];
    if (adapter != "iot_rest_v1" && adapter != "notification_hub_v1" && adapter != "workflow_manual_v1") return errorResponse(422, "UNKNOWN_ADAPTER", "The adapter type is not supported.", traceId);
    const auto configured = storage_->query("SELECT id,endpoint_base_url,secret_ref,health_status,required_for_promotion FROM integration_configs WHERE tenant_id=? AND adapter_type=?", {auth.context.tenantId, adapter});
    if (configured.empty()) return errorResponse(404, "NOT_FOUND", "Integration configuration not found.", traceId);
    if (parts[3] == "test") {
      if (!safeAdapterUrl(config_, configured.front().value("endpoint_base_url", "http://localhost"))) return errorResponse(422, "UNSAFE_ADAPTER_URL", "The configured adapter endpoint is unsafe.", traceId);
      Json settings = Json::object();
      try { settings = Json::parse(configured.front().value("settings_json", "{}")); } catch (const std::exception&) { return errorResponse(422, "INVALID_ADAPTER_SETTINGS", "The adapter settings are invalid.", traceId); }
      auto testStatus = std::string("fixture_ok");
      auto externalCalls = 0;
      if (!settings.value("fixture_mode", true)) {
        const auto secret = shared::SecretResolver::environment(configured.front().value("secret_ref", ""));
        shared::HttpClientPool clientPool;
        const auto testedResponse = infrastructure::AdapterContract::testConnection(adapter, configured.front().value("endpoint_base_url", ""), secret.value_or(""), settings, clientPool);
        if (testedResponse.disposition != infrastructure::DeliveryDisposition::published) return errorResponse(503, testedResponse.errorCode.empty() ? "ADAPTER_UNAVAILABLE" : testedResponse.errorCode, "The live adapter connection test failed.", traceId);
        testStatus = "live_ok";
        externalCalls = 1;
      }
      const bool tested = storage_->transaction([&] {
        if (!storage_->execute("UPDATE integration_configs SET health_status='healthy',last_success_at=datetime('now'),last_polled_at=datetime('now'),last_error_code=NULL,updated_at=datetime('now') WHERE tenant_id=? AND adapter_type=?", {auth.context.tenantId, adapter})) return false;
        return storage_->appendEvidence(auth.context.tenantId, "integration.tested", "integration", adapter, {{"adapter_type", adapter}, {"status", testStatus}, {"external_calls", externalCalls}}, "operator", auth.context.actorId).has_value();
      });
      if (!tested) return errorResponse(409, "INTEGRATION_TEST_FAILED", "The adapter test result could not be committed.", traceId);
      return jsonResponse(200, {{"adapter_type", adapter}, {"status", testStatus}, {"external_calls", externalCalls}, {"enabled", false}});
    }
    if (parts[3] != "test") {
      try { if (!hasReason(parseBody(request))) return errorResponse(422, "REASON_REQUIRED", "Changing integration enablement requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    }
    const auto enabled = parts[3] == "enable" ? "1" : "0";
    if (parts[3] == "enable" && configured.front().value("secret_ref", "").empty()) return errorResponse(422, "SECRET_REFERENCE_REQUIRED", "An adapter secret reference is required before enablement.", traceId);
    if (parts[3] == "enable" && configured.front().value("health_status", "") != "healthy") return errorResponse(409, "ADAPTER_TEST_REQUIRED", "An adapter must pass its fixture or sandbox connection test before enablement.", traceId);
    if (parts[3] == "disable" && configured.front().value("required_for_promotion", 0) != 0) {
      const auto activeReleases = storage_->query("SELECT id FROM releases WHERE tenant_id=? AND status IN ('running','paused','scheduled','awaiting_approval')", {auth.context.tenantId});
      if (!activeReleases.empty()) return errorResponse(409, "INTEGRATION_DISABLE_REQUIRES_CONTROL", "A required promotion adapter cannot be disabled while a release is active or awaiting approval.", traceId);
    }
    const bool changed = storage_->transaction([&] {
      if (!storage_->execute("UPDATE integration_configs SET enabled=?,health_status=?,updated_at=datetime('now') WHERE tenant_id=? AND adapter_type=?", {enabled, parts[3] == "enable" ? "healthy" : "disabled", auth.context.tenantId, adapter})) return false;
      return storage_->appendEvidence(auth.context.tenantId, std::string("integration.") + (parts[3] == "enable" ? "enabled" : "disabled"), "integration", adapter, {{"adapter_type", adapter}, {"enabled", parts[3] == "enable"}}, "operator", auth.context.actorId).has_value();
    });
    if (!changed) return errorResponse(409, "INTEGRATION_UPDATE_FAILED", storage_->lastError(), traceId);
    return jsonResponse(200, {{"adapter_type", adapter}, {"enabled", parts[3] == "enable"}, {"status", parts[3] == "enable" ? "healthy" : "disabled"}});
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "notices") {
    if (parts.size() == 2 && request.method == "GET") { if (!web::requireRole(auth.context, "evidence", "read")) return errorResponse(403, "FORBIDDEN", "Notice read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->query("SELECT id,release_id,event_id,severity,title,body,acknowledged_by_actor_id,acknowledged_at,created_at FROM operator_notices WHERE tenant_id=? ORDER BY created_at DESC", {auth.context.tenantId})}}); }
    if (parts.size() == 4 && parts[3] == "acknowledge" && request.method == "POST") { if (!web::requireRole(auth.context, "evidence", "read")) return errorResponse(403, "FORBIDDEN", "Notice permission is required.", traceId); return storage_->execute("UPDATE operator_notices SET acknowledged_by_actor_id=?,acknowledged_at=datetime('now') WHERE tenant_id=? AND id=?", {auth.context.actorId, auth.context.tenantId, parts[2]}) ? jsonResponse(200, {{"id", parts[2]}, {"acknowledged", true}}) : errorResponse(404, "NOT_FOUND", "Notice not found.", traceId); }
  }
  if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "outbox") {
    if (parts.size() == 2 && request.method == "GET") { if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can inspect the outbox.", traceId); return jsonResponse(200, {{"items", storage_->query("SELECT id,evidence_event_id,adapter_type,status,idempotency_key,attempt_count,next_attempt_at,last_status_code,last_error_code,external_reference,published_at,updated_at FROM outbox_deliveries WHERE tenant_id=? ORDER BY created_at DESC", {auth.context.tenantId})}}); }
    if (parts.size() == 4 && parts[3] == "retry" && request.method == "POST") { if (!web::requireRole(auth.context, "tenant", "write")) return errorResponse(403, "FORBIDDEN", "Only administrators can retry outbox deliveries.", traceId); try { if (!hasReason(parseBody(request))) return errorResponse(422, "REASON_REQUIRED", "Outbox retry requires a reason.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); } return storage_->execute("UPDATE outbox_deliveries SET status='pending',next_attempt_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[2]}) ? jsonResponse(202, {{"id", parts[2]}, {"status", "pending"}}) : errorResponse(404, "NOT_FOUND", "Outbox delivery not found.", traceId); }
  }
  if (path.starts_with("/api/agent/v1/")) return agentRoute(request, auth, parts);
  if (path.starts_with("/app")) return htmlRoute(request, auth);
  return errorResponse(404, "NOT_FOUND", "The requested route does not exist.", traceId);
}

HttpResponse ControlPlane::releaseRoute(const HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts) {
  const auto traceId = auth.traceId;
  if (parts.size() == 4 && parts[3] == "replays" && request.method == "POST") {
    if (!web::requireRole(auth.context, "simulation", "write")) return errorResponse(403, "FORBIDDEN", "Replay permission is required.", traceId);
    const auto chain = storage_->verifyEvidence(auth.context.tenantId);
    if (!chain.value("valid", false)) return errorResponse(409, "EVIDENCE_CHAIN_BROKEN", "The tenant evidence chain must verify before replay.", traceId);
    const auto rows = storage_->query("SELECT id,sequence_no,tenant_id,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash FROM evidence_events WHERE tenant_id=? AND aggregate_type='release' AND aggregate_id=? ORDER BY sequence_no", {auth.context.tenantId, parts[2]});
    if (rows.empty()) return errorResponse(422, "EMPTY_REPLAY_SOURCE", "The release has no evidence range to replay.", traceId);
    try {
      Json events = Json::array();
      for (const auto& row : rows) {
        events.push_back({{"id", row.at("id")}, {"sequence_no", row.at("sequence_no")}, {"tenant_id", row.at("tenant_id")}, {"aggregate_type", row.at("aggregate_type")}, {"aggregate_id", row.at("aggregate_id")}, {"event_type", row.at("event_type")}, {"actor_type", row.at("actor_type")}, {"actor_id", row.at("actor_id")}, {"payload", Json::parse(row.at("payload_json").get<std::string>())}, {"occurred_at", row.at("occurred_at")}, {"trace_id", row.at("trace_id")}, {"previous_hash", row.at("previous_hash")}, {"event_hash", row.at("event_hash")}});
      }
      const auto requested = parseBody(request);
      const auto expected = requested.value("expected_decision_digest", shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(events)));
      const auto replayId = shared::Uuid::generate().str();
      const bool saved = storage_->execute("INSERT INTO replay_runs(id,tenant_id,release_id,source_kind,status,source_event_from,source_event_to,source_snapshot_json,expected_decision_digest,created_at) VALUES(?,?,?,'evidence','queued',?,?,?,?,datetime('now'))", {replayId, auth.context.tenantId, parts[2], std::to_string(rows.front().at("sequence_no").get<long long>()), std::to_string(rows.back().at("sequence_no").get<long long>()), shared::CanonicalJson::serialize(events), expected});
      return saved ? jsonResponse(202, {{"id", replayId}, {"status", "queued"}, {"source_event_from", rows.front().at("sequence_no")}, {"source_event_to", rows.back().at("sequence_no")}, {"expected_decision_digest", expected}}) : errorResponse(409, "REPLAY_CREATE_FAILED", storage_->lastError(), traceId);
    } catch (const std::exception&) { return errorResponse(422, "REPLAY_SOURCE_INVALID", "The release evidence source is malformed.", traceId); }
  }
  if (parts.size() == 2 && request.method == "GET") { if (!web::requireRole(auth.context, "release_drafts", "read")) return errorResponse(403, "FORBIDDEN", "Release read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->listReleases(auth.context.tenantId)}}); }
  if (parts.size() == 2 && request.method == "POST") { if (!web::requireRole(auth.context, "release_drafts", "write")) return errorResponse(403, "FORBIDDEN", "Release write permission is required.", traceId); try { auto body = parseBody(request); body["created_by_actor_id"] = auth.context.actorId; const auto release = storage_->createRelease(auth.context.tenantId, body); return release.has_value() ? jsonResponse(201, *release) : errorResponse(409, "RELEASE_CREATE_FAILED", storage_->lastError(), traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); } }
  if (parts.size() == 3 && request.method == "GET") {
    if (!web::requireRole(auth.context, "release_drafts", "read")) return errorResponse(403, "FORBIDDEN", "Release read permission is required.", traceId);
    const auto release = storage_->getRelease(auth.context.tenantId, parts[2]);
    if (!release.has_value()) return errorResponse(404, "NOT_FOUND", "Release not found.", traceId);
    auto result = *release;
    result["stages"] = storage_->query("SELECT id,ordinal,target_percentage,status,eligible_count,assigned_count,observation_started_at,observation_ends_at,gate_decision_json,started_at,ended_at FROM release_stages WHERE tenant_id=? AND release_id=? ORDER BY ordinal", {auth.context.tenantId, parts[2]});
    result["assignment_counts"] = storage_->query("SELECT state,COUNT(*) AS count FROM release_assignments WHERE tenant_id=? AND release_id=? GROUP BY state ORDER BY state", {auth.context.tenantId, parts[2]});
    result["latest_gate"] = storage_->query("SELECT id,stage_id,decision,failed_gates_json,evidence_digest,evaluated_at FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? ORDER BY evaluated_at DESC LIMIT 1", {auth.context.tenantId, parts[2]});
    return jsonResponse(200, result);
  }
  if (parts.size() == 3 && request.method == "PATCH") {
    if (!web::requireRole(auth.context, "release_drafts", "write")) return errorResponse(403, "FORBIDDEN", "Release write permission is required.", traceId);
    try { const auto body = parseBody(request); const auto current = storage_->getRelease(auth.context.tenantId, parts[2]); if (!current.has_value()) return errorResponse(404, "NOT_FOUND", "Release not found.", traceId); if (!body.contains("expected_version") || !body.at("expected_version").is_number_integer()) return errorResponse(422, "EXPECTED_VERSION_REQUIRED", "Release updates require expected_version.", traceId); const auto expectedVersion = body.at("expected_version").get<int>(); const bool changed = storage_->execute("UPDATE releases SET name=COALESCE(NULLIF(?,''),name),target_selector_json=COALESCE(?,target_selector_json),updated_at=datetime('now'),version=version+1 WHERE tenant_id=? AND id=? AND status='draft' AND version=?", {body.value("name", ""), shared::CanonicalJson::serialize(body.value("selector", Json::object())), auth.context.tenantId, parts[2], std::to_string(expectedVersion)}); return changed ? jsonResponse(200, storage_->getRelease(auth.context.tenantId, parts[2]).value_or(Json::object())) : errorResponse(409, "RELEASE_VERSION_CONFLICT", "The release changed after this control was loaded.", traceId); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
  }
  if (parts.size() == 4 && request.method == "GET" && (parts[3] == "membership" || parts[3] == "gates" || parts[3] == "assignments")) {
    if (!web::requireRole(auth.context, "release_drafts", "read")) return errorResponse(403, "FORBIDDEN", "Release read permission is required.", traceId);
    if (parts[3] == "membership") return jsonResponse(200, {{"items", storage_->query("SELECT id,release_id,device_id,cohort_hash_hex,cohort_ordinal,frozen_labels_json,frozen_observed_digest,frozen_observed_generation,included_at FROM release_memberships WHERE tenant_id=? AND release_id=? ORDER BY cohort_ordinal", {auth.context.tenantId, parts[2]})}});
    if (parts[3] == "gates") return jsonResponse(200, {{"items", storage_->query("SELECT id,release_id,stage_id,decision,sample_window_start,sample_window_end,sample_count,eligible_device_count,fresh_device_count,metrics_json,failed_gates_json,evidence_digest,evaluated_at FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? ORDER BY evaluated_at DESC", {auth.context.tenantId, parts[2]})}});
    return jsonResponse(200, {{"items", storage_->query("SELECT id,release_id,stage_id,device_id,desired_artifact_id,desired_generation,state,latest_report_sequence,commanded_at,acknowledged_at,converged_at,failure_code,updated_at FROM release_assignments WHERE tenant_id=? AND release_id=? ORDER BY device_id", {auth.context.tenantId, parts[2]})}});
  }
  if (parts.size() == 4 && parts[3] == "validate" && request.method == "POST") {
    if (!web::requireRole(auth.context, "release_drafts", "write")) return errorResponse(403, "FORBIDDEN", "Release validation permission is required.", traceId);
    const auto release = storage_->getRelease(auth.context.tenantId, parts[2]);
    if (!release.has_value()) return errorResponse(404, "NOT_FOUND", "Release not found.", traceId);
    if (release->value("status", "") != "draft" && release->value("status", "") != "blocked") return errorResponse(409, "INVALID_RELEASE_STATE", "Only draft or blocked releases can be validated.", traceId);
    Json validationBody;
    try { validationBody = parseBody(request); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    if (!validationBody.contains("expected_version") || !validationBody.at("expected_version").is_number_integer()) return errorResponse(422, "EXPECTED_VERSION_REQUIRED", "Release validation requires expected_version.", traceId);
    if (validationBody.at("expected_version").get<int>() != release->value("version", 1)) return errorResponse(409, "RELEASE_VERSION_CONFLICT", "The release version is stale.", traceId, {{"current_version", release->value("version", 1)}});
    const auto policy = storage_->getPolicy(auth.context.tenantId, release->value("policy_id", ""));
    const auto artifact = storage_->query("SELECT a.id,a.status,a.storage_key,a.manifest_json,a.signature,a.signature_key_id,a.sha256_digest,a.size_bytes,a.name,a.version,a.hardware_model,a.architecture,k.status AS key_status FROM artifacts a JOIN artifact_signing_keys k ON k.tenant_id=a.tenant_id AND k.id=a.signature_key_id WHERE a.tenant_id=? AND a.id=?", {auth.context.tenantId, stringValue(*release, "artifact_id")});
    const auto rollbackArtifact = stringValue(*release, "rollback_artifact_id").empty() ? std::vector<Json>{} : storage_->query("SELECT a.id,a.status,a.storage_key,a.manifest_json,a.signature,a.signature_key_id,a.sha256_digest,a.size_bytes,a.name,a.version,a.hardware_model,a.architecture,k.status AS key_status FROM artifacts a JOIN artifact_signing_keys k ON k.tenant_id=a.tenant_id AND k.id=a.signature_key_id WHERE a.tenant_id=? AND a.id=?", {auth.context.tenantId, stringValue(*release, "rollback_artifact_id")});
    if (!policy.has_value() || policy->value("status", "") != "active") return errorResponse(422, "POLICY_NOT_ACTIVE", "A release requires an active policy snapshot.", traceId);
    if (artifact.empty() || artifact.front().value("status", "") != "ready" || artifact.front().value("key_status", "") != "active") return errorResponse(422, "ARTIFACT_NOT_READY", "The target artifact or its signing key is not trusted.", traceId);
    if (!artifactBytesMatch(artifact.front())) return errorResponse(503, "ARTIFACT_CORRUPT", "The target artifact failed its integrity check.", traceId);
    const auto rollbackRequirement = stringValue(*policy, "rollback_requirement", "required");
    if (rollbackRequirement == "required" && rollbackArtifact.empty()) return errorResponse(422, "ROLLBACK_ARTIFACT_REQUIRED", "This policy requires a rollback artifact.", traceId);
    if (!rollbackArtifact.empty() && (rollbackArtifact.front().value("status", "") != "ready" || rollbackArtifact.front().value("key_status", "") != "active")) return errorResponse(422, "ROLLBACK_ARTIFACT_NOT_READY", "The rollback artifact or its signing key is not trusted.", traceId);
    if (!rollbackArtifact.empty() && !artifactBytesMatch(rollbackArtifact.front())) return errorResponse(503, "ROLLBACK_ARTIFACT_CORRUPT", "The rollback artifact failed its integrity check.", traceId);
    if (!rollbackArtifact.empty() && rollbackArtifact.front().value("signature_key_id", "") == artifact.front().value("signature_key_id", "")) return errorResponse(422, "ROLLBACK_KEY_NOT_DISTINCT", "Rollback must be signed by a different trusted key.", traceId);
    const auto devices = storage_->query("SELECT id,stable_key,hardware_model,architecture,labels_json,observed_artifact_digest,COALESCE(observed_generation,0) AS observed_generation FROM devices WHERE tenant_id=? AND fleet_id=? AND lifecycle_status NOT IN ('quarantined','decommissioned') ORDER BY stable_key", {auth.context.tenantId, release->value("fleet_id", "")});
    try {
      const auto stageJson = Json::parse(policy->value("stage_plan_json", "[1,5,20,50,100]"));
      std::vector<int> stagePlan; for (const auto& value : stageJson) stagePlan.push_back(value.get<int>());
      const auto policySelector = Json::parse(policy->value("selector_json", "{}"));
      const auto releaseSelector = Json::parse(release->value("target_selector_json", "{}"));
      const auto selector = releaseSelector.empty() ? policySelector : releaseSelector;
      if (!selector.is_object()) return errorResponse(422, "INVALID_SELECTOR", "The release selector must be an object.", traceId);
      std::vector<Json> eligibleDevices;
      for (const auto& device : devices) {
        const auto labels = Json::parse(device.value("labels_json", "{}"));
        if (validSelector(labels, selector)) eligibleDevices.push_back(device);
      }
      if (eligibleDevices.empty()) return errorResponse(422, "EMPTY_COHORT", "No eligible devices matched the frozen selector.", traceId);
      for (const auto& device : eligibleDevices) {
        const auto targetModel = artifact.front().value("hardware_model", "");
        const auto targetArchitecture = artifact.front().value("architecture", "");
        if ((!targetModel.empty() && targetModel != "any" && targetModel != device.value("hardware_model", "")) ||
            (!targetArchitecture.empty() && targetArchitecture != "any" && targetArchitecture != device.value("architecture", ""))) {
          return errorResponse(422, "ARTIFACT_INCOMPATIBLE", "The target artifact is incompatible with a frozen cohort device.", traceId, {{"device_id", device.at("id")}});
        }
        if (!rollbackArtifact.empty()) {
          const auto rollbackModel = rollbackArtifact.front().value("hardware_model", "");
          const auto rollbackArchitecture = rollbackArtifact.front().value("architecture", "");
          if ((!rollbackModel.empty() && rollbackModel != "any" && rollbackModel != device.value("hardware_model", "")) ||
              (!rollbackArchitecture.empty() && rollbackArchitecture != "any" && rollbackArchitecture != device.value("architecture", ""))) {
            return errorResponse(422, "ROLLBACK_ARTIFACT_INCOMPATIBLE", "The rollback artifact is incompatible with a frozen cohort device.", traceId, {{"device_id", device.at("id")} });
          }
        }
      }
      if (rollbackRequirement == "allow_first_install" && rollbackArtifact.empty()) {
        for (const auto& device : eligibleDevices) if (!stringValue(device, "observed_artifact_digest").empty() || device.value("observed_generation", 0LL) != 0) return errorResponse(422, "ROLLBACK_ARTIFACT_REQUIRED", "allow_first_install is valid only for devices with no prior observed installation.", traceId);
      }
      std::vector<domain::CohortDevice> cohortDevices;
      for (const auto& device : eligibleDevices) cohortDevices.push_back({device.at("id"), device.at("stable_key"), device.at("hardware_model"), device.at("architecture"), Json::parse(device.value("labels_json", "{}")), stringValue(device, "observed_artifact_digest"), device.value("observed_generation", 0LL)});
      const auto salt = shared::Uuid::generate().str();
      const auto plan = domain::CohortPlanner::plan(parts[2], salt, std::move(cohortDevices), stagePlan);
      if (!plan.ok()) return errorResponse(plan.error->status, plan.error->code, plan.error->message, traceId);
      const auto nowVersion = release->value("version", 1) + 1;
      const bool committed = storage_->transaction([&] {
        if (!storage_->updateRelease(auth.context.tenantId, parts[2], "validating", release->value("version", 1))) return false;
        if (!storage_->execute("DELETE FROM release_memberships WHERE tenant_id=? AND release_id=?", {auth.context.tenantId, parts[2]}) || !storage_->execute("DELETE FROM release_stages WHERE tenant_id=? AND release_id=?", {auth.context.tenantId, parts[2]})) return false;
        for (const auto& member : plan.value->members) if (!storage_->execute("INSERT INTO release_memberships(id,tenant_id,release_id,device_id,cohort_hash_hex,cohort_ordinal,frozen_labels_json,frozen_observed_digest,frozen_observed_generation,included_at) VALUES(?,?,?,?,?,?,?,?,?,datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, parts[2], member.device.id, member.cohortHash, std::to_string(member.ordinal), shared::CanonicalJson::serialize(member.device.labels), member.device.observedArtifactDigest, std::to_string(member.device.observedGeneration)})) return false;
        for (std::size_t index = 0; index < stagePlan.size(); ++index) if (!storage_->execute("INSERT INTO release_stages(id,tenant_id,release_id,ordinal,target_percentage,status,eligible_count,created_at,updated_at) VALUES(?,?,?,?,?,'pending',?,datetime('now'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, parts[2], std::to_string(index + 1), std::to_string(stagePlan[index]), std::to_string(eligibleDevices.size())})) return false;
        const auto frozenPolicy = shared::CanonicalJson::serialize(*policy);
        const auto frozenManifest = artifact.front().at("manifest_json").get<std::string>();
        const auto frozenRollback = rollbackArtifact.empty() ? std::string{} : rollbackArtifact.front().at("manifest_json").get<std::string>();
        if (!storage_->execute("UPDATE releases SET target_selector_json=?,frozen_policy_json=?,frozen_manifest_json=?,frozen_rollback_json=?,cohort_salt_ciphertext=?,membership_digest=?,eligible_device_count=?,current_stage_ordinal=1,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND status='validating' AND version=?", {shared::CanonicalJson::serialize(selector), frozenPolicy, frozenManifest, frozenRollback, salt, plan.value->digest, std::to_string(eligibleDevices.size()), auth.context.tenantId, parts[2], std::to_string(nowVersion)})) return false;
        if (!storage_->updateRelease(auth.context.tenantId, parts[2], "ready", nowVersion)) return false;
        return storage_->appendEvidence(auth.context.tenantId, "release.validated", "release", parts[2], {{"membership_digest", plan.value->digest}, {"eligible_device_count", eligibleDevices.size()}}, "operator", auth.context.actorId).has_value();
      });
      if (!committed) return errorResponse(409, "RELEASE_VALIDATION_COMMIT_FAILED", "Release validation could not commit its frozen inputs and audit evidence.", traceId);
      return jsonResponse(200, {{"release_id", parts[2]}, {"status", "ready"}, {"membership_digest", plan.value->digest}, {"eligible_device_count", eligibleDevices.size()}, {"stage_count", stagePlan.size()}});
    } catch (const std::exception&) { return errorResponse(422, "INVALID_FROZEN_INPUT", "Policy stages or device labels are not valid JSON.", traceId); }
  }
  if (parts.size() == 5 && parts[3] == "gates" && parts[4] == "evaluate" && request.method == "POST") {
    if (!web::requireRole(auth.context, "live_release", "control")) return errorResponse(403, "FORBIDDEN", "Release control permission is required.", traceId);
    const auto release = storage_->getRelease(auth.context.tenantId, parts[2]);
    if (!release.has_value()) return errorResponse(404, "NOT_FOUND", "Release not found.", traceId);
    Json body;
    try { body = parseBody(request); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    const auto stageOrdinal = body.value("stage_ordinal", release->value("current_stage_ordinal", 1));
    const auto stages = storage_->query("SELECT id FROM release_stages WHERE tenant_id=? AND release_id=? AND ordinal=?", {auth.context.tenantId, parts[2], std::to_string(stageOrdinal)});
    if (stages.empty()) return errorResponse(404, "NOT_FOUND", "Release stage not found.", traceId);
    Json policy = Json::object();
    try { policy = stringValue(*release, "frozen_policy_json").empty() ? storage_->getPolicy(auth.context.tenantId, release->value("policy_id", "")).value_or(Json::object()) : Json::parse(stringValue(*release, "frozen_policy_json", "{}")); }
    catch (const std::exception&) { return errorResponse(409, "FROZEN_POLICY_INVALID", "The release frozen policy cannot be evaluated.", traceId); }
    const auto freshnessSeconds = policy.value("telemetry_freshness_sec", config_.telemetryFreshnessSeconds);
    const bool iotEvidenceRequired = boolValue(policy, "require_iot_evidence", false);
    const auto assignments = storage_->query("SELECT a.id,a.state,a.failure_code,d.last_seen_at FROM release_assignments a JOIN release_stages s ON s.tenant_id=a.tenant_id AND s.id=a.stage_id JOIN devices d ON d.tenant_id=a.tenant_id AND d.id=a.device_id WHERE a.tenant_id=? AND a.release_id=? AND s.ordinal<=?", {auth.context.tenantId, parts[2], std::to_string(stageOrdinal)});
    domain::GateMetrics metrics;
    metrics.assigned = static_cast<int>(assignments.size());
    const auto freshnessSql = "datetime('now','-" + std::to_string(freshnessSeconds) + " seconds')";
    for (const auto& assignment : assignments) {
      if (!assignment.value("last_seen_at", "").empty() && !storage_->query("SELECT 1 AS fresh WHERE ? >= " + freshnessSql, {assignment.value("last_seen_at", "")}).empty()) ++metrics.fresh;
      const auto stateValue = assignment.value("state", "");
      if (stateValue == "failed") ++metrics.installFailures;
      if (stateValue == "converged") ++metrics.converged;
      if (stateValue == "offline" || assignment.value("last_seen_at", "").empty()) ++metrics.offline;
      if (stateValue == "failed" && assignment.value("failure_code", "") == "ROLLBACK_FAILED") ++metrics.rollbackFailures;
    }
    bool iotEvidenceReady = true;
    if (iotEvidenceRequired) {
      const auto configured = storage_->query("SELECT id FROM integration_configs WHERE tenant_id=? AND adapter_type='iot_rest_v1' AND enabled=1 AND required_for_promotion=1 AND health_status='healthy'", {auth.context.tenantId});
      const auto freshIotSamples = storage_->query("SELECT COUNT(DISTINCT h.device_id) AS count FROM health_samples h JOIN release_stages s ON s.tenant_id=h.tenant_id AND s.id=h.stage_id WHERE h.tenant_id=? AND h.release_id=? AND s.ordinal<=? AND h.source='iot_rest_v1' AND h.freshness_state='fresh' AND h.observed_at >= datetime('now','-" + std::to_string(freshnessSeconds) + " seconds')", {auth.context.tenantId, parts[2], std::to_string(stageOrdinal)});
      iotEvidenceReady = !configured.empty() && !freshIotSamples.empty() && freshIotSamples.front().value("count", 0) >= metrics.assigned;
    }
    const auto healthFailures = storage_->query("SELECT COUNT(DISTINCT h.device_id) AS count FROM health_samples h JOIN release_stages s ON s.tenant_id=h.tenant_id AND s.id=h.stage_id WHERE h.tenant_id=? AND h.release_id=? AND s.ordinal<=? AND h.metric_name='health_failure' AND h.metric_value>0 AND h.freshness_state='fresh'", {auth.context.tenantId, parts[2], std::to_string(stageOrdinal)});
    metrics.healthFailures = healthFailures.empty() ? 0 : healthFailures.front().value("count", 0);
    const auto crashFree = storage_->query("SELECT AVG(h.metric_value) AS value FROM health_samples h JOIN release_stages s ON s.tenant_id=h.tenant_id AND s.id=h.stage_id WHERE h.tenant_id=? AND h.release_id=? AND s.ordinal<=? AND h.metric_name IN ('crash_free_percent','crash_free_rate') AND h.freshness_state='fresh'", {auth.context.tenantId, parts[2], std::to_string(stageOrdinal)});
    if (!crashFree.empty() && !crashFree.front().at("value").is_null()) {
      metrics.crashFreePercent = crashFree.front().at("value").get<double>();
      if (metrics.crashFreePercent <= 1.0) metrics.crashFreePercent *= 100.0;
    }
    const auto thresholds = domain::GateEvaluator::thresholdsFromPolicy(policy);
    const auto decision = iotEvidenceReady ? domain::GateEvaluator::evaluate(metrics, thresholds) : domain::GateDecision::insufficient_evidence;
    const auto metricsJson = Json{{"assigned", metrics.assigned}, {"fresh_device_count", metrics.fresh}, {"install_failures", metrics.installFailures}, {"health_failures", metrics.healthFailures}, {"rollback_failures", metrics.rollbackFailures}, {"crash_free_percent", metrics.crashFreePercent}, {"converged", metrics.converged}, {"offline", metrics.offline}};
    Json failed = Json::array();
    for (const auto& gate : domain::GateEvaluator::failedGates(metrics, thresholds)) failed.push_back(gate);
    if (!iotEvidenceReady) failed.push_back("required_iot_evidence");
    const auto evidenceDigest = shared::DigestService::sha256Hex(shared::CanonicalJson::serialize({{"release_id", parts[2]}, {"stage_ordinal", stageOrdinal}, {"metrics", metricsJson}, {"decision", domain::toString(decision)}}));
    const auto id = shared::Uuid::generate().str();
    const auto sql = "INSERT INTO health_gate_evaluations(id,tenant_id,release_id,stage_id,decision,sample_window_start,sample_window_end,sample_count,eligible_device_count,fresh_device_count,metrics_json,failed_gates_json,evidence_digest,evaluated_at) VALUES(?,?,?,?,?,datetime('now','-" + std::to_string(config_.minObservationSeconds) + " seconds'),datetime('now'),?,?,?,?,?,?,datetime('now'))";
    const bool saved = storage_->execute(sql, {id, auth.context.tenantId, parts[2], stages.front().at("id").get<std::string>(), domain::toString(decision), std::to_string(metrics.assigned), std::to_string(metrics.assigned), std::to_string(metrics.fresh), metricsJson.dump(), failed.dump(), evidenceDigest});
    if (!saved) return errorResponse(409, "GATE_EVALUATION_FAILED", storage_->lastError(), traceId);
    if (!storage_->appendEvidence(auth.context.tenantId, "release.gate.evaluated", "release", parts[2], {{"evaluation_id", id}, {"decision", domain::toString(decision)}, {"evidence_digest", evidenceDigest}}, "operator", auth.context.actorId).has_value()) return errorResponse(500, "EVIDENCE_COMMIT_FAILED", "The gate evaluation could not write audit evidence.", traceId);
    return jsonResponse(201, {{"id", id}, {"stage_ordinal", stageOrdinal}, {"decision", domain::toString(decision)}, {"metrics", metricsJson}, {"failed_gates", failed}, {"evidence_digest", evidenceDigest}});
  }
  if (parts.size() == 6 && parts[3] == "gates" && parts[5] == "override" && request.method == "POST") {
    if (!web::requireRole(auth.context, "live_release", "control")) return errorResponse(403, "FORBIDDEN", "Release control permission is required.", traceId);
    const auto evaluation = storage_->query("SELECT id,decision,evidence_digest FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? AND id=?", {auth.context.tenantId, parts[2], parts[4]});
    if (evaluation.empty()) return errorResponse(404, "NOT_FOUND", "Gate evaluation not found.", traceId);
    if (evaluation.front().value("decision", "") != "pause" && evaluation.front().value("decision", "") != "insufficient_evidence") return errorResponse(409, "GATE_OVERRIDE_NOT_ALLOWED", "Only pause or insufficient-evidence evaluations may be overridden.", traceId);
    const auto release = storage_->getRelease(auth.context.tenantId, parts[2]);
    if (!release.has_value()) return errorResponse(404, "NOT_FOUND", "Release not found.", traceId);
    Json body;
    try { body = parseBody(request); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    const auto reason = body.value("reason", "");
    if (reason.empty()) return errorResponse(422, "REASON_REQUIRED", "A gate override requires a reason.", traceId);
    if (!body.contains("expected_version") || !body.at("expected_version").is_number_integer()) return errorResponse(422, "EXPECTED_VERSION_REQUIRED", "Gate overrides require expected_version.", traceId);
    if (body.at("expected_version").get<int>() != release->value("version", 1)) return errorResponse(409, "RELEASE_VERSION_CONFLICT", "The release version is stale.", traceId);
    const auto pending = storage_->query("SELECT id FROM approval_requests WHERE tenant_id=? AND release_id=? AND action='gate_override' AND gate_evaluation_id=? AND status='requested' AND expires_at > datetime('now')", {auth.context.tenantId, parts[2], parts[4]});
    if (!pending.empty()) return jsonResponse(202, {{"id", pending.front().at("id")}, {"status", "requested"}});
    const auto id = shared::Uuid::generate().str();
    const bool created = storage_->transaction([&] {
      if (!storage_->execute("INSERT INTO approval_requests(id,tenant_id,release_id,action,status,captured_release_version,gate_evaluation_id,requested_by_actor_id,request_reason,evidence_digest,expires_at,created_at,updated_at) SELECT ?,tenant_id,id,'gate_override','requested',version,?,?,?, ?,datetime('now','+30 minutes'),datetime('now'),datetime('now') FROM releases WHERE tenant_id=? AND id=?", {id, parts[4], auth.context.actorId, reason, evaluation.front().at("evidence_digest").get<std::string>(), auth.context.tenantId, parts[2]})) return false;
      return storage_->appendEvidence(auth.context.tenantId, "release.gate_override_approval_requested", "release", parts[2], {{"approval_request_id", id}, {"evaluation_id", parts[4]}, {"release_version", release->value("version", 1)}, {"reason", reason}}, "operator", auth.context.actorId).has_value();
    });
    return created ? jsonResponse(202, {{"id", id}, {"status", "requested"}}) : errorResponse(409, "APPROVAL_CREATE_FAILED", storage_->lastError(), traceId);
  }
  if (parts.size() == 4 && request.method == "POST") {
    if (!web::requireRole(auth.context, "live_release", "control")) return errorResponse(403, "FORBIDDEN", "Release control permission is required.", traceId);
    const auto action = releaseAction(parts[3]);
    if (!action.has_value()) return errorResponse(404, "NOT_FOUND", "Release action does not exist.", traceId);
    const auto release = storage_->getRelease(auth.context.tenantId, parts[2]);
    if (!release.has_value()) return errorResponse(404, "NOT_FOUND", "Release not found.", traceId);
    const auto current = release->value("status", "draft");
    static const std::vector<domain::ReleaseState> states{domain::ReleaseState::draft, domain::ReleaseState::validating, domain::ReleaseState::blocked, domain::ReleaseState::ready, domain::ReleaseState::awaiting_approval, domain::ReleaseState::scheduled, domain::ReleaseState::running, domain::ReleaseState::paused, domain::ReleaseState::aborting, domain::ReleaseState::rolling_back, domain::ReleaseState::completed, domain::ReleaseState::aborted, domain::ReleaseState::rolled_back, domain::ReleaseState::failed, domain::ReleaseState::cancelled};
    const auto state = std::find_if(states.begin(), states.end(), [&](domain::ReleaseState candidate) { return domain::toString(candidate) == current; });
    if (state == states.end()) return errorResponse(409, "INVALID_RELEASE_STATE", "Release status is not recognized.", traceId);
    int version = release->value("version", 1);
    Json body;
    try { body = parseBody(request); } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
    if (!body.contains("expected_version") || !body.at("expected_version").is_number_integer()) return errorResponse(422, "EXPECTED_VERSION_REQUIRED", "Release control actions require expected_version.", traceId);
    const auto expectedVersion = body.at("expected_version").get<int>();
    if (expectedVersion != version) return errorResponse(409, "RELEASE_VERSION_CONFLICT", "The release version is stale.", traceId, {{"current_version", version}});
    if (*action != domain::ReleaseAction::validate && !hasReason(body)) return errorResponse(422, "REASON_REQUIRED", "A release control action requires a reason.", traceId);
    const auto policy = storage_->getPolicy(auth.context.tenantId, release->value("policy_id", ""));
    const bool requiresApproval = policy.has_value() && policy->value("two_person_approval", 1) != 0;
    const bool controlApproval = requiresApproval && (*action == domain::ReleaseAction::schedule || *action == domain::ReleaseAction::start || *action == domain::ReleaseAction::resume || *action == domain::ReleaseAction::abort || *action == domain::ReleaseAction::rollback);
    if (controlApproval) {
      const auto approvalAction = (*action == domain::ReleaseAction::schedule || *action == domain::ReleaseAction::start) ? std::string("start") : parts[3];
      const auto approved = storage_->query("SELECT id FROM approval_requests WHERE tenant_id=? AND release_id=? AND action=? AND status='approved' AND approved_release_version=? AND consumed_at IS NULL AND expires_at > datetime('now')", {auth.context.tenantId, parts[2], approvalAction, std::to_string(version)});
      if (approved.empty()) {
        if (*action == domain::ReleaseAction::schedule) return errorResponse(409, "START_APPROVAL_REQUIRED", "Scheduling requires an approved start authorization for this release version.", traceId);
        const auto pending = storage_->query("SELECT id FROM approval_requests WHERE tenant_id=? AND release_id=? AND action=? AND status='requested' AND expires_at > datetime('now') ORDER BY created_at DESC LIMIT 1", {auth.context.tenantId, parts[2], approvalAction});
        if (!pending.empty()) return jsonResponse(202, {{"release_id", parts[2]}, {"status", "awaiting_approval"}, {"approval_request_id", pending.front().at("id")}});
        const auto requestId = shared::Uuid::generate().str();
        const auto capturedVersion = current == "ready" && *action == domain::ReleaseAction::start ? version + 1 : version;
        const bool created = storage_->transaction([&] {
          if (!storage_->execute("INSERT INTO approval_requests(id,tenant_id,release_id,action,status,captured_release_version,requested_by_actor_id,request_reason,evidence_digest,expires_at,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,datetime('now','+24 hours'),datetime('now'),datetime('now'))", {requestId, auth.context.tenantId, parts[2], approvalAction, "requested", std::to_string(capturedVersion), auth.context.actorId, body.value("reason", std::string("Release ") + parts[3] + " requested"), stringValue(*release, "membership_digest")})) return false;
          if (*action == domain::ReleaseAction::start && current == "ready" && !storage_->updateRelease(auth.context.tenantId, parts[2], "awaiting_approval", version)) return false;
          return storage_->appendEvidence(auth.context.tenantId, "release." + parts[3] + "_approval_requested", "release", parts[2], {{"approval_request_id", requestId}, {"release_version", version}}, "operator", auth.context.actorId).has_value();
        });
        if (!created) return errorResponse(409, "APPROVAL_CREATE_FAILED", "The start approval request and audit evidence could not be stored.", traceId);
        return jsonResponse(202, {{"release_id", parts[2]}, {"status", current == "ready" && *action == domain::ReleaseAction::start ? "awaiting_approval" : current}, {"approval_request_id", requestId}});
      }
      if (approved.empty()) return errorResponse(409, "APPROVAL_REQUIRED", "A valid approval for this release version is required.", traceId);
    }
    if (*action == domain::ReleaseAction::resume) {
      const auto gate = storage_->query("SELECT decision FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? ORDER BY evaluated_at DESC LIMIT 1", {auth.context.tenantId, parts[2]});
      if (gate.empty() || gate.front().value("decision", "") != "pass") return errorResponse(409, "FRESH_EVIDENCE_REQUIRED", "Resume requires a fresh passing health evaluation.", traceId);
    }
    if (*action == domain::ReleaseAction::rollback) {
      if (stringValue(*release, "frozen_rollback_json").empty()) return errorResponse(422, "ROLLBACK_ARTIFACT_REQUIRED", "No trusted frozen rollback artifact is available.", traceId);
      const auto rollback = storage_->query("SELECT a.id,a.sha256_digest,a.status,k.status AS key_status FROM artifacts a JOIN artifact_signing_keys k ON k.tenant_id=a.tenant_id AND k.id=a.signature_key_id WHERE a.tenant_id=? AND a.id=?", {auth.context.tenantId, stringValue(*release, "rollback_artifact_id")});
      if (rollback.empty() || rollback.front().value("status", "") != "ready" || rollback.front().value("key_status", "") != "active") return errorResponse(422, "ROLLBACK_ARTIFACT_NOT_READY", "The frozen rollback artifact is no longer trusted.", traceId);
    }
    int observationSeconds = config_.minObservationSeconds;
    if (!stringValue(*release, "frozen_policy_json").empty()) {
      try {
        observationSeconds = std::clamp(Json::parse(stringValue(*release, "frozen_policy_json")).value("min_observation_sec", observationSeconds), 1, 7 * 24 * 60 * 60);
      } catch (const std::exception&) {
        return errorResponse(409, "FROZEN_POLICY_INVALID", "The release frozen policy cannot be evaluated.", traceId);
      }
    }
    const auto next = domain::ReleaseStateMachine::transition(*state, *action);
    if (!next.has_value()) return errorResponse(409, "ILLEGAL_RELEASE_TRANSITION", "The requested action is not valid in the current state.", traceId, {{"current_state", current}, {"action", parts[3]}});
    if (*action == domain::ReleaseAction::schedule && body.value("scheduled_for", "").empty()) return errorResponse(422, "SCHEDULE_REQUIRED", "scheduled_for is required.", traceId);
    const bool committed = storage_->transaction([&] {
      if (!storage_->updateRelease(auth.context.tenantId, parts[2], domain::toString(*next), version)) return false;
      if (*action == domain::ReleaseAction::submit) {
        const auto requestId = shared::Uuid::generate().str();
        if (!storage_->execute("INSERT INTO approval_requests(id,tenant_id,release_id,action,status,captured_release_version,requested_by_actor_id,request_reason,evidence_digest,expires_at,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,datetime('now','+24 hours'),datetime('now'),datetime('now'))", {requestId, auth.context.tenantId, parts[2], "start", "requested", std::to_string(version + 1), auth.context.actorId, body.value("reason", "Release start requested"), stringValue(*release, "membership_digest")})) return false;
      }
      if (*action == domain::ReleaseAction::schedule && !storage_->execute("UPDATE releases SET scheduled_for=? WHERE tenant_id=? AND id=? AND version=?", {body.value("scheduled_for", ""), auth.context.tenantId, parts[2], std::to_string(version + 1)})) return false;
      if (*action == domain::ReleaseAction::cancel && !storage_->execute("UPDATE approval_requests SET status='superseded',updated_at=datetime('now') WHERE tenant_id=? AND release_id=? AND status='requested'", {auth.context.tenantId, parts[2]})) return false;
      if (*action == domain::ReleaseAction::start && next.value() == domain::ReleaseState::running) {
        const auto stages = storage_->query("SELECT id,target_percentage FROM release_stages WHERE tenant_id=? AND release_id=? AND ordinal=1", {auth.context.tenantId, parts[2]});
        const auto memberships = storage_->query("SELECT device_id FROM release_memberships WHERE tenant_id=? AND release_id=? ORDER BY cohort_ordinal", {auth.context.tenantId, parts[2]});
        if (!stages.empty()) {
          if (!storage_->execute("UPDATE release_stages SET status='active',started_at=COALESCE(started_at,datetime('now')),observation_started_at=COALESCE(observation_started_at,datetime('now')),observation_ends_at=COALESCE(observation_ends_at,datetime('now','+" + std::to_string(observationSeconds) + " seconds')),updated_at=datetime('now') WHERE tenant_id=? AND release_id=? AND ordinal=1 AND status='pending'", {auth.context.tenantId, parts[2]})) return false;
          const auto targetPercentage = stages.front().value("target_percentage", 1);
          const auto targetCount = std::min(memberships.size(), std::max<std::size_t>(1, (memberships.size() * static_cast<std::size_t>(targetPercentage) + 99U) / 100U));
          for (std::size_t index = 0; index < targetCount; ++index) {
            const auto deviceId = memberships[index].at("device_id").get<std::string>();
            const auto device = storage_->getDevice(auth.context.tenantId, deviceId);
            if (!device.has_value()) return false;
            const auto generation = std::max<long long>(1, device->value("desired_generation", 0LL) + 1);
            const auto assignmentId = shared::Uuid::generate().str();
            const auto inserted = storage_->execute("INSERT OR IGNORE INTO release_assignments(id,tenant_id,release_id,stage_id,device_id,desired_artifact_id,desired_generation,state,commanded_at,updated_at) SELECT ?,tenant_id,?,?,?,artifact_id,?,'commanded',datetime('now'),datetime('now') FROM releases WHERE tenant_id=? AND id=?", {assignmentId, parts[2], stages.front().at("id").get<std::string>(), deviceId, std::to_string(generation), auth.context.tenantId, parts[2]});
            if (inserted) {
              if (!storage_->execute("UPDATE devices SET desired_generation=CASE WHEN desired_generation<? THEN ? ELSE desired_generation END,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {std::to_string(generation), std::to_string(generation), auth.context.tenantId, deviceId})) return false;
              if (!storage_->execute("INSERT OR IGNORE INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) SELECT ?,tenant_id,release_id,stage_id,id,?,'install',desired_generation,desired_artifact_id,?, ?,datetime('now'),datetime('now','+7 days'),datetime('now') FROM release_assignments WHERE tenant_id=? AND id=?", {shared::Uuid::generate().str(), deviceId, shared::CanonicalJson::serialize({{"type", "install"}, {"assignment_id", assignmentId}, {"generation", generation}}), "release-install-" + assignmentId, auth.context.tenantId, assignmentId})) return false;
            }
          }
          if (!storage_->execute("UPDATE release_stages SET assigned_count=(SELECT COUNT(*) FROM release_assignments WHERE tenant_id=? AND release_id=? AND stage_id=?),updated_at=datetime('now') WHERE tenant_id=? AND release_id=? AND ordinal=1", {auth.context.tenantId, parts[2], stages.front().at("id").get<std::string>(), auth.context.tenantId, parts[2]})) return false;
        }
      }
      if (*action == domain::ReleaseAction::rollback) {
        const auto assignments = storage_->query("SELECT id,device_id,stage_id FROM release_assignments WHERE tenant_id=? AND release_id=? AND state NOT IN ('rolled_back','superseded')", {auth.context.tenantId, parts[2]});
        for (const auto& assignment : assignments) {
          const auto device = storage_->getDevice(auth.context.tenantId, assignment.at("device_id").get<std::string>());
          if (!device.has_value()) return false;
          const auto generation = device->value("desired_generation", 0LL) + 1;
          if (!storage_->execute("UPDATE release_assignments SET desired_artifact_id=?,desired_generation=?,state='pending',updated_at=datetime('now') WHERE tenant_id=? AND id=?", {stringValue(*release, "rollback_artifact_id"), std::to_string(generation), auth.context.tenantId, assignment.at("id").get<std::string>()}) ||
              !storage_->execute("UPDATE devices SET desired_generation=CASE WHEN desired_generation<? THEN ? ELSE desired_generation END,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {std::to_string(generation), std::to_string(generation), auth.context.tenantId, assignment.at("device_id").get<std::string>()}) ||
              !storage_->execute("INSERT OR IGNORE INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) VALUES(?,?,?,?,? ,?,'rollback',?,?,?, ?,datetime('now'),datetime('now','+7 days'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, parts[2], assignment.at("stage_id").get<std::string>(), assignment.at("id").get<std::string>(), assignment.at("device_id").get<std::string>(), std::to_string(generation), stringValue(*release, "rollback_artifact_id"), "{}", "rollback-" + assignment.at("id").get<std::string>() + "-" + std::to_string(generation)})) return false;
        }
      }
      if (*action == domain::ReleaseAction::abort) {
        const auto assignments = storage_->query("SELECT id,stage_id,device_id,desired_generation FROM release_assignments WHERE tenant_id=? AND release_id=? AND state IN ('pending','commanded','acknowledged')", {auth.context.tenantId, parts[2]});
        for (const auto& assignment : assignments) {
          const auto assignmentId = assignment.at("id").get<std::string>();
          const auto generation = assignment.at("desired_generation").get<long long>();
          if (!storage_->execute("UPDATE release_assignments SET state='cancelling',updated_at=datetime('now') WHERE tenant_id=? AND id=? AND state IN ('pending','commanded','acknowledged')", {auth.context.tenantId, assignmentId}) ||
              !storage_->execute("INSERT OR IGNORE INTO rollout_commands(id,tenant_id,release_id,stage_id,assignment_id,device_id,command_type,desired_generation,artifact_id,payload_json,idempotency_key,not_before,expires_at,issued_at) VALUES(?,?,?,?,?,?, 'cancel',?,NULLIF(?,''),?,?,datetime('now'),datetime('now','+7 days'),datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, parts[2], assignment.at("stage_id").get<std::string>(), assignmentId, assignment.at("device_id").get<std::string>(), std::to_string(generation), "", shared::CanonicalJson::serialize({{"type", "cancel"}, {"assignment_id", assignmentId}, {"generation", generation}}), "cancel-" + assignmentId + "-" + std::to_string(generation)})) return false;
        }
      }
      if (*action == domain::ReleaseAction::start || *action == domain::ReleaseAction::schedule || *action == domain::ReleaseAction::resume || *action == domain::ReleaseAction::abort || *action == domain::ReleaseAction::rollback) {
        const auto approvalAction = (*action == domain::ReleaseAction::schedule || *action == domain::ReleaseAction::start) ? std::string("start") : parts[3];
        if (!storage_->execute("UPDATE approval_requests SET consumed_at=datetime('now') WHERE tenant_id=? AND release_id=? AND action=? AND status='approved' AND approved_release_version=? AND consumed_at IS NULL", {auth.context.tenantId, parts[2], approvalAction, std::to_string(version)})) return false;
      }
      return storage_->appendEvidence(auth.context.tenantId, "release." + parts[3], "release", parts[2], {{"from", current}, {"to", domain::toString(*next)}, {"actor_id", auth.context.actorId}, {"release_version", version + 1}}, "operator", auth.context.actorId).has_value();
    });
    if (!committed) return errorResponse(409, "RELEASE_ACTION_COMMIT_FAILED", "The release action could not be committed with its side effects and audit evidence.", traceId);
    return jsonResponse(200, storage_->getRelease(auth.context.tenantId, parts[2]).value_or(Json::object()));
  }
  return errorResponse(404, "NOT_FOUND", "The release route does not exist.", traceId);
}

HttpResponse ControlPlane::simulationRoute(const HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts) {
  const auto traceId = auth.traceId;
  if (parts.size() == 2 && request.method == "GET") { if (!web::requireRole(auth.context, "simulation", "read")) return errorResponse(403, "FORBIDDEN", "Simulation read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->query("SELECT id,scenario_name,status,seed,input_digest,result_digest,trace_digest,created_at,completed_at FROM simulation_runs WHERE tenant_id=? ORDER BY created_at DESC", {auth.context.tenantId})}}); }
  if (parts.size() == 2 && request.method == "POST") {
    if (!web::requireRole(auth.context, "simulation", "write")) return errorResponse(403, "FORBIDDEN", "Simulation write permission is required.", traceId);
    try {
      const auto body = parseBody(request);
      const auto seed = body.value("seed", 1ULL);
      const auto input = body.value("input", body);
      const auto validation = domain::Simulator::run(input, seed);
      if (!validation.ok()) return errorResponse(validation.error->status, validation.error->code, validation.error->message, traceId);
      const auto id = shared::Uuid::generate().str();
      const auto serialized = shared::CanonicalJson::serialize(input);
      const bool saved = storage_->execute("INSERT INTO simulation_runs(id,tenant_id,scenario_name,scenario_version,status,seed,input_json,input_digest,simulator_version,result_json,result_digest,trace_digest,requested_by_actor_id,started_at,completed_at,created_at) VALUES(?,?,?,?,?,?,?,?,?,NULL,NULL,NULL,?,NULL,NULL,datetime('now'))", {id, auth.context.tenantId, body.value("scenario_name", "adhoc"), body.value("scenario_version", "1"), "queued", std::to_string(seed), serialized, shared::DigestService::sha256Hex(serialized), validation.value->simulatorVersion, auth.context.actorId});
      return saved ? jsonResponse(202, {{"id", id}, {"status", "queued"}, {"input_digest", shared::DigestService::sha256Hex(serialized)}, {"simulator_version", validation.value->simulatorVersion}}) : errorResponse(409, "SIMULATION_SAVE_FAILED", storage_->lastError(), traceId);
    } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Simulation input must be valid JSON.", traceId); }
  }
  if (parts.size() == 3 && request.method == "GET") {
    if (!web::requireRole(auth.context, "simulation", "read")) return errorResponse(403, "FORBIDDEN", "Simulation read permission is required.", traceId);
    const auto result = storage_->query("SELECT id,scenario_name,scenario_version,status,seed,input_json,input_digest,simulator_version,result_json,result_digest,trace_digest,created_at,completed_at FROM simulation_runs WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[2]});
    return result.empty() ? errorResponse(404, "NOT_FOUND", "Simulation run not found.", traceId) : jsonResponse(200, result.front());
  }
  if (parts.size() == 4 && request.method == "POST" && (parts[3] == "cancel" || parts[3] == "replay")) {
    if (!web::requireRole(auth.context, "simulation", "write")) return errorResponse(403, "FORBIDDEN", "Simulation control permission is required.", traceId);
    if (parts[3] == "cancel") return storage_->execute("UPDATE simulation_runs SET status='cancelled',completed_at=datetime('now') WHERE tenant_id=? AND id=? AND status IN ('queued','running')", {auth.context.tenantId, parts[2]}) ? jsonResponse(200, {{"id", parts[2]}, {"status", "cancelled"}}) : errorResponse(409, "SIMULATION_CANCEL_FAILED", "The simulation is no longer cancellable.", traceId);
    const auto source = storage_->query("SELECT input_json,seed,result_digest FROM simulation_runs WHERE tenant_id=? AND id=? AND status='completed'", {auth.context.tenantId, parts[2]});
    if (source.empty()) return errorResponse(404, "NOT_FOUND", "Completed simulation run not found.", traceId);
    try {
      const auto replayId = shared::Uuid::generate().str();
      const bool saved = storage_->execute("INSERT INTO replay_runs(id,tenant_id,simulation_run_id,source_kind,status,expected_decision_digest,created_at) VALUES(?,?,?,'simulation','queued',?,datetime('now'))", {replayId, auth.context.tenantId, parts[2], source.front().value("result_digest", "")});
      return saved ? jsonResponse(202, {{"id", replayId}, {"status", "queued"}, {"expected_digest", source.front().value("result_digest", "")}}) : errorResponse(409, "REPLAY_CREATE_FAILED", storage_->lastError(), traceId);
    } catch (const std::exception&) { return errorResponse(422, "REPLAY_SOURCE_INVALID", "The frozen simulation input cannot be replayed.", traceId); }
  }
  return errorResponse(404, "NOT_FOUND", "The simulation route does not exist.", traceId);
}

HttpResponse ControlPlane::evidenceRoute(const HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts) {
  const auto traceId = auth.traceId;
  if (parts.size() == 2 && request.method == "GET") { if (!web::requireRole(auth.context, "evidence", "read")) return errorResponse(403, "FORBIDDEN", "Evidence read permission is required.", traceId); return jsonResponse(200, {{"items", storage_->query("SELECT id,sequence_no,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no", {auth.context.tenantId})}}); }
  if (parts.size() == 2 && request.method == "POST") { if (!web::requireRole(auth.context, "evidence", "write")) return errorResponse(403, "FORBIDDEN", "Evidence write permission is required.", traceId); try { const auto body = parseBody(request); const auto event = storage_->appendEvidence(auth.context.tenantId, body.value("event_type", "operator.note"), body.value("aggregate_type", "tenant"), body.value("aggregate_id", auth.context.tenantId), body.value("payload", body), "operator", auth.context.actorId); if (!event.has_value()) ++evidenceAppendFailures_; return event.has_value() ? jsonResponse(201, *event) : errorResponse(409, "EVIDENCE_APPEND_FAILED", storage_->lastError(), traceId); } catch (const std::exception&) { ++evidenceAppendFailures_; return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); } }
  if (parts.size() == 3 && parts[2] == "verify" && request.method == "POST") { if (!web::requireRole(auth.context, "evidence", "read")) return errorResponse(403, "FORBIDDEN", "Evidence read permission is required.", traceId); return jsonResponse(200, storage_->verifyEvidence(auth.context.tenantId)); }
  if (parts.size() == 3 && parts[2] == "exports" && request.method == "POST") {
    if (!web::requireRole(auth.context, "evidence_exports", "write")) return errorResponse(403, "FORBIDDEN", "Evidence export permission is required.", traceId);
    const auto tenant = storage_->getTenant(auth.context.tenantId).value_or(Json::object());
    const auto events = storage_->query("SELECT id,sequence_no,tenant_id,aggregate_type,aggregate_id,event_type,actor_type,actor_id,payload_json,occurred_at,trace_id,previous_hash,event_hash FROM evidence_events WHERE tenant_id=? ORDER BY sequence_no", {auth.context.tenantId});
    const auto id = shared::Uuid::generate().str();
    const bool saved = storage_->execute("INSERT INTO evidence_exports(id,tenant_id,status,source_event_from,source_event_to,source_chain_head_hash,aggregate_filters_json,tenant_snapshot_json,requested_by_actor_id,created_at) VALUES(?,?, 'queued',?,?,?,?,?,?,datetime('now'))", {id, auth.context.tenantId, events.empty() ? "0" : std::to_string(events.front().at("sequence_no").get<int>()), events.empty() ? "0" : std::to_string(events.back().at("sequence_no").get<int>()), events.empty() ? std::string(64, '0') : events.back().at("event_hash").get<std::string>(), "{}", shared::CanonicalJson::serialize(tenant), auth.context.actorId});
    return saved ? jsonResponse(202, {{"id", id}, {"status", "queued"}, {"event_count", events.size()}}) : errorResponse(409, "EVIDENCE_EXPORT_FAILED", storage_->lastError(), traceId);
  }
  if (parts.size() == 4 && parts[2] == "exports" && request.method == "GET") {
    if (!web::requireRole(auth.context, "evidence", "read")) return errorResponse(403, "FORBIDDEN", "Evidence export permission is required.", traceId);
    const auto result = storage_->query("SELECT id,status,source_event_from,source_event_to,source_chain_head_hash,tenant_snapshot_json,chain_manifest_json,output_sha256,created_at,completed_at FROM evidence_exports WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[3]});
    return result.empty() ? errorResponse(404, "NOT_FOUND", "Evidence export not found.", traceId) : jsonResponse(200, result.front());
  }
  if (parts.size() == 5 && parts[2] == "exports" && parts[4] == "download" && request.method == "GET") {
    if (!web::requireRole(auth.context, "evidence", "read")) return errorResponse(403, "FORBIDDEN", "Evidence export permission is required.", traceId);
    const auto result = storage_->query("SELECT output_storage_key,output_sha256,status FROM evidence_exports WHERE tenant_id=? AND id=?", {auth.context.tenantId, parts[3]});
    if (result.empty() || result.front().value("status", "") != "completed") return errorResponse(404, "NOT_FOUND", "Completed evidence export not found.", traceId);
    std::ifstream input(result.front().at("output_storage_key").get<std::string>(), std::ios::binary); std::ostringstream content; content << input.rdbuf();
    return {200, "application/x-ndjson", content.str(), {{"Content-Disposition", "attachment; filename=\"" + parts[3] + ".ndjson\""}, {"X-Content-SHA256", result.front().at("output_sha256").get<std::string>()}}};
  }
  return errorResponse(404, "NOT_FOUND", "The evidence route does not exist.", traceId);
}

HttpResponse ControlPlane::agentRoute(const HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts) {
  const auto traceId = auth.traceId;
  if (!auth.context.authenticated || auth.context.role != shared::Role::device) return errorResponse(403, "FORBIDDEN", "Device protocol credentials are required.", traceId);
  if (parts.size() == 4 && parts[3] == "desired-state" && request.method == "GET") {
    const auto deviceId = header(request, "x-device-id");
    if (deviceId.empty()) return errorResponse(400, "DEVICE_ID_REQUIRED", "X-Device-Id is required.", traceId);
    if (deviceId != auth.context.actorId) return errorResponse(404, "NOT_FOUND", "Device not found.", traceId);
    const auto device = storage_->getDevice(auth.context.tenantId, deviceId);
    if (!device.has_value()) return errorResponse(404, "NOT_FOUND", "Device not found.", traceId);
    const auto assignments = storage_->query("SELECT a.id,a.release_id,a.desired_generation,a.desired_artifact_id,r.name,a.state,c.id AS command_id,c.command_type,c.payload_json,c.not_before,c.expires_at FROM release_assignments a JOIN releases r ON r.tenant_id=a.tenant_id AND r.id=a.release_id JOIN rollout_commands c ON c.tenant_id=a.tenant_id AND c.assignment_id=a.id AND c.desired_generation=a.desired_generation WHERE a.tenant_id=? AND a.device_id=? AND a.desired_generation>=? AND a.state IN ('pending','commanded','acknowledged') AND c.not_before <= datetime('now') AND c.expires_at > datetime('now') ORDER BY a.desired_generation DESC,c.issued_at DESC LIMIT 1", {auth.context.tenantId, deviceId, std::to_string(device->value("desired_generation", 0))});
    if (assignments.empty()) return {204, "application/json", "", {{"X-Server-Time", shared::TenantClock::nowIso8601()}}};
    const auto artifact = storage_->query("SELECT sha256_digest,manifest_json FROM artifacts WHERE tenant_id=? AND id=?", {auth.context.tenantId, assignments.front().at("desired_artifact_id").get<std::string>()});
    if (artifact.empty()) return errorResponse(503, "ARTIFACT_UNAVAILABLE", "The assigned artifact metadata is unavailable.", traceId);
    return jsonResponse(200, {{"device_id", deviceId}, {"release_id", assignments.front().at("release_id")}, {"assignment_id", assignments.front().at("id")}, {"command_id", assignments.front().at("command_id")}, {"command_type", assignments.front().at("command_type")}, {"desired_generation", assignments.front().at("desired_generation")}, {"artifact_id", assignments.front().at("desired_artifact_id")}, {"artifact_digest", artifact.front().at("sha256_digest")}, {"manifest", Json::parse(artifact.front().at("manifest_json").get<std::string>())}, {"download_path", "/api/agent/v1/artifacts/" + artifact.front().at("sha256_digest").get<std::string>()}, {"payload", Json::parse(assignments.front().at("payload_json").get<std::string>())}, {"not_before", assignments.front().at("not_before")}, {"expires_at", assignments.front().at("expires_at")}, {"release_name", assignments.front().at("name")}, {"server_time", shared::TenantClock::nowIso8601()}});
  }
  if (parts.size() == 5 && parts[3] == "artifacts" && request.method == "GET") {
    const auto artifact = storage_->query("SELECT a.storage_key,a.sha256_digest,a.status,a.file_name FROM artifacts a JOIN release_assignments r ON r.tenant_id=a.tenant_id AND r.desired_artifact_id=a.id WHERE a.tenant_id=? AND r.device_id=? AND a.sha256_digest=? AND a.status IN ('ready','retired') ORDER BY r.desired_generation DESC LIMIT 1", {auth.context.tenantId, auth.context.actorId, parts[4]});
    if (artifact.empty()) return errorResponse(404, "NOT_FOUND", "Assigned artifact not found.", traceId);
    std::ifstream input(artifact.front().at("storage_key").get<std::string>(), std::ios::binary);
    if (!input.is_open() || !shared::DigestService::constantTimeEqual(shared::DigestService::sha256File(artifact.front().at("storage_key").get<std::string>()), parts[4])) return errorResponse(503, "ARTIFACT_CORRUPT", "The assigned artifact failed its integrity check.", traceId);
    std::error_code fileError;
    const auto fileSize = std::filesystem::file_size(artifact.front().at("storage_key").get<std::string>(), fileError);
    if (fileError || fileSize == 0) return errorResponse(503, "ARTIFACT_UNAVAILABLE", "The assigned artifact bytes are temporarily unavailable.", traceId);
    int status = 200;
    std::uintmax_t start = 0;
    std::uintmax_t end = fileSize - 1;
    std::map<std::string, std::string> responseHeaders{{"ETag", "\"" + parts[4] + "\""}, {"X-Artifact-SHA256", parts[4]}, {"Content-Disposition", "attachment; filename=\"" + artifact.front().at("file_name").get<std::string>() + "\""}, {"Accept-Ranges", "bytes"}};
    const auto range = header(request, "range");
    if (range.starts_with("bytes=")) {
      try {
        if (range.find(',', 6) != std::string::npos) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
        const auto dash = range.find('-', 6);
        if (dash == std::string::npos) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
        const auto startText = range.substr(6, dash - 6);
        const auto endText = range.substr(dash + 1);
        if (startText.empty()) {
          const auto suffixValue = parseByteOffset(endText);
          if (!suffixValue.has_value()) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
          const auto suffix = *suffixValue;
          if (suffix == 0) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
          start = suffix >= fileSize ? 0 : fileSize - suffix;
        } else {
          const auto startValue = parseByteOffset(startText);
          if (!startValue.has_value()) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
          start = *startValue;
          if (!endText.empty()) {
            const auto endValue = parseByteOffset(endText);
            if (!endValue.has_value()) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
            end = *endValue;
          }
        }
        if (start >= fileSize || end < start) return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId);
        const auto boundedEnd = std::min<std::uintmax_t>(end, fileSize - 1);
        status = 206;
        responseHeaders["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(boundedEnd) + "/" + std::to_string(fileSize);
      } catch (const std::exception&) { return errorResponse(416, "RANGE_NOT_SATISFIABLE", "The requested artifact range is invalid.", traceId); }
    }
    return {status, "application/octet-stream", {}, responseHeaders, artifact.front().at("storage_key").get<std::string>(), start, end - start + 1};
  }
  if (parts.size() == 4 && parts[3] == "reports" && request.method == "POST") {
    try {
      const auto reportSize = request.bodySize == 0 ? request.body.size() : request.bodySize;
      if (reportSize > 256 * 1024) return errorResponse(413, "REPORT_TOO_LARGE", "Device reports are limited to 256 KiB.", traceId);
      const auto body = parseBody(request);
      const auto deviceId = body.value("device_id", header(request, "x-device-id"));
      if (deviceId.empty() || deviceId != auth.context.actorId) return errorResponse(404, "NOT_FOUND", "Device not found.", traceId);
      const auto reportId = body.value("report_id", std::string{});
      const auto sequence = body.value("report_sequence", 0LL);
      const auto observedGeneration = body.value("observed_generation", 0LL);
      const auto reportType = body.value("report_type", "observation");
      static const std::vector<std::string> reportTypes{"command_received", "install_started", "install_result", "observation", "health", "health_sample", "rollback_result", "credential_rotation_ack"};
      if (reportId.empty() || sequence < 1 || observedGeneration < 0 || std::find(reportTypes.begin(), reportTypes.end(), reportType) == reportTypes.end()) return errorResponse(422, "INVALID_REPORT", "Report identifiers, sequences, generations, and report type must be valid.", traceId);
      if (header(request, "x-device-sequence") != std::to_string(sequence)) return errorResponse(409, "DEVICE_SEQUENCE_MISMATCH", "The signed device sequence must match the report sequence.", traceId);
      auto releaseId = body.value("release_id", "");
      if (!releaseId.empty() && storage_->getRelease(auth.context.tenantId, releaseId) == std::nullopt) return errorResponse(404, "NOT_FOUND", "Release not found.", traceId);
      const auto commandId = body.value("command_id", "");
      std::string commandType;
      if (!commandId.empty()) {
        const auto command = storage_->query("SELECT release_id,device_id,desired_generation,command_type FROM rollout_commands WHERE tenant_id=? AND id=?", {auth.context.tenantId, commandId});
        if (command.empty() || command.front().at("device_id") != deviceId) return errorResponse(404, "NOT_FOUND", "The report command is not assigned to this device.", traceId);
        if (!releaseId.empty() && command.front().at("release_id") != releaseId) return errorResponse(409, "COMMAND_RELEASE_MISMATCH", "The report command belongs to a different release.", traceId);
        releaseId = command.front().value("release_id", "");
        commandType = command.front().value("command_type", "");
      }
      const auto health = body.value("health", Json::object());
      if (!health.is_object() || health.size() > 50) return errorResponse(422, "INVALID_HEALTH", "Health metrics must be a string-keyed object with at most 50 values.", traceId);
      for (const auto& [metric, value] : health.items()) if (!value.is_number() || metric.empty() || metric.size() > 64) return errorResponse(422, "INVALID_HEALTH", "Health metric names and values must be valid.", traceId);
      const auto payload = shared::CanonicalJson::serialize(body);
      const auto payloadDigest = shared::DigestService::sha256Hex(payload);
      const auto existing = storage_->query("SELECT report_id,payload_digest,report_sequence FROM device_reports WHERE tenant_id=? AND device_id=? AND report_id=?", {auth.context.tenantId, deviceId, reportId});
      if (!existing.empty()) {
        if (existing.front().at("payload_digest") == payloadDigest) return jsonResponse(202, {{"accepted", true}, {"idempotent", true}, {"report_id", reportId}, {"report_sequence", sequence}});
        return errorResponse(409, "REPORT_ID_REUSED", "The report identifier was already used for different bytes.", traceId);
      }
      const auto highestCommand = storage_->query("SELECT COALESCE(MAX(desired_generation),0) AS generation FROM rollout_commands WHERE tenant_id=? AND device_id=?", {auth.context.tenantId, deviceId});
      const auto commandGeneration = highestCommand.empty() ? 0LL : highestCommand.front().value("generation", 0LL);
      bool inserted = false;
      bool evidenceWritten = false;
      bool projectionUpdated = false;
      const bool committed = storage_->transaction([&] {
        inserted = storage_->execute("INSERT INTO device_reports(id,tenant_id,device_id,release_id,command_id,report_id,report_sequence,report_type,observed_generation,observed_artifact_digest,health_json,result_code,payload_digest,device_recorded_at,server_received_at) VALUES(?,?,?,NULLIF(?,''),NULLIF(?,''),?,?,?,?,?,?,?,?,?,datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, deviceId, releaseId, body.value("command_id", ""), reportId, std::to_string(sequence), reportType, std::to_string(observedGeneration), body.value("observed_artifact_digest", ""), shared::CanonicalJson::serialize(body.value("health", Json::object())), body.value("result_code", ""), payloadDigest, body.value("device_recorded_at", "")});
        if (!inserted) return false;
        const auto reportAssignment = storage_->query("SELECT release_id,stage_id FROM release_assignments WHERE tenant_id=? AND device_id=? AND release_id=? AND state NOT IN ('superseded','rolled_back') ORDER BY desired_generation DESC LIMIT 1", {auth.context.tenantId, deviceId, releaseId});
        const auto device = storage_->getDevice(auth.context.tenantId, deviceId);
        const auto currentSequence = device ? device->value("last_report_sequence", 0LL) : 0LL;
        const bool advancesProjection = device && sequence > currentSequence;
        for (const auto& [metric, value] : health.items()) {
          if (advancesProjection && !reportAssignment.empty() && !storage_->execute("INSERT OR IGNORE INTO health_samples(id,tenant_id,release_id,stage_id,device_id,source,source_event_id,metric_name,metric_value,unit,observed_at,received_at,freshness_state,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,datetime('now'),datetime('now'),'fresh',datetime('now'))", {shared::Uuid::generate().str(), auth.context.tenantId, reportAssignment.front().at("release_id").get<std::string>(), reportAssignment.front().at("stage_id").get<std::string>(), deviceId, "device", reportId, metric, std::to_string(value.get<double>()), "ratio"})) return false;
        }
        if (device && sequence > currentSequence && observedGeneration <= commandGeneration) {
          const auto observedDigest = body.value("observed_artifact_digest", "");
          projectionUpdated = storage_->execute("UPDATE devices SET observed_generation=CASE WHEN observed_generation IS NULL OR observed_generation<? THEN ? ELSE observed_generation END,observed_artifact_digest=CASE WHEN observed_generation IS NULL OR observed_generation<=? THEN ? ELSE observed_artifact_digest END,last_report_sequence=?,last_seen_at=datetime('now'),updated_at=datetime('now') WHERE tenant_id=? AND id=? AND last_report_sequence<?", {std::to_string(observedGeneration), std::to_string(observedGeneration), std::to_string(observedGeneration), observedDigest, std::to_string(sequence), auth.context.tenantId, deviceId, std::to_string(sequence)});
          const auto assignment = storage_->query("SELECT a.id,a.desired_generation,ar.sha256_digest FROM release_assignments a JOIN artifacts ar ON ar.tenant_id=a.tenant_id AND ar.id=a.desired_artifact_id WHERE a.tenant_id=? AND a.device_id=? AND a.state IN ('pending','commanded','acknowledged') ORDER BY a.desired_generation DESC LIMIT 1", {auth.context.tenantId, deviceId});
          if (!assignment.empty()) {
            const auto desired = assignment.front();
            const bool converged = observedGeneration == desired.value("desired_generation", -1LL) && body.value("observed_artifact_digest", "") == desired.value("sha256_digest", "");
            const bool failed = body.value("result_code", "") == "failed" || body.value("result_code", "") == "install_failed";
            const auto state = failed ? "failed" : commandType == "cancel" && (body.value("result_code", "") == "cancelled" || body.value("result_code", "") == "cancel_succeeded") ? "superseded" : reportType == "rollback_result" && converged ? "rolled_back" : converged ? "converged" : "acknowledged";
            if (!storage_->execute("UPDATE release_assignments SET state=?,latest_report_sequence=?,acknowledged_at=CASE WHEN ?='acknowledged' OR ?='converged' THEN COALESCE(acknowledged_at,datetime('now')) ELSE acknowledged_at END,converged_at=CASE WHEN ?='converged' THEN datetime('now') ELSE converged_at END,failure_code=CASE WHEN ?='failed' THEN COALESCE(NULLIF(?,''),CASE WHEN ?='rollback_result' THEN 'ROLLBACK_FAILED' ELSE 'INSTALL_FAILED' END) ELSE failure_code END,updated_at=datetime('now') WHERE tenant_id=? AND id=? AND latest_report_sequence<?", {state, std::to_string(sequence), state, state, state, state, body.value("result_code", ""), reportType, auth.context.tenantId, desired.at("id").get<std::string>(), std::to_string(sequence)})) return false;
          }
        }
        if (!settleReleaseAfterDeviceReport(*storage_, auth.context.tenantId, releaseId, deviceId)) return false;
        evidenceWritten = storage_->appendEvidence(auth.context.tenantId, "device.report.accepted", "device", deviceId, {{"report_id", reportId}, {"report_sequence", sequence}, {"projection_updated", projectionUpdated}}, "device", deviceId).has_value();
        return evidenceWritten;
      });
      if (!committed) return errorResponse(inserted ? 500 : 409, inserted ? "EVIDENCE_COMMIT_FAILED" : "REPORT_SEQUENCE_CONFLICT", inserted ? "The report and its audit evidence could not be committed atomically." : "The report sequence or identifier was already recorded.", traceId);
      return jsonResponse(202, {{"accepted", true}, {"report_id", reportId}, {"report_sequence", sequence}, {"projection_updated", projectionUpdated}});
    } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Request body must be valid JSON.", traceId); }
  }
  if (parts.size() == 5 && parts[3] == "credential-rotation" && parts[4] == "ack" && request.method == "POST") {
    try {
      const auto body = parseBody(request);
       const auto reportId = body.value("report_id", std::string{});
       const auto sequence = body.value("sequence", body.value("report_sequence", 0LL));
       if (reportId.empty() || sequence < 1 || header(request, "x-device-sequence") != std::to_string(sequence)) return errorResponse(422, "INVALID_ROTATION_ACK", "Rotation acknowledgement requires a report_id and matching sequence.", traceId);
       const auto version = body.value("key_version", std::stoi(header(request, "x-device-key-version")));
       if (std::to_string(version) != header(request, "x-device-key-version")) return errorResponse(409, "ROTATION_VERSION_MISMATCH", "The signed key version must match the acknowledgement.", traceId);
       const auto successor = storage_->query("SELECT id,supersedes_credential_id,secret_ciphertext FROM device_credentials WHERE tenant_id=? AND device_id=? AND key_version=? AND supersedes_credential_id IS NOT NULL AND acknowledged_at IS NULL AND revoked_at IS NULL", {auth.context.tenantId, auth.context.actorId, std::to_string(version)});
       const auto successorSecret = successor.empty() ? std::optional<std::string>{} : shared::DigestService::decryptSecret(config_.credentialEncryptionKey, successor.front().value("secret_ciphertext", ""));
      const bool acknowledged = !successor.empty() && successorSecret.has_value() && storage_->transaction([&] {
        if (!storage_->execute("UPDATE device_credentials SET acknowledged_at=datetime('now') WHERE tenant_id=? AND id=? AND acknowledged_at IS NULL", {auth.context.tenantId, successor.front().at("id").get<std::string>()}) ||
            !storage_->execute("UPDATE device_credentials SET revoked_at=datetime('now') WHERE tenant_id=? AND id=? AND revoked_at IS NULL", {auth.context.tenantId, successor.front().at("supersedes_credential_id").get<std::string>()}) ||
            !storage_->execute("UPDATE devices SET device_secret_hash=?,device_key_version=?,updated_at=datetime('now') WHERE tenant_id=? AND id=?", {shared::DigestService::sha256Hex(*successorSecret), std::to_string(version), auth.context.tenantId, auth.context.actorId})) return false;
        return storage_->appendEvidence(auth.context.tenantId, "device.credential_rotation_acknowledged", "device", auth.context.actorId, {{"key_version", version}, {"report_id", reportId}, {"sequence", sequence}}, "device", auth.context.actorId).has_value();
      });
      if (!acknowledged) return errorResponse(409, "ROTATION_ACK_INVALID", "The successor credential is unknown or already acknowledged.", traceId);
      return jsonResponse(202, {{"accepted", true}, {"device_id", auth.context.actorId}, {"key_version", version}});
    } catch (const std::exception&) { return errorResponse(400, "INVALID_JSON", "Rotation acknowledgement must be valid JSON.", traceId); }
  }
  return errorResponse(404, "NOT_FOUND", "The device protocol route does not exist.", traceId);
}

HttpResponse ControlPlane::htmlRoute(const HttpRequest& request, const AuthResult& auth) {
  if (request.method != "GET") return errorResponse(405, "METHOD_NOT_ALLOWED", "The page is read-only.", auth.traceId);
  if (pathOnly(request.target) == "/login") {
    const auto body = std::string("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Sign in · Edge Fleet</title><link rel=\"stylesheet\" href=\"/static/app.css\"><script src=\"/static/htmx.min.js\" defer></script><script src=\"/static/app.js\" defer></script></head><body><main><p class=\"eyebrow\">EDGE FLEET / SAFETY CONTROL</p><h1>Sign in</h1><section class=\"card\"><form method=\"post\" action=\"/auth/session\"><label for=\"api-key\">API key</label><input id=\"api-key\" name=\"api_key\" type=\"password\" autocomplete=\"off\" required><button type=\"submit\">Create browser session</button></form><p class=\"muted\">The key is exchanged for an HttpOnly Secure session cookie; it is never rendered back.</p></section></main></body></html>");
    return {200, "text/html; charset=utf-8", body, {{"Cache-Control", "no-store"}}};
  }
  if (!auth.context.authenticated) return {302, "text/html; charset=utf-8", "", {{"Location", "/login"}}};
  const auto path = pathOnly(request.target);
  if ((path == "/app/settings/integrations" || path == "/app/settings/tenant") && auth.context.role != shared::Role::admin) return errorResponse(403, "FORBIDDEN", "Administrator permission is required for this settings page.", auth.traceId);
  if (path == "/app/releases/new" && !web::requireRole(auth.context, "release_drafts", "write")) return errorResponse(403, "FORBIDDEN", "Release write permission is required for this page.", auth.traceId);
  if (path == "/app/fragments/summary") {
    const auto fleetRows = storage_->query("SELECT COUNT(*) AS count FROM fleets WHERE tenant_id=? AND status='active'", {auth.context.tenantId});
    const auto releaseRows = storage_->query("SELECT COUNT(*) AS count FROM releases WHERE tenant_id=? AND status IN ('scheduled','running','paused','aborting','rolling_back')", {auth.context.tenantId});
    const auto noticeRows = storage_->query("SELECT COUNT(*) AS count FROM operator_notices WHERE tenant_id=? AND acknowledged_at IS NULL", {auth.context.tenantId});
    const auto deviceRows = storage_->query("SELECT COUNT(*) AS count FROM devices WHERE tenant_id=? AND lifecycle_status NOT IN ('decommissioned','quarantined')", {auth.context.tenantId});
    const auto count = [](const std::vector<Json>& rows) { return rows.empty() ? 0LL : rows.front().value("count", 0LL); };
    std::ostringstream fragment;
    fragment << "<ul class=\"summary-list\" id=\"live-summary\"><li><span>Fleets</span><strong>" << count(fleetRows) << "</strong></li><li><span>Managed devices</span><strong>" << count(deviceRows) << "</strong></li><li><span>Active releases</span><strong>" << count(releaseRows) << "</strong></li><li><span>Open safety notices</span><strong>" << count(noticeRows) << "</strong></li></ul>";
    return {200, "text/html; charset=utf-8", fragment.str(), {{"Cache-Control", "no-store"}}};
  }
  if (path == "/app/fragments/releases") {
    if (!web::requireRole(auth.context, "release_drafts", "read")) return errorResponse(403, "FORBIDDEN", "Release read permission is required.", auth.traceId);
    const auto rows = storage_->query("SELECT id,name,status,current_stage_ordinal,version,updated_at FROM releases WHERE tenant_id=? AND status IN ('scheduled','running','paused','aborting','rolling_back') ORDER BY updated_at DESC LIMIT 25", {auth.context.tenantId});
    std::ostringstream fragment;
    if (rows.empty()) fragment << "<p class=\"muted\">No active releases.</p>";
    else { fragment << "<div class=\"table-scroll\"><table><caption class=\"sr-only\">Active releases</caption><thead><tr><th>Release</th><th>Status</th><th>Stage</th><th>Version</th></tr></thead><tbody>"; for (const auto& row : rows) fragment << "<tr><td><a href=\"/app/releases/" << htmlEscape(row.at("id").get<std::string>()) << "\">" << htmlEscape(row.at("name").get<std::string>()) << "</a></td><td>" << htmlEscape(row.at("status").get<std::string>()) << "</td><td>" << row.at("current_stage_ordinal") << "</td><td>" << row.at("version") << "</td></tr>"; fragment << "</tbody></table></div>"; }
    return {200, "text/html; charset=utf-8", fragment.str(), {{"Cache-Control", "no-store"}}};
  }
  if (path == "/app/fragments/jobs") {
    if (!web::requireRole(auth.context, "simulation", "read")) return errorResponse(403, "FORBIDDEN", "Job read permission is required.", auth.traceId);
    const auto simulations = storage_->query("SELECT id,scenario_name,status,created_at FROM simulation_runs WHERE tenant_id=? AND status IN ('queued','running') ORDER BY created_at DESC LIMIT 10", {auth.context.tenantId});
    const auto replays = storage_->query("SELECT id,source_kind,status,created_at FROM replay_runs WHERE tenant_id=? AND status IN ('queued','running') ORDER BY created_at DESC LIMIT 10", {auth.context.tenantId});
    const auto exports = storage_->query("SELECT id,status,created_at FROM evidence_exports WHERE tenant_id=? AND status IN ('queued','running') ORDER BY created_at DESC LIMIT 10", {auth.context.tenantId});
    std::ostringstream fragment; fragment << "<ul class=\"summary-list job-list\"><li><span>Simulations</span><strong>" << simulations.size() << "</strong></li><li><span>Replays</span><strong>" << replays.size() << "</strong></li><li><span>Evidence exports</span><strong>" << exports.size() << "</strong></li></ul>";
    return {200, "text/html; charset=utf-8", fragment.str(), {{"Cache-Control", "no-store"}}};
  }
  const auto title = path == "/app" ? "Fleet overview" : path == "/app/fleets" ? "Fleets" : path == "/app/releases" ? "Releases" : path == "/app/evidence" ? "Evidence" : path == "/app/approvals" ? "Approvals" : path == "/app/simulations" ? "Simulations" : path == "/app/artifacts" ? "Artifacts" : path == "/app/policies" ? "Policies" : "Edge Fleet control plane";
  const std::string displayName = auth.principal.contains("display_name") && auth.principal.at("display_name").is_string() ? auth.principal.at("display_name").get<std::string>() : "operator";
  const std::string role = auth.principal.contains("role") && auth.principal.at("role").is_string() ? auth.principal.at("role").get<std::string>() : "operator";
  const auto detailId = [](const std::string& prefix, const std::string& value) { return value.starts_with(prefix) ? value.substr(prefix.size()) : std::string{}; };
  const auto fleetId = detailId("/app/fleets/", path);
  const auto deviceId = detailId("/app/devices/", path);
  const auto releaseId = detailId("/app/releases/", path);
  const auto simulationId = detailId("/app/simulations/", path);
  const bool canFleetWrite = web::requireRole(auth.context, "fleets", "write");
  const bool canDeviceWrite = web::requireRole(auth.context, "devices", "write");
  const bool canArtifactWrite = web::requireRole(auth.context, "artifacts", "write");
  const bool canPolicyWrite = web::requireRole(auth.context, "policies", "write");
  const bool canReleaseWrite = web::requireRole(auth.context, "release_drafts", "write");
  const bool canApprove = web::requireRole(auth.context, "approvals", "approve");
  const bool canSimulationWrite = web::requireRole(auth.context, "simulation", "write");
  std::string panel = "<section class=\"card\"><h2>Observed state is the source of truth</h2><p>Rollouts advance only after fresh device reports satisfy the frozen gate. Commands and evidence remain immutable.</p><p class=\"status\">Current operator: " + htmlEscape(auth.context.actorId) + "</p><div class=\"api-state\" id=\"live-summary\" data-api-state hx-get=\"/app/fragments/summary\" hx-trigger=\"load\" hx-target=\"#live-summary\" hx-swap=\"innerHTML\" aria-live=\"polite\">Loading live API state…</div></section>";
  if (path == "/app") panel = "<section class=\"card\"><h2>Fleet health</h2><p>Review active releases, fresh device observations, and critical notices before changing exposure.</p><p class=\"status\">No decision is promoted from delivery or acknowledgement alone.</p><div class=\"api-state\" data-api-state aria-live=\"polite\">Loading current counts…</div></section>";
  else if (path == "/app/fleets") panel = std::string("<section class=\"card\"><h2>Fleet inventory</h2><p>Filter by environment, lifecycle, labels, or drift. Device registration secrets are displayed only once.</p><div class=\"resource-state\" data-resource=\"fleets\" aria-live=\"polite\">Loading fleet inventory…</div>") + (canFleetWrite ? "<form class=\"ui-form\" data-api-action=\"/api/fleets\" data-api-method=\"POST\"><h3>Create fleet</h3><label>Slug<input name=\"slug\" required pattern=\"[a-z0-9-]+\"></label><label>Name<input name=\"name\" required></label><label>Environment<select name=\"environment\"><option>development</option><option>staging</option><option>production</option></select></label><label>Label schema JSON<textarea name=\"label_schema_json\">{}</textarea></label><button type=\"submit\">Create fleet</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path.starts_with("/app/fleets/") && !fleetId.empty()) panel = std::string("<section class=\"card\"><h2>Fleet detail</h2><p>Register devices against this tenant-scoped fleet and pause exposure with an explicit reason.</p><div class=\"resource-state\" data-resource=\"fleet\" data-resource-id=\"" + htmlEscape(fleetId) + "\" aria-live=\"polite\">Loading fleet…</div><div class=\"resource-state\" data-resource=\"devices\" data-resource-query=\"fleet_id=" + htmlEscape(fleetId) + "\" aria-live=\"polite\">Loading devices…</div>") + (canDeviceWrite ? "<form class=\"ui-form\" data-api-action=\"/api/fleets/" + htmlEscape(fleetId) + "/devices\" data-api-method=\"POST\"><h3>Register device</h3><label>Stable key<input name=\"stable_key\" required></label><label>Display name<input name=\"display_name\"></label><label>Hardware model<input name=\"hardware_model\" required></label><label>Architecture<input name=\"architecture\" required></label><label>Labels JSON<textarea name=\"labels_json\">{}</textarea></label><label>Device secret (optional)<input name=\"device_secret\" type=\"password\" autocomplete=\"new-password\"></label><button type=\"submit\">Register device</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + (auth.context.role == shared::Role::admin ? "<form class=\"ui-form\" data-api-action=\"/api/fleets/" + htmlEscape(fleetId) + "/pause\" data-api-method=\"POST\"><h3>Pause fleet</h3><label>Reason<input name=\"reason\" required></label><button type=\"submit\">Pause fleet</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path == "/app/devices") panel = "<section class=\"card\"><h2>Device truth</h2><p>Compare desired generation and digest with the latest observed report. Quarantine and rotation remain permission-controlled.</p><div class=\"resource-state\" data-resource=\"devices\" aria-live=\"polite\">Loading device truth…</div></section>";
  else if (path.starts_with("/app/devices/") && !deviceId.empty()) panel = std::string("<section class=\"card\"><h2>Device detail</h2><p>Observed state, assignment facts, and reports are immutable evidence. Lifecycle actions require a reason.</p><div class=\"resource-state\" data-resource=\"device\" data-resource-id=\"" + htmlEscape(deviceId) + "\" aria-live=\"polite\">Loading device…</div>") + (canDeviceWrite ? "<form class=\"ui-form\" data-api-action=\"/api/devices/" + htmlEscape(deviceId) + "/quarantine\" data-api-method=\"POST\"><label>Reason<input name=\"reason\" required></label><button type=\"submit\">Quarantine device</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path == "/app/artifacts") panel = std::string("<section class=\"card\"><h2>Signed artifacts</h2><p>Upload immutable bytes, verify Ed25519 manifests, inspect compatibility, digest, and retirement state.</p><div class=\"resource-state\" data-resource=\"artifacts\" aria-live=\"polite\">Loading artifacts…</div>") + (canArtifactWrite ? "<form class=\"ui-form artifact-upload\" data-artifact-upload><h3>Stream an artifact</h3><label>Name<input name=\"name\" required></label><label>Version<input name=\"version\" required></label><label>Hardware model<input name=\"hardware_model\" required></label><label>Architecture<input name=\"architecture\" required></label><label>File name<input name=\"file_name\" required></label><label>Signing key ID<input name=\"signing_key_id\" required></label><label>Manifest JSON<textarea name=\"manifest_json\" required>{}</textarea></label><label>Detached signature<input name=\"signature\" required></label><label>Binary bytes<input name=\"file\" type=\"file\" required></label><button type=\"submit\">Upload and verify</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path == "/app/policies") panel = std::string("<section class=\"card\"><h2>Fixed-wave policies</h2><p>Draft, validate, activate, and archive bounded stage plans with explicit health gates.</p><div class=\"resource-state\" data-resource=\"policies\" aria-live=\"polite\">Loading policies…</div>") + (canPolicyWrite ? "<form class=\"ui-form\" data-api-action=\"/api/policies\" data-api-method=\"POST\"><h3>Draft policy</h3><label>Name<input name=\"name\" required></label><label>Stage plan JSON<textarea name=\"stage_plan_json\" required>[1,5,20,50,100]</textarea></label><label>Selector JSON<textarea name=\"selector_json\">{}</textarea></label><label>Health gates JSON<textarea name=\"health_gates_json\" required>{}</textarea></label><label>Rollback requirement<select name=\"rollback_requirement\"><option>required</option><option>allow_first_install</option></select></label><button type=\"submit\">Save policy draft</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path == "/app/releases/new") panel = std::string("<section class=\"card\"><h2>New frozen release</h2><p>Select a fleet, signed target, rollback artifact, and active policy. Validation freezes membership before approval.</p>") + (canReleaseWrite ? "<form class=\"ui-form release-wizard\" data-api-action=\"/api/releases\" data-api-method=\"POST\" data-release-wizard><label>Release name<input name=\"name\" required></label><label>Fleet<select name=\"fleet_id\" data-options-resource=\"fleets\" required></select></label><label>Target artifact<select name=\"artifact_id\" data-options-resource=\"artifacts\" required></select></label><label>Rollback artifact<select name=\"rollback_artifact_id\" data-options-resource=\"artifacts\"><option value=\"\">None</option></select></label><label>Policy<select name=\"policy_id\" data-options-resource=\"policies\" required></select></label><button type=\"submit\">Create draft</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path == "/app/releases") panel = "<section class=\"card\"><h2>Release monitor</h2><p>Stages, fresh gate evaluations, assignments, stranded devices, approvals, and evidence are shown together.</p><div class=\"resource-state\" data-resource=\"releases\" aria-live=\"polite\">Loading releases…</div><a href=\"/app/releases/new\">Create a release draft</a></section>";
  else if (path.starts_with("/app/releases/") && !releaseId.empty()) panel = std::string("<section class=\"card\"><h2>Release monitor</h2><p>Every control is version-bound and requires a reason. Approval-bound actions remain local when integrations are unavailable.</p><div class=\"resource-state\" data-resource=\"release\" data-resource-id=\"" + htmlEscape(releaseId) + "\" aria-live=\"polite\">Loading release…</div><div class=\"resource-state\" data-resource=\"gates\" data-resource-id=\"" + htmlEscape(releaseId) + "\" aria-live=\"polite\">Loading gates…</div>") + (canReleaseWrite ? "<form class=\"ui-form release-control\" data-release-control data-release-id=\"" + htmlEscape(releaseId) + "\"><label>Action<select name=\"action\"><option>validate</option><option>submit</option><option>schedule</option><option>start</option><option>pause</option><option>resume</option><option>cancel</option><option>abort</option><option>rollback</option></select></label><label>Expected release version<input name=\"expected_version\" type=\"number\" min=\"1\" required></label><label>Scheduled for (ISO-8601, for schedule)<input name=\"scheduled_for\" placeholder=\"2026-09-01T12:00:00Z\"></label><label>Reason<input name=\"reason\" required></label><button type=\"submit\">Submit safety action</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path == "/app/approvals") panel = std::string("<section class=\"card\"><h2>Pending safety decisions</h2><p>Four-eyes approval requires a different actor and remains bound to the captured release version and evidence.</p><div class=\"resource-state\" data-resource=\"approvals\" data-can-approve=\"") + (canApprove ? "true" : "false") + "\" aria-live=\"polite\">Loading approvals…</div></section>";
  else if (path == "/app/simulations") panel = std::string("<section class=\"card\"><h2>Deterministic simulation</h2><p>Compare control-plane, all-at-once, and fixed-ten-percent strategies over a frozen seed and trace.</p><div class=\"resource-state\" data-resource=\"simulations\" aria-live=\"polite\">Loading simulations…</div>") + (canSimulationWrite ? "<form class=\"ui-form\" data-api-action=\"/api/simulations\" data-api-method=\"POST\"><h3>Queue scenario</h3><label>Scenario name<input name=\"scenario_name\" value=\"healthy-10k\" required></label><label>Scenario version<input name=\"scenario_version\" value=\"1\" required></label><label>Seed<input name=\"seed\" type=\"number\" value=\"42\" required></label><label>Scenario JSON<textarea name=\"input_json\">{\"schema_version\":\"v1\",\"device_count\":100,\"duration_seconds\":3600}</textarea></label><button type=\"submit\">Run simulation</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  else if (path.starts_with("/app/simulations/") && !simulationId.empty()) panel = std::string("<section class=\"card\"><h2>Simulation result</h2><p>Frozen inputs, strategy metrics, trace digest, and replay status are read from the API.</p><div class=\"resource-state\" data-resource=\"simulation\" data-resource-id=\"") + htmlEscape(simulationId) + "\" aria-live=\"polite\">Loading simulation…</div></section>";
  else if (path == "/app/replays") panel = "<section class=\"card\"><h2>Replay evidence</h2><p>Reproduce one frozen simulation or release evidence range and report the first deterministic divergence.</p><div class=\"resource-state\" data-resource=\"replays\" aria-live=\"polite\">Loading replays…</div><a href=\"/api/replays\">Open replay data</a></section>";
  else if (path == "/app/evidence") panel = "<section class=\"card\"><h2>Evidence chain</h2><p>Verify the tenant chain, export bounded NDJSON, and retain immutable event digests.</p><div class=\"resource-state\" data-resource=\"evidence\" aria-live=\"polite\">Loading evidence…</div><a href=\"/api/evidence\">Open evidence data</a></section>";
  else if (path == "/app/settings/integrations") panel = "<section class=\"card\"><h2>Optional integrations</h2><p>Adapters default off, accept secret references only, and cannot become release authority. Local safety actions remain available during outages.</p><div class=\"resource-state\" data-resource=\"integrations\" aria-live=\"polite\">Loading integration health…</div><form class=\"ui-form\" data-integration-config><h3>Configure an adapter</h3><label>Adapter type<select name=\"adapter_type\" required><option value=\"iot_rest_v1\">IoT REST v1</option><option value=\"notification_hub_v1\">Notification Hub v1</option><option value=\"workflow_manual_v1\">Workflow Manual v1</option></select></label><label>Private endpoint base URL<input name=\"endpoint_base_url\" type=\"url\" placeholder=\"https://adapter.internal\"></label><label>Secret reference name<input name=\"secret_ref\" pattern=\"[A-Z][A-Z0-9_]{2,127}\" placeholder=\"EDGEFLEET_NOTIFICATION_API_KEY\"></label><label>Workflow ID (workflow adapter only)<input name=\"workflow_id\"></label><label>Mode<select name=\"fixture_mode\"><option value=\"true\">Fixture / local contract</option><option value=\"false\">Live private HTTPS</option></select></label><label class=\"checkbox-label\"><input name=\"required_for_promotion\" type=\"checkbox\"> Required for promotion</label><button type=\"submit\">Save disabled configuration</button><p class=\"form-status\" aria-live=\"polite\"></p></form></section>";
  else if (path == "/app/settings/tenant") panel = "<section class=\"card\"><h2>Tenant identity</h2><p>Manage the tenant identity used in evidence exports and operator sessions. Credentials are listed separately and secrets are never returned.</p><div class=\"resource-state\" data-resource=\"tenant\" data-resource-id=\"me\" aria-live=\"polite\">Loading tenant identity…</div><form class=\"ui-form\" data-api-action=\"/api/tenants/me\" data-api-method=\"PATCH\"><h3>Update identity</h3><label>Display name<input name=\"display_name\" required></label><label>Legal name<input name=\"legal_name\" required></label><button type=\"submit\">Save tenant identity</button><p class=\"form-status\" aria-live=\"polite\"></p></form></section>";
  if (path == "/app") panel += "<section class=\"card\"><h2>Live release monitor</h2><div id=\"live-releases\" data-api-state hx-get=\"/app/fragments/releases\" hx-trigger=\"load, every 5s\" hx-target=\"#live-releases\" aria-live=\"polite\">Loading active releases…</div></section><section class=\"card\"><h2>Background jobs</h2><div id=\"live-jobs\" data-api-state hx-get=\"/app/fragments/jobs\" hx-trigger=\"load, every 5s\" hx-target=\"#live-jobs\" aria-live=\"polite\">Loading background jobs…</div></section>";
  if (path == "/app/fleets") panel += "<section class=\"card filter-card\"><h2>Filter fleet inventory</h2><form data-resource-filter=\"fleets\"><label>Environment<select name=\"environment\"><option value=\"\">All environments</option><option>development</option><option>staging</option><option>production</option></select></label><label>Status<select name=\"status\"><option value=\"\">All statuses</option><option>active</option><option>paused</option><option>retired</option></select></label><button class=\"secondary\" type=\"submit\">Apply filters</button></form></section>";
  if (path.starts_with("/app/fleets/") && !fleetId.empty()) {
    panel += "<section class=\"card\"><h2>Fleet controls</h2><form class=\"ui-form\" data-api-action=\"/api/fleets/" + htmlEscape(fleetId) + "\" data-api-method=\"PATCH\"><h3>Edit fleet metadata</h3><label>Name<input name=\"name\" required></label><label>Description<input name=\"description\"></label><label>Environment<select name=\"environment\"><option>development</option><option>staging</option><option>production</option></select></label><label>Expected version<input name=\"expected_version\" type=\"number\" min=\"1\" required></label><button type=\"submit\">Save fleet</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" + (auth.context.role == shared::Role::admin ? "<form class=\"ui-form\" data-api-action=\"/api/fleets/" + htmlEscape(fleetId) + "/retire\" data-api-method=\"POST\"><h3>Retire fleet</h3><label>Reason<input name=\"reason\" required></label><button class=\"danger\" type=\"submit\">Retire fleet</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  }
  if (path.starts_with("/app/devices/") && !deviceId.empty()) {
    panel += "<section class=\"card\"><h2>Device controls</h2>" + (canDeviceWrite ? "<form class=\"ui-form\" data-api-action=\"/api/devices/" + htmlEscape(deviceId) + "\" data-api-method=\"PATCH\"><h3>Update assignment labels</h3><label>Display name<input name=\"display_name\"></label><label>Labels JSON<textarea name=\"labels_json\">{}</textarea></label><button type=\"submit\">Save device metadata</button><p class=\"form-status\" aria-live=\"polite\"></p></form><form class=\"ui-form\" data-api-action=\"/api/devices/" + htmlEscape(deviceId) + "/quarantine\" data-api-method=\"POST\"><h3>Quarantine</h3><label>Reason<input name=\"reason\" required></label><button class=\"danger\" type=\"submit\">Quarantine device</button><p class=\"form-status\" aria-live=\"polite\"></p></form><form class=\"ui-form\" data-api-action=\"/api/devices/" + htmlEscape(deviceId) + "/reactivate\" data-api-method=\"POST\"><h3>Reactivate</h3><label>Reason<input name=\"reason\" required></label><button type=\"submit\">Reactivate device</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + (auth.context.role == shared::Role::admin ? "<form class=\"ui-form\" data-api-action=\"/api/devices/" + htmlEscape(deviceId) + "/credentials/rotate\" data-api-method=\"POST\"><h3>Rotate device credential</h3><p class=\"muted\">The successor secret is shown once after a successful rotation.</p><button type=\"submit\">Rotate credential</button><p class=\"form-status\" aria-live=\"polite\"></p></form><form class=\"ui-form\" data-api-action=\"/api/devices/" + htmlEscape(deviceId) + "/decommission\" data-api-method=\"POST\"><h3>Decommission</h3><label>Reason<input name=\"reason\" required></label><button class=\"danger\" type=\"submit\">Decommission device</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  }
  if (path == "/app/artifacts" && auth.context.role == shared::Role::admin) panel += "<section class=\"card\"><h2>Signing keys</h2><p>Compromise and retirement controls are administrator-only and remain audited.</p><div class=\"resource-state\" data-resource=\"signing-keys\" aria-live=\"polite\">Signing-key status is available from the API.</div><form class=\"ui-form\" data-api-action=\"/api/artifact-signing-keys\" data-api-method=\"POST\"><button type=\"submit\">Generate signing key</button><p class=\"form-status\" aria-live=\"polite\"></p></form></section>";
  if (path.starts_with("/app/releases/") && path != "/app/releases/new" && !releaseId.empty()) panel += "<section class=\"card\"><h2>Release evidence</h2><div class=\"resource-state\" data-resource=\"assignments\" data-resource-id=\"" + htmlEscape(releaseId) + "\" aria-live=\"polite\">Loading assignments…</div><div class=\"resource-state\" data-resource=\"membership\" data-resource-id=\"" + htmlEscape(releaseId) + "\" aria-live=\"polite\">Loading frozen membership…</div></section>";
  if (path.starts_with("/app/simulations/") && !simulationId.empty() && canSimulationWrite) panel += "<section class=\"card\"><h2>Simulation controls</h2><form class=\"ui-form\" data-api-action=\"/api/simulations/" + htmlEscape(simulationId) + "/cancel\" data-api-method=\"POST\"><button class=\"danger\" type=\"submit\">Cancel queued simulation</button><p class=\"form-status\" aria-live=\"polite\"></p></form><form class=\"ui-form\" data-api-action=\"/api/simulations/" + htmlEscape(simulationId) + "/replay\" data-api-method=\"POST\"><button type=\"submit\">Queue deterministic replay</button><p class=\"form-status\" aria-live=\"polite\"></p></form></section>";
  if (path == "/app/replays") panel += "<section class=\"card\"><h2>Replay contract</h2><p>Every replay exposes expected and actual digests plus the first divergent event when available. Use the API detail link for the complete trace.</p></section>";
  if (path == "/app/evidence") panel += std::string("<section class=\"card\"><h2>Chain actions</h2><form class=\"ui-form inline-form\" data-api-action=\"/api/evidence/verify\" data-api-method=\"POST\"><button type=\"submit\">Verify evidence chain</button><p class=\"form-status\" aria-live=\"polite\"></p></form>") + (web::requireRole(auth.context, "evidence_exports", "write") ? "<form class=\"ui-form inline-form\" data-api-action=\"/api/evidence/exports\" data-api-method=\"POST\"><button type=\"submit\">Export NDJSON evidence</button><p class=\"form-status\" aria-live=\"polite\"></p></form>" : "") + "</section>";
  if (path == "/app/settings/tenant" && auth.context.role == shared::Role::admin) panel += "<section class=\"card\"><h2>Operator credentials</h2><p>Only prefixes and lifecycle metadata are returned. Newly created secrets are shown once.</p><div class=\"resource-state\" data-resource=\"credentials\" aria-live=\"polite\">Loading credentials…</div><form class=\"ui-form\" data-api-action=\"/api/credentials\" data-api-method=\"POST\"><h3>Create operator credential</h3><label>Label<input name=\"label\" required></label><label>Role<select name=\"role\"><option>viewer</option><option>release_manager</option><option>approver</option><option>admin</option></select></label><button type=\"submit\">Create one-time API key</button><p class=\"form-status\" aria-live=\"polite\"></p></form></section>";
  std::ostringstream navigation;
  navigation << "<a href=\"/app\">Overview</a>";
  if (web::requireRole(auth.context, "fleets", "read")) navigation << "<a href=\"/app/fleets\">Fleets</a>";
  if (web::requireRole(auth.context, "devices", "read")) navigation << "<a href=\"/app/devices\">Devices</a>";
  if (web::requireRole(auth.context, "artifacts", "read")) navigation << "<a href=\"/app/artifacts\">Artifacts</a>";
  if (web::requireRole(auth.context, "policies", "read")) navigation << "<a href=\"/app/policies\">Policies</a>";
  if (web::requireRole(auth.context, "release_drafts", "read")) navigation << "<a href=\"/app/releases\">Releases</a>";
  if (web::requireRole(auth.context, "approvals", "read")) navigation << "<a href=\"/app/approvals\">Approvals</a>";
  if (web::requireRole(auth.context, "simulation", "read")) navigation << "<a href=\"/app/simulations\">Simulations</a><a href=\"/app/replays\">Replays</a>";
  if (web::requireRole(auth.context, "evidence", "read")) navigation << "<a href=\"/app/evidence\">Evidence</a>";
  if (auth.context.role == shared::Role::admin) navigation << "<a href=\"/app/settings/integrations\">Integrations</a><a href=\"/app/settings/tenant\">Tenant</a>";
  std::ostringstream page;
  page << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" << htmlEscape(title) << " - Edge Fleet</title><link rel=\"stylesheet\" href=\"/static/app.css\"><script src=\"/static/htmx.min.js\" defer></script><script src=\"/static/app.js\" defer></script></head><body><a class=\"skip-link\" href=\"#main-content\">Skip to main content</a><main id=\"main-content\" data-role=\"" << htmlEscape(role) << "\"><p class=\"eyebrow\">EDGE FLEET / SAFETY CONTROL</p><h1>" << htmlEscape(title) << "</h1><p>Tenant: " << htmlEscape(displayName) << " - Role: " << htmlEscape(role) << "</p><nav aria-label=\"Primary\">" << navigation.str() << "</nav><button class=\"secondary sign-out\" id=\"sign-out\" type=\"button\">Sign out</button>" << panel << "</main></body></html>";
  const auto body = page.str();
  return {200, "text/html; charset=utf-8", body, {}};
}

}  // namespace edgefleet::application
