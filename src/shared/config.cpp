#include "shared/config.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <charconv>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#endif

namespace edgefleet::shared {
namespace {

std::string envOr(std::string_view name, std::string fallback) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, std::string(name).c_str()) != 0 || value == nullptr) return std::move(fallback);
  std::string result(value, length == 0 ? 0 : length - 1);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(std::string(name).c_str());
  return value == nullptr ? std::move(fallback) : std::string(value);
#endif
}

bool parseBool(std::string_view value, bool fallback) {
  if (value == "true" || value == "1" || value == "yes") return true;
  if (value == "false" || value == "0" || value == "no") return false;
  return fallback;
}

int parseInt(std::string_view value, int fallback) {
  int result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? result : fallback;
}

std::string generatedSecret() {
  std::random_device device;
  std::mt19937_64 generator(device());
  std::uniform_int_distribution<unsigned int> distribution(0, 255);
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (int index = 0; index < 32; ++index) result << std::setw(2) << distribution(generator);
  return result.str();
}

bool protectRuntimeSecretFile(const std::filesystem::path& path) {
#ifdef _WIN32
  DWORD usernameLength = 0;
  GetUserNameW(nullptr, &usernameLength);
  if (usernameLength == 0) return false;
  std::vector<wchar_t> username(usernameLength);
  if (!GetUserNameW(username.data(), &usernameLength)) return false;
  EXPLICIT_ACCESSW access{};
  BuildExplicitAccessWithNameW(&access, username.data(), GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL | WRITE_DAC, SET_ACCESS, NO_INHERITANCE);
  PACL dacl = nullptr;
  if (SetEntriesInAclW(1, &access, nullptr, &dacl) != ERROR_SUCCESS) return false;
  auto nativePath = path.wstring();
  const auto result = SetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
  LocalFree(dacl);
  return result == ERROR_SUCCESS;
#else
  std::error_code error;
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
  return !error;
#endif
}

std::string runtimeSecret(std::string_view name, std::string supplied, bool persist) {
  if (!supplied.empty() || !persist) return supplied;
  const auto path = std::filesystem::path("local-secrets") / "runtime.env";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::string value;
  if (!error) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
      const auto equals = line.find('=');
      if (equals != std::string::npos && line.substr(0, equals) == name) { value = line.substr(equals + 1); break; }
    }
  }
  if (value.empty()) {
    value = generatedSecret();
    std::ofstream output(path, std::ios::app);
    if (!output) return {};
    output << name << "=" << value << "\n";
    output.close();
    if (!protectRuntimeSecretFile(path)) return {};
  }
  return value;
}

bool safeLocalUrl(std::string_view value) {
  const auto scheme = value.find("://");
  if (scheme == std::string_view::npos || (value.substr(0, scheme) != "http" && value.substr(0, scheme) != "https")) return false;
  const auto start = scheme + 3;
  const auto end = value.find_first_of("/?#", start);
  const auto authority = value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  std::string host;
  if (authority.front() == '[') {
    const auto closing = authority.find(']');
    if (closing == std::string::npos || (closing + 1 < authority.size() && authority[closing + 1] != ':')) return false;
    host = authority.substr(1, closing - 1);
    if (closing + 1 < authority.size()) {
      const auto port = authority.substr(closing + 2);
      unsigned int parsed = 0;
      const auto result = std::from_chars(port.data(), port.data() + port.size(), parsed);
      if (port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() || parsed > 65535) return false;
    }
  } else {
    const auto colon = authority.find(':');
    if (colon != std::string::npos && authority.find(':', colon + 1) != std::string::npos) return false;
    host = authority.substr(0, colon);
    if (colon != std::string::npos) {
      const auto port = authority.substr(colon + 1);
      unsigned int parsed = 0;
      const auto result = std::from_chars(port.data(), port.data() + port.size(), parsed);
      if (port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() || parsed > 65535) return false;
    }
  }
  if (host == "localhost" || host == "::1") return true;
  std::array<unsigned int, 4> octets{};
  std::size_t offset = 0;
  for (std::size_t index = 0; index < octets.size(); ++index) {
    const auto dot = host.find('.', offset);
    const auto part = host.substr(offset, dot == std::string::npos ? std::string::npos : dot - offset);
    if (part.empty() || part.size() > 3) return false;
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), octets[index]);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || octets[index] > 255) return false;
    if (dot == std::string::npos) {
      if (index != octets.size() - 1) return false;
      offset = host.size();
    } else {
      if (index == octets.size() - 1) return false;
      offset = dot + 1;
    }
  }
  return octets[0] == 127 || octets[0] == 10 || (octets[0] == 192 && octets[1] == 168) || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31);
}

bool validHttpUrl(std::string_view value) {
  const auto scheme = value.find("://");
  if (scheme == std::string_view::npos || (value.substr(0, scheme) != "http" && value.substr(0, scheme) != "https")) return false;
  const auto start = scheme + 3;
  const auto end = value.find_first_of("/?#", start);
  const auto authority = value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
  if (authority.empty() || authority.find('@') != std::string_view::npos) return false;
  for (const auto character : authority) if (std::isspace(static_cast<unsigned char>(character)) || character == '\\') return false;
  if (authority.front() == '[') {
    const auto closing = authority.find(']');
    if (closing == std::string_view::npos || (closing + 1 < authority.size() && authority[closing + 1] != ':')) return false;
    if (closing + 1 < authority.size()) {
      const auto port = authority.substr(closing + 2);
      unsigned int parsed = 0;
      const auto result = std::from_chars(port.data(), port.data() + port.size(), parsed);
      if (port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() || parsed > 65535) return false;
    }
    return closing > 1;
  }
  const auto colon = authority.find(':');
  if (colon != std::string_view::npos && authority.find(':', colon + 1) != std::string_view::npos) return false;
  if (colon == 0) return false;
  if (colon != std::string_view::npos) {
    const auto port = authority.substr(colon + 1);
    unsigned int parsed = 0;
    const auto result = std::from_chars(port.data(), port.data() + port.size(), parsed);
    if (port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() || parsed > 65535) return false;
  }
  return !authority.substr(0, colon).empty();
}

bool validCidrList(std::string_view value) {
  if (value.empty()) return true;
  std::stringstream stream{std::string(value)};
  std::string entry;
  while (std::getline(stream, entry, ',')) {
    const auto slash = entry.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= entry.size()) return false;
    unsigned int prefix = 0;
    const auto prefixText = entry.substr(slash + 1);
    const auto parsed = std::from_chars(prefixText.data(), prefixText.data() + prefixText.size(), prefix);
    if (parsed.ec != std::errc{} || parsed.ptr != prefixText.data() + prefixText.size() || prefix > (entry.front() == ':' ? 128U : 32U)) return false;
    const auto host = entry.substr(0, slash);
    for (const auto character : host) if (std::isspace(static_cast<unsigned char>(character))) return false;
  }
  return true;
}

std::vector<int> parseStages(std::string_view value, std::vector<int> fallback) {
  std::vector<int> result;
  std::stringstream stream{std::string(value)};
  std::string part;
  while (std::getline(stream, part, ',')) result.push_back(parseInt(part, -1));
  return result.empty() ? std::move(fallback) : result;
}

}  // namespace

bool isSafePrivateHttpUrl(std::string_view value) { return safeLocalUrl(value); }

Config Config::defaultsForTests() {
  Config config;
  config.sessionEncryptionKey = "test-session-key";
  config.credentialEncryptionKey = "test-credential-key";
  config.cursorHmacKey = "test-cursor-key";
  return config;
}

Config Config::load() {
  Config config = defaultsForTests();
  const auto markInvalid = [&config](std::string_view name) {
    if (config.parseError.empty()) config.parseError = std::string(name) + " is not a valid value";
  };
  const auto intEnv = [&markInvalid](std::string_view name, int fallback) {
    const auto raw = envOr(name, std::to_string(fallback));
    int parsed = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) { markInvalid(name); return fallback; }
    return parsed;
  };
  const auto int64Env = [&markInvalid](std::string_view name, std::int64_t fallback) {
    const auto raw = envOr(name, std::to_string(fallback));
    std::int64_t parsed = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) { markInvalid(name); return fallback; }
    return parsed;
  };
  const auto doubleEnv = [&markInvalid](std::string_view name, double fallback) {
    const auto raw = envOr(name, std::to_string(fallback));
    try {
      std::size_t consumed = 0;
      const auto parsed = std::stod(raw, &consumed);
      if (consumed != raw.size() || !std::isfinite(parsed)) { markInvalid(name); return fallback; }
      return parsed;
    } catch (const std::exception&) { markInvalid(name); return fallback; }
  };
  const auto boolEnv = [&markInvalid](std::string_view name, bool fallback) {
    const auto raw = envOr(name, fallback ? "true" : "false");
    if (raw != "true" && raw != "1" && raw != "yes" && raw != "false" && raw != "0" && raw != "no") { markInvalid(name); return fallback; }
    return parseBool(raw, fallback);
  };
  config.environment = envOr("EDGEFLEET_ENV", config.environment);
  config.host = envOr("EDGEFLEET_HOST", config.host);
  config.port = intEnv("EDGEFLEET_PORT", config.port);
  config.publicUrl = envOr("EDGEFLEET_PUBLIC_URL", config.publicUrl);
  config.logLevel = envOr("EDGEFLEET_LOG_LEVEL", config.logLevel);
  config.logFormat = envOr("EDGEFLEET_LOG_FORMAT", config.logFormat);
  config.workerEnabled = boolEnv("EDGEFLEET_WORKER_ENABLED", true);
  config.workerConcurrency = intEnv("EDGEFLEET_WORKER_CONCURRENCY", 1);
  config.selfRegistrationEnabled = boolEnv("SELF_REGISTRATION_ENABLED", true);
  config.defaultTenantName = envOr("DEFAULT_TENANT_NAME", config.defaultTenantName);
  config.defaultTenantLegalName = envOr("DEFAULT_TENANT_LEGAL_NAME", config.defaultTenantLegalName);
  config.defaultTenantTimezone = envOr("DEFAULT_TENANT_TIMEZONE", config.defaultTenantTimezone);
  config.sessionEncryptionKey = envOr("SESSION_ENCRYPTION_KEY", "");
  config.credentialEncryptionKey = envOr("CREDENTIAL_ENCRYPTION_KEY", "");
  config.cursorHmacKey = envOr("CURSOR_HMAC_KEY", "");
  config.trustedProxyCidrs = envOr("TRUSTED_PROXY_CIDRS", config.trustedProxyCidrs);
  config.privateAdapterCidrs = envOr("PRIVATE_ADAPTER_CIDRS", config.privateAdapterCidrs);
  config.storageBackend = envOr("STORAGE_BACKEND", config.storageBackend);
  config.sqlitePath = envOr("SQLITE_PATH", config.sqlitePath);
  config.sqliteBusyTimeoutMs = intEnv("SQLITE_BUSY_TIMEOUT_MS", 5000);
  config.databaseUrl = envOr("DATABASE_URL", "");
  config.databasePoolSize = intEnv("DATABASE_POOL_SIZE", 20);
  config.artifactStorePath = envOr("ARTIFACT_STORE_PATH", config.artifactStorePath);
  config.artifactTempPath = envOr("ARTIFACT_TEMP_PATH", config.artifactTempPath);
  config.artifactMaxBytes = int64Env("ARTIFACT_MAX_BYTES", config.artifactMaxBytes);
  config.artifactMinFreeBytes = int64Env("ARTIFACT_MIN_FREE_BYTES", config.artifactMinFreeBytes);
  config.traceStorePath = envOr("TRACE_STORE_PATH", config.traceStorePath);
  config.exportStorePath = envOr("EXPORT_STORE_PATH", config.exportStorePath);
  const auto stageText = envOr("DEFAULT_STAGE_PERCENTAGES", "1,5,20,50,100");
  config.stagePercentages = parseStages(stageText, config.stagePercentages);
  if (std::any_of(config.stagePercentages.begin(), config.stagePercentages.end(), [](int value) { return value < 1; })) markInvalid("DEFAULT_STAGE_PERCENTAGES");
  config.minObservationSeconds = intEnv("DEFAULT_MIN_OBSERVATION_SECONDS", 900);
  config.telemetryFreshnessSeconds = intEnv("DEFAULT_TELEMETRY_FRESHNESS_SECONDS", 120);
  config.convergenceTarget = doubleEnv("DEFAULT_CONVERGENCE_TARGET", 0.98);
  config.maxOfflineFraction = doubleEnv("DEFAULT_MAX_OFFLINE_FRACTION", 0.20);
  config.commandExpirySeconds = intEnv("COMMAND_EXPIRY_SECONDS", 604800);
  config.simulatorMaxDevices = intEnv("SIMULATOR_MAX_DEVICES", 100000);
  config.simulatorMaxEvents = intEnv("SIMULATOR_MAX_EVENTS", 10000000);
  config.simulatorWorkerConcurrency = intEnv("SIMULATOR_WORKER_CONCURRENCY", 1);
  config.benchmarkCorpusPath = envOr("BENCHMARK_CORPUS_PATH", config.benchmarkCorpusPath);
  config.replayCheckpointInterval = intEnv("REPLAY_CHECKPOINT_INTERVAL", 10000);
  config.iotEnabled = boolEnv("IOT_AGGREGATOR_ENABLED", false);
  config.iotUrl = envOr("IOT_AGGREGATOR_URL", config.iotUrl);
  config.iotApiKey = envOr("IOT_AGGREGATOR_API_KEY", "");
  config.iotRequired = boolEnv("IOT_AGGREGATOR_REQUIRED_FOR_PROMOTION", false);
  config.iotFixtureMode = boolEnv("IOT_AGGREGATOR_FIXTURE_MODE", true);
  config.notificationEnabled = boolEnv("NOTIFICATION_HUB_ENABLED", false);
  config.notificationUrl = envOr("NOTIFICATION_HUB_URL", config.notificationUrl);
  config.notificationApiKey = envOr("NOTIFICATION_HUB_API_KEY", "");
  config.notificationFixtureMode = boolEnv("NOTIFICATION_HUB_FIXTURE_MODE", true);
  config.workflowEnabled = boolEnv("WORKFLOW_ENGINE_ENABLED", false);
  config.workflowUrl = envOr("WORKFLOW_ENGINE_URL", config.workflowUrl);
  config.workflowApiKey = envOr("WORKFLOW_ENGINE_API_KEY", "");
  config.workflowId = envOr("WORKFLOW_ENGINE_WORKFLOW_ID", "");
  config.workflowFixtureMode = boolEnv("WORKFLOW_ENGINE_FIXTURE_MODE", true);
  config.metricsHost = envOr("METRICS_BIND_HOST", config.metricsHost);
  config.metricsPort = intEnv("METRICS_PORT", 9090);
  config.otelEndpoint = envOr("OTEL_EXPORTER_OTLP_ENDPOINT", "");
  config.sentryDsn = envOr("SENTRY_DSN", "");
  const auto persistSecrets = config.environment != "production";
  config.sessionEncryptionKey = runtimeSecret("SESSION_ENCRYPTION_KEY", config.sessionEncryptionKey, persistSecrets);
  config.credentialEncryptionKey = runtimeSecret("CREDENTIAL_ENCRYPTION_KEY", config.credentialEncryptionKey, persistSecrets);
  config.cursorHmacKey = runtimeSecret("CURSOR_HMAC_KEY", config.cursorHmacKey, persistSecrets);
  return config;
}

std::optional<Error> Config::validate(const Config& config) {
  if (!config.parseError.empty()) return Error{"INVALID_CONFIGURATION", config.parseError, 422};
  if (config.environment != "development" && config.environment != "test" && config.environment != "production") return Error{"INVALID_ENVIRONMENT", "EDGEFLEET_ENV is unsupported.", 422};
  if (config.host.empty() || !validHttpUrl(config.publicUrl)) return Error{"INVALID_HOST", "EDGEFLEET_HOST and EDGEFLEET_PUBLIC_URL must be valid.", 422};
  if (config.port < 1 || config.port > 65535) return Error{"INVALID_PORT", "EDGEFLEET_PORT must be between 1 and 65535.", 422};
  if (config.logLevel != "trace" && config.logLevel != "debug" && config.logLevel != "info" && config.logLevel != "warn" && config.logLevel != "error") return Error{"INVALID_LOG_LEVEL", "EDGEFLEET_LOG_LEVEL is unsupported.", 422};
  if (config.logFormat != "json" && config.logFormat != "pretty") return Error{"INVALID_LOG_FORMAT", "EDGEFLEET_LOG_FORMAT is unsupported.", 422};
  if (config.storageBackend != "sqlite" && config.storageBackend != "postgres") return Error{"INVALID_STORAGE_BACKEND", "STORAGE_BACKEND must be sqlite or postgres.", 422};
  if (config.storageBackend == "postgres" && config.databaseUrl.empty()) return Error{"DATABASE_URL_REQUIRED", "PostgreSQL storage requires DATABASE_URL.", 422};
  if (!validCidrList(config.trustedProxyCidrs) || !validCidrList(config.privateAdapterCidrs)) return Error{"INVALID_CIDR", "Proxy and adapter CIDR lists are invalid.", 422};
  if (config.sqlitePath.empty() || config.artifactStorePath.empty() || config.artifactTempPath.empty() || config.traceStorePath.empty() || config.exportStorePath.empty()) return Error{"INVALID_STORAGE_PATH", "Runtime storage paths cannot be empty.", 422};
  if (config.sqliteBusyTimeoutMs < 0 || config.sqliteBusyTimeoutMs > 120000) return Error{"INVALID_SQLITE_TIMEOUT", "SQLITE_BUSY_TIMEOUT_MS is out of bounds.", 422};
  if (config.workerConcurrency < 1 || config.workerConcurrency > 32) return Error{"INVALID_WORKER_CONCURRENCY", "Worker concurrency is out of bounds.", 422};
  if (config.databasePoolSize < 1 || config.databasePoolSize > 200) return Error{"INVALID_DATABASE_POOL_SIZE", "Database pool size is out of bounds.", 422};
  if (config.artifactMaxBytes < 1 || config.artifactMinFreeBytes < 0) return Error{"INVALID_ARTIFACT_LIMIT", "Artifact storage limits are invalid.", 422};
  if (config.minObservationSeconds < 1 || config.minObservationSeconds > 7 * 24 * 60 * 60 || config.telemetryFreshnessSeconds < 1 || config.telemetryFreshnessSeconds > 24 * 60 * 60 || config.commandExpirySeconds < 1 || config.commandExpirySeconds > 30 * 24 * 60 * 60) return Error{"INVALID_TIMING_DEFAULT", "Safety timing defaults are out of bounds.", 422};
  if (config.simulatorMaxDevices < 1 || config.simulatorMaxDevices > 1000000 || config.simulatorMaxEvents < 1 || config.simulatorMaxEvents > 100000000 || config.simulatorWorkerConcurrency < 1 || config.simulatorWorkerConcurrency > 32 || config.replayCheckpointInterval < 1) return Error{"INVALID_SIMULATOR_LIMIT", "Simulation and replay limits are out of bounds.", 422};
  if (config.stagePercentages.empty() || config.stagePercentages.size() > 5 || config.stagePercentages.back() != 100) return Error{"INVALID_STAGE_PLAN", "Stage percentages must end at 100 and contain at most five stages.", 422};
  for (std::size_t index = 0; index < config.stagePercentages.size(); ++index) {
    if (config.stagePercentages[index] <= 0 || (index > 0 && config.stagePercentages[index] <= config.stagePercentages[index - 1])) return Error{"INVALID_STAGE_PLAN", "Stage percentages must increase strictly.", 422};
  }
  if (config.convergenceTarget <= 0 || config.convergenceTarget > 1 || config.maxOfflineFraction < 0 || config.maxOfflineFraction > 1) return Error{"INVALID_RELEASE_DEFAULT", "Release ratios must be within (0,1].", 422};
  if (config.metricsHost.empty() || config.metricsPort < 1 || config.metricsPort > 65535) return Error{"INVALID_METRICS_BIND", "Metrics binding is invalid.", 422};
  if (config.metricsHost == config.host && config.metricsPort == config.port) return Error{"METRICS_BIND_CONFLICT", "The metrics listener must use a separate host or port.", 422};
  if (!isSafePrivateHttpUrl(config.iotUrl) || !isSafePrivateHttpUrl(config.notificationUrl) || !isSafePrivateHttpUrl(config.workflowUrl)) return Error{"INVALID_ADAPTER_URL", "Adapter URLs must be explicit private or loopback HTTP(S) endpoints.", 422};
  if ((config.iotEnabled && !config.iotFixtureMode && config.iotUrl.starts_with("http://")) ||
      (config.notificationEnabled && !config.notificationFixtureMode && config.notificationUrl.starts_with("http://")) ||
      (config.workflowEnabled && !config.workflowFixtureMode && config.workflowUrl.starts_with("http://"))) return Error{"ADAPTER_HTTPS_REQUIRED", "Live adapters require HTTPS endpoints.", 422};
  if (config.workflowEnabled && config.workflowId.empty()) return Error{"WORKFLOW_ID_REQUIRED", "Workflow integration requires a workflow identifier.", 422};
  if (config.environment == "production" && (config.sessionEncryptionKey.empty() || config.credentialEncryptionKey.empty() || config.cursorHmacKey.empty() || config.sessionEncryptionKey.starts_with("dev_") || config.credentialEncryptionKey.starts_with("dev_") || config.cursorHmacKey.starts_with("dev_"))) return Error{"PRODUCTION_SECRET_REQUIRED", "Production requires host-managed runtime secrets.", 422};
  if (config.environment == "production" && config.publicUrl.starts_with("http://")) return Error{"PRODUCTION_HTTPS_REQUIRED", "Production public URLs must use HTTPS.", 422};
  return std::nullopt;
}

}  // namespace edgefleet::shared
