#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "shared/types.hpp"

namespace edgefleet::shared {

struct Config {
  std::string environment = "development";
  std::string host = "127.0.0.1";
  int port = 8080;
  std::string publicUrl = "http://localhost:8080";
  std::string logLevel = "info";
  std::string logFormat = "pretty";
  bool workerEnabled = true;
  int workerConcurrency = 1;
  bool selfRegistrationEnabled = true;
  std::string defaultTenantName = "Default";
  std::string defaultTenantLegalName = "Default";
  std::string defaultTenantTimezone = "UTC";
  std::string sessionEncryptionKey;
  std::string credentialEncryptionKey;
  std::string cursorHmacKey;
  std::string trustedProxyCidrs = "127.0.0.1/32";
  std::string privateAdapterCidrs;
  std::string storageBackend = "sqlite";
  std::string sqlitePath = "./data/edgefleet.db";
  int sqliteBusyTimeoutMs = 5000;
  std::string databaseUrl;
  int databasePoolSize = 20;
  std::string artifactStorePath = "./data/artifacts";
  std::string artifactTempPath = "./data/tmp";
  std::int64_t artifactMaxBytes = 100 * 1024 * 1024;
  std::int64_t artifactMinFreeBytes = 2LL * 1024 * 1024 * 1024;
  std::string traceStorePath = "./data/traces";
  std::string exportStorePath = "./data/exports";
  std::vector<int> stagePercentages{1, 5, 20, 50, 100};
  int minObservationSeconds = 900;
  int telemetryFreshnessSeconds = 120;
  double convergenceTarget = 0.98;
  double maxOfflineFraction = 0.20;
  int commandExpirySeconds = 604800;
  int simulatorMaxDevices = 100000;
  int simulatorMaxEvents = 10000000;
  int simulatorWorkerConcurrency = 1;
  std::string benchmarkCorpusPath = "./fixtures/benchmarks/v1/manifest.json";
  int replayCheckpointInterval = 10000;
  bool iotEnabled = false;
  std::string iotUrl = "http://localhost:3000";
  std::string iotApiKey;
  bool iotRequired = false;
  bool iotFixtureMode = true;
  bool notificationEnabled = false;
  std::string notificationUrl = "http://localhost:3847";
  std::string notificationApiKey;
  bool notificationFixtureMode = true;
  bool workflowEnabled = false;
  std::string workflowUrl = "http://localhost:8000";
  std::string workflowApiKey;
  std::string workflowId;
  bool workflowFixtureMode = true;
  std::string metricsHost = "127.0.0.1";
  int metricsPort = 9090;
  std::string otelEndpoint;
  std::string sentryDsn;
  std::string parseError;

  static Config load();
  static Config defaultsForTests();
  static std::optional<Error> validate(const Config& config);
};

bool isSafePrivateHttpUrl(std::string_view value);

}  // namespace edgefleet::shared
