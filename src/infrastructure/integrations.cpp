#include "infrastructure/integrations.hpp"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

namespace edgefleet::infrastructure {
namespace {

std::string isoNow(std::chrono::system_clock::time_point value) {
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
  const auto raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &raw);
#else
  gmtime_r(&raw, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << "Z";
  return output.str();
}

bool parseTimestamp(const std::string& value, std::chrono::system_clock::time_point& result) {
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':') return false;
  std::tm utc{};
  std::istringstream input(value.substr(0, 19));
  input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) return false;
#ifdef _WIN32
  const auto timestamp = _mkgmtime(&utc);
#else
  const auto timestamp = timegm(&utc);
#endif
  if (timestamp < 0) return false;
  result = std::chrono::system_clock::from_time_t(timestamp);
  return true;
}

std::string derivedEventId(const shared::Json& reading, const std::string& observedAt, const std::string& unit) {
  return shared::DigestService::sha256Hex(shared::CanonicalJson::serialize(
      {{"device_id", reading.at("device_id")}, {"metric_name", reading.at("metric_name")},
       {"observed_at", observedAt}, {"value", reading.at("value")}, {"unit", unit}}));
}

std::string appendPath(const std::string& endpoint, const std::string& path) {
  if (endpoint.empty()) return {};
  return endpoint.back() == '/' ? endpoint.substr(0, endpoint.size() - 1) + path : endpoint + path;
}

bool isSuccess(int status) { return status >= 200 && status < 300; }

DeliveryResult classifyResponse(const shared::Result<shared::HttpClientResponse>& response) {
  if (!response.ok()) return {DeliveryDisposition::retryable_failure, 0, response.error->code, {}, {}};
  const auto status = response.value->statusCode;
  if (isSuccess(status)) return {DeliveryDisposition::published, status, {}, {}, {}};
  if (status == 401 || status == 403) return {DeliveryDisposition::permanent_failure, status, "ADAPTER_UNAUTHORIZED", {}, {}};
  if (status == 429 || status >= 500) return {DeliveryDisposition::retryable_failure, status, "RETRYABLE_ADAPTER_FAILURE", {}, {}};
  if (status >= 300 && status < 400) return {DeliveryDisposition::permanent_failure, status, "SSRF_REDIRECT_REJECTED", {}, {}};
  return {DeliveryDisposition::permanent_failure, status, "ADAPTER_REJECTED", {}, {}};
}

}  // namespace

std::optional<AdapterType> adapterTypeFromString(const std::string& value) {
  if (value == "iot_rest_v1") return AdapterType::iot_rest_v1;
  if (value == "notification_hub_v1") return AdapterType::notification_hub_v1;
  if (value == "workflow_manual_v1") return AdapterType::workflow_manual_v1;
  return std::nullopt;
}

std::string_view adapterTypeName(AdapterType value) {
  switch (value) {
    case AdapterType::iot_rest_v1: return "iot_rest_v1";
    case AdapterType::notification_hub_v1: return "notification_hub_v1";
    case AdapterType::workflow_manual_v1: return "workflow_manual_v1";
  }
  return "unknown";
}

bool isSupportedAdapterType(const std::string& value) { return adapterTypeFromString(value).has_value(); }

DeliveryResult Adapter::fixtureDelivery(const shared::Json& settings, const std::string& outboxId, int attemptCount) const {
  return AdapterContract::fixtureDelivery(std::string(name()), settings, outboxId, attemptCount);
}

DeliveryResult Adapter::liveDelivery(const std::string& endpoint, const std::string& apiKey, const shared::Json& payload,
                                     const std::string& idempotencyKey, const shared::Json& settings, shared::HttpClientPool& clientPool) const {
  return AdapterContract::liveDelivery(std::string(name()), endpoint, apiKey, payload, idempotencyKey, settings, clientPool);
}

DeliveryResult Adapter::testConnection(const std::string& endpoint, const std::string& apiKey, const shared::Json& settings,
                                       shared::HttpClientPool& clientPool) const {
  return AdapterContract::testConnection(std::string(name()), endpoint, apiKey, settings, clientPool);
}

std::unique_ptr<Adapter> createAdapter(AdapterType type) { return std::make_unique<Adapter>(type); }

std::unique_ptr<Adapter> createAdapter(const std::string& value) {
  const auto type = adapterTypeFromString(value);
  return type.has_value() ? createAdapter(*type) : nullptr;
}

shared::Result<std::vector<IotReading>> IotFixtureParser::parse(const shared::Json& settings,
                                                                 std::chrono::system_clock::time_point now) {
  if (!settings.is_object()) return shared::Result<std::vector<IotReading>>::failure({"IOT_FIXTURE_INVALID", "IoT fixture settings must be an object.", 422});
  const auto readings = settings.value("readings", shared::Json::array());
  if (!readings.is_array()) return shared::Result<std::vector<IotReading>>::failure({"IOT_FIXTURE_INVALID", "IoT fixture readings must be an array.", 422});
  const auto freshnessSeconds = settings.value("freshness_seconds", 120);
  if (freshnessSeconds < 1 || freshnessSeconds > 24 * 60 * 60) return shared::Result<std::vector<IotReading>>::failure({"IOT_FIXTURE_INVALID", "IoT freshness_seconds is out of bounds.", 422});

  std::vector<IotReading> result;
  result.reserve(readings.size());
  for (const auto& reading : readings) {
    if (!reading.is_object() || !reading.contains("device_id") || !reading.at("device_id").is_string() || reading.at("device_id").get<std::string>().empty() ||
        !reading.contains("metric_name") || !reading.at("metric_name").is_string() || reading.at("metric_name").get<std::string>().empty() ||
        !reading.contains("value") || !reading.at("value").is_number() || !std::isfinite(reading.at("value").get<double>())) {
      return shared::Result<std::vector<IotReading>>::failure({"IOT_READING_INVALID", "IoT readings require finite device, metric, and value fields.", 422});
    }
    const auto unitValue = reading.find("unit");
    if (unitValue != reading.end() && !unitValue->is_string()) return shared::Result<std::vector<IotReading>>::failure({"IOT_READING_INVALID", "IoT reading units must be strings.", 422});
    const auto unit = unitValue == reading.end() ? std::string("ratio") : unitValue->get<std::string>();
    const auto observedValue = reading.find("observed_at");
    if (observedValue != reading.end() && !observedValue->is_string()) return shared::Result<std::vector<IotReading>>::failure({"IOT_READING_INVALID", "IoT observed_at must be an ISO timestamp.", 422});
    const auto observedAt = observedValue == reading.end() ? isoNow(now) : observedValue->get<std::string>();
    std::chrono::system_clock::time_point observedTime;
    if (!parseTimestamp(observedAt, observedTime)) return shared::Result<std::vector<IotReading>>::failure({"IOT_READING_INVALID", "IoT observed_at must be an ISO timestamp.", 422});
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - observedTime).count();
    const auto freshValue = reading.find("fresh");
    if (freshValue != reading.end() && !freshValue->is_boolean()) return shared::Result<std::vector<IotReading>>::failure({"IOT_READING_INVALID", "IoT freshness must be boolean.", 422});
    const auto explicitFresh = freshValue == reading.end() ? true : freshValue->get<bool>();
    const auto sourceValue = reading.find("source_event_id");
    if (sourceValue != reading.end() && !sourceValue->is_string()) return shared::Result<std::vector<IotReading>>::failure({"IOT_READING_INVALID", "IoT source_event_id must be a string.", 422});
    const auto sourceEvent = sourceValue == reading.end() ? std::string{} : sourceValue->get<std::string>();
    IotReading normalized;
    normalized.deviceId = reading.at("device_id").get<std::string>();
    normalized.metricName = reading.at("metric_name").get<std::string>();
    normalized.value = reading.at("value").get<double>();
    normalized.unit = unit;
    normalized.observedAt = observedAt;
    normalized.sourceEventId = sourceEvent.empty() ? derivedEventId(reading, normalized.observedAt, normalized.unit) : sourceEvent;
    normalized.fresh = explicitFresh && age >= 0 && age <= freshnessSeconds;
    result.push_back(std::move(normalized));
  }
  return shared::Result<std::vector<IotReading>>::success(std::move(result));
}

DeliveryResult AdapterContract::fixtureDelivery(const std::string& adapterType, const shared::Json& settings,
                                                 const std::string& outboxId, int attemptCount) {
  DeliveryResult result;
  if (!settings.is_object() || !settings.value("fixture_mode", true)) {
    result.disposition = attemptCount >= 4 ? DeliveryDisposition::permanent_failure : DeliveryDisposition::retryable_failure;
    result.statusCode = 0;
    result.errorCode = attemptCount >= 4 ? "ADAPTER_UNAVAILABLE" : "RETRYABLE_ADAPTER_FAILURE";
    return result;
  }
  const auto fixtureStatus = settings.value("fixture_status", 202);
  const auto behavior = settings.value("fixture_behavior", "success");
  if (behavior == "timeout_after_write") {
    result.disposition = DeliveryDisposition::ambiguous_delivery;
    result.errorCode = "AMBIGUOUS_DELIVERY";
    return result;
  }
  if (behavior == "malformed" || behavior == "redirect" || behavior == "tls_failure") {
    result.disposition = DeliveryDisposition::permanent_failure;
    result.errorCode = behavior == "malformed" ? "MALFORMED_ADAPTER_RESPONSE" : behavior == "redirect" ? "SSRF_REDIRECT_REJECTED" : "TLS_VALIDATION_FAILED";
    return result;
  }
  if (fixtureStatus == 401 || fixtureStatus == 403) {
    result.disposition = DeliveryDisposition::permanent_failure;
    result.statusCode = fixtureStatus;
    result.errorCode = "ADAPTER_UNAUTHORIZED";
    return result;
  }
  if (fixtureStatus == 429 || fixtureStatus >= 500) {
    result.statusCode = fixtureStatus;
    result.disposition = attemptCount >= 4 ? DeliveryDisposition::permanent_failure : DeliveryDisposition::retryable_failure;
    result.errorCode = attemptCount >= 4 ? "RETRY_EXHAUSTED" : "RETRYABLE_ADAPTER_FAILURE";
    return result;
  }
  if (fixtureStatus < 200 || fixtureStatus >= 300) {
    result.disposition = DeliveryDisposition::permanent_failure;
    result.statusCode = fixtureStatus;
    result.errorCode = "ADAPTER_REJECTED";
    return result;
  }
  result.disposition = DeliveryDisposition::published;
  result.statusCode = fixtureStatus;
  if (adapterType == "workflow_manual_v1") {
    result.externalReference = settings.value("fixture_execution_id", "fixture-execution-" + outboxId);
    result.externalStatus = settings.value("fixture_execution_status", "running");
  }
  return result;
}

DeliveryResult AdapterContract::liveDelivery(const std::string& adapterType, const std::string& endpoint, const std::string& apiKey,
                                              const shared::Json& payload, const std::string& idempotencyKey, const shared::Json& settings,
                                              shared::HttpClientPool& clientPool) {
  if (endpoint.empty() || apiKey.empty()) return {DeliveryDisposition::permanent_failure, 0, "ADAPTER_SECRET_UNAVAILABLE", {}, {}};
  std::string path;
  if (adapterType == "notification_hub_v1") path = "/api/events";
  else if (adapterType == "workflow_manual_v1") {
    const auto workflowId = settings.value("workflow_id", "");
    if (workflowId.empty()) return {DeliveryDisposition::permanent_failure, 0, "WORKFLOW_ID_REQUIRED", {}, {}};
    path = "/api/workflows/" + workflowId + "/execute";
  }
  else return {DeliveryDisposition::permanent_failure, 0, "ADAPTER_OPERATION_UNSUPPORTED", {}, {}};
  shared::HttpClientRequest request;
  request.method = "POST";
  request.url = appendPath(endpoint, path);
  request.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"}, {"X-API-Key", apiKey}, {"Idempotency-Key", idempotencyKey}};
  request.body = shared::CanonicalJson::serialize(adapterType == "workflow_manual_v1" ? shared::Json{{"trigger_data", payload}} : payload);
  request.timeoutSeconds = 10;
  if (settings.is_object() && settings.contains("timeout_seconds")) {
    const auto& timeout = settings.at("timeout_seconds");
    if (!timeout.is_number_integer() || timeout.get<int>() < 1 || timeout.get<int>() > 120) {
      return {DeliveryDisposition::permanent_failure, 0, "ADAPTER_TIMEOUT_INVALID", {}, {}};
    }
    request.timeoutSeconds = timeout.get<int>();
  }
  const auto response = clientPool.request(request);
  auto result = classifyResponse(response);
  if (result.disposition == DeliveryDisposition::published && adapterType == "workflow_manual_v1") {
    try {
      const auto body = shared::Json::parse(response.value->body);
      result.externalReference = body.value("execution_id", body.value("id", ""));
      result.externalStatus = body.value("status", "accepted");
      if (result.externalReference.empty()) return {DeliveryDisposition::permanent_failure, result.statusCode, "MALFORMED_ADAPTER_RESPONSE", {}, {}};
    } catch (const std::exception&) { return {DeliveryDisposition::permanent_failure, result.statusCode, "MALFORMED_ADAPTER_RESPONSE", {}, {}}; }
  }
  return result;
}

DeliveryResult AdapterContract::testConnection(const std::string& adapterType, const std::string& endpoint, const std::string& apiKey,
                                                const shared::Json& settings, shared::HttpClientPool& clientPool) {
  (void)settings;
  if (endpoint.empty() || apiKey.empty()) return {DeliveryDisposition::permanent_failure, 0, "ADAPTER_SECRET_UNAVAILABLE", {}, {}};
  shared::HttpClientRequest request;
  request.method = "GET";
  request.url = appendPath(endpoint, adapterType == "iot_rest_v1" ? "/api/health" : "/health");
  request.headers = {{"Accept", "application/json"}, {"X-API-Key", apiKey}};
  return classifyResponse(clientPool.request(request));
}

shared::Result<shared::Json> AdapterContract::liveIotReadings(const std::string& endpoint, const std::string& apiKey,
                                                              const std::string& cursor, shared::HttpClientPool& clientPool) {
  if (endpoint.empty() || apiKey.empty()) return shared::Result<shared::Json>::failure({"ADAPTER_SECRET_UNAVAILABLE", "The IoT adapter secret is unavailable.", 503});
  shared::HttpClientRequest request;
  request.method = "GET";
  request.url = appendPath(endpoint, "/api/readings");
  request.headers = {{"Accept", "application/json"}, {"X-API-Key", apiKey}};
  if (!cursor.empty()) request.headers["X-EdgeFleet-Cursor"] = cursor;
  const auto response = clientPool.request(request);
  if (!response.ok()) return shared::Result<shared::Json>::failure(*response.error);
  if (!isSuccess(response.value->statusCode)) return shared::Result<shared::Json>::failure({response.value->statusCode == 401 || response.value->statusCode == 403 ? "ADAPTER_UNAUTHORIZED" : "IOT_UPSTREAM_UNAVAILABLE", "The IoT adapter did not return readings.", response.value->statusCode >= 400 ? response.value->statusCode : 503});
  try {
    const auto body = shared::Json::parse(response.value->body);
    if (!body.is_array() && (!body.is_object() || !body.value("readings", shared::Json::array()).is_array())) return shared::Result<shared::Json>::failure({"IOT_RESPONSE_INVALID", "The IoT adapter response does not contain readings.", 502});
    return shared::Result<shared::Json>::success(body.is_array() ? shared::Json{{"readings", body}} : body);
  } catch (const std::exception&) { return shared::Result<shared::Json>::failure({"IOT_RESPONSE_INVALID", "The IoT adapter response is not valid JSON.", 502}); }
}

shared::Result<std::string> AdapterContract::liveWorkflowStatus(const std::string& endpoint, const std::string& apiKey,
                                                                const std::string& executionId, shared::HttpClientPool& clientPool) {
  if (endpoint.empty() || apiKey.empty() || executionId.empty()) return shared::Result<std::string>::failure({"WORKFLOW_STATUS_UNAVAILABLE", "The workflow execution cannot be observed.", 503});
  shared::HttpClientRequest request;
  request.method = "GET";
  request.url = appendPath(endpoint, "/api/executions/" + executionId);
  request.headers = {{"Accept", "application/json"}, {"X-API-Key", apiKey}};
  const auto response = clientPool.request(request);
  if (!response.ok()) return shared::Result<std::string>::failure(*response.error);
  if (!isSuccess(response.value->statusCode)) return shared::Result<std::string>::failure({"WORKFLOW_STATUS_UNAVAILABLE", "The workflow execution status is unavailable.", response.value->statusCode >= 400 ? response.value->statusCode : 503});
  try {
    const auto body = shared::Json::parse(response.value->body);
    const auto status = body.value("status", "");
    if (status.empty()) return shared::Result<std::string>::failure({"WORKFLOW_RESPONSE_INVALID", "The workflow status response is malformed.", 502});
    return shared::Result<std::string>::success(status);
  } catch (const std::exception&) { return shared::Result<std::string>::failure({"WORKFLOW_RESPONSE_INVALID", "The workflow status response is not valid JSON.", 502}); }
}

}  // namespace edgefleet::infrastructure
