#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include "infrastructure/storage.hpp"

namespace edgefleet::application {

class JobCoordinator {
 public:
  static bool acquire(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                      const std::string& shardKey, const std::string& owner, int leaseSeconds);
  static bool release(infrastructure::Storage& storage, const std::string& tenantId, const std::string& jobName,
                      const std::string& shardKey, const std::string& owner);
  static int recoverExpired(infrastructure::Storage& storage, const std::string& tenantId);
};

struct MaintenanceSummary {
  int expiredApprovals = 0;
  int expiredCommands = 0;
  int deletedIdempotencyRecords = 0;
  int checkpointedTenants = 0;
  int recoveredLeases = 0;
};

struct WorkerSummary {
  int scheduledReleases = 0;
  int gateEvaluations = 0;
  int expiredApprovals = 0;
  int expiredCommands = 0;
  int freshnessUpdates = 0;
  int iotSamples = 0;
  int outboxPublished = 0;
  int outboxDeadLettered = 0;
  int simulationsCompleted = 0;
  int replaysCompleted = 0;
  int benchmarksCompleted = 0;
  int exportsCompleted = 0;
  int temporaryUploadsRemoved = 0;
};

class ScheduledReleaseStarter {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner);
};

class StageGateEvaluatorJob {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner);
};

class ApprovalExpiryScanner {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId);
};

class CommandExpiryScanner {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId);
};

class DeviceFreshnessProjector {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId, int freshnessSeconds = 120);
};

class IotHealthPoller {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId);
};

class OutboxPublisher {
 public:
  static std::pair<int, int> run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner);
};

class WorkflowExecutionObserver {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId);
};

class SimulationJobRunner {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& traceStorePath,
                 const std::string& owner);
};

class ReplayJobRunner {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId, const std::string& owner);
};

class BenchmarkJobRunner {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& exportStorePath,
                 const std::string& owner);
};

class EvidenceExportJobRunner {
 public:
  static int run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& exportStorePath,
                 const std::string& owner);
};

class WorkerCoordinator {
 public:
  static WorkerSummary run(infrastructure::Storage& storage, const std::string& tenantId, const std::filesystem::path& traceStorePath,
                           const std::filesystem::path& exportStorePath, const std::filesystem::path& artifactTempPath,
                           const std::string& owner);
};

class MaintenanceJobRunner {
 public:
  static MaintenanceSummary run(infrastructure::Storage& storage, const std::string& tenantId,
                                const std::string& owner, const std::filesystem::path& artifactTempPath = {});
  static int expireApprovals(infrastructure::Storage& storage, const std::string& tenantId);
  static int expireCommands(infrastructure::Storage& storage, const std::string& tenantId);
  static int cleanupIdempotency(infrastructure::Storage& storage, const std::string& tenantId);
  static bool checkpointEvidence(infrastructure::Storage& storage, const std::string& tenantId,
                                 std::int64_t checkpointInterval = 10000);
  static int cleanupTemporaryUploads(const std::filesystem::path& artifactTempPath, std::int64_t olderThanSeconds = 86400);
};

}  // namespace edgefleet::application
