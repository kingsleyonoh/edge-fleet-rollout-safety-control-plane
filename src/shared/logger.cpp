#include "shared/logger.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <string>

#include "shared/canonical_json.hpp"

#if defined(EDGEFLEET_HAS_SPDLOG)
#include <spdlog/spdlog.h>
#endif

namespace edgefleet::shared {
namespace {

std::mutex loggerMutex;
bool jsonFormat = true;
int minimumLevel = 2;

int levelRank(std::string_view level) {
  if (level == "trace") return 0;
  if (level == "debug") return 1;
  if (level == "info") return 2;
  if (level == "warn") return 3;
  return 4;
}

bool sensitiveKey(std::string_view key) {
  std::string lower(key);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return lower.find("secret") != std::string::npos || lower.find("token") != std::string::npos || lower.find("password") != std::string::npos ||
         lower.find("api_key") != std::string::npos || lower.find("apikey") != std::string::npos || lower.find("credential") != std::string::npos ||
         lower.find("ciphertext") != std::string::npos || lower.find("private_key") != std::string::npos;
}

Json redact(const Json& value, std::string_view key = {}) {
  if (!key.empty() && sensitiveKey(key)) return "[REDACTED]";
  if (value.is_object()) {
    Json result = Json::object();
    for (const auto& [childKey, childValue] : value.items()) result[childKey] = redact(childValue, childKey);
    return result;
  }
  if (value.is_array()) {
    Json result = Json::array();
    for (const auto& child : value) result.push_back(redact(child));
    return result;
  }
  return value;
}

}  // namespace

void Logger::configure(std::string_view level, std::string_view format) {
  std::lock_guard lock(loggerMutex);
  jsonFormat = format != "pretty";
  minimumLevel = levelRank(level);
#if defined(EDGEFLEET_HAS_SPDLOG)
  spdlog::set_pattern("%v");
  if (level == "trace") spdlog::set_level(spdlog::level::trace);
  else if (level == "debug") spdlog::set_level(spdlog::level::debug);
  else if (level == "warn") spdlog::set_level(spdlog::level::warn);
  else if (level == "error") spdlog::set_level(spdlog::level::err);
  else spdlog::set_level(spdlog::level::info);
#endif
}

void Logger::event(std::string_view level, std::string_view eventName, Json fields) {
  std::lock_guard lock(loggerMutex);
  if (levelRank(level) < minimumLevel) return;
  auto payload = redact(fields);
  if (!payload.is_object()) payload = Json{{"value", payload}};
  payload["event"] = eventName;
  payload["level"] = level;
  const auto serialized = jsonFormat ? CanonicalJson::serialize(payload) : std::string(eventName) + " " + payload.dump();
#if defined(EDGEFLEET_HAS_SPDLOG)
  if (level == "error") spdlog::error("{}", serialized);
  else if (level == "warn") spdlog::warn("{}", serialized);
  else if (level == "debug") spdlog::debug("{}", serialized);
  else spdlog::info("{}", serialized);
#else
  std::cerr << serialized << '\n';
#endif
}

}  // namespace edgefleet::shared
