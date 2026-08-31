#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "shared/http_client_pool.hpp"
#include "shared/types.hpp"

namespace edgefleet::infrastructure {

enum class AdapterType { iot_rest_v1, notification_hub_v1, workflow_manual_v1 };

std::optional<AdapterType> adapterTypeFromString(const std::string& value);
std::string_view adapterTypeName(AdapterType value);
bool isSupportedAdapterType(const std::string& value);

struct IotReading {
  std::string deviceId;
  std::string metricName;
  double value = 0.0;
  std::string unit = "ratio";
  std::string observedAt;
  std::string sourceEventId;
  bool fresh = false;
};

class IotFixtureParser {
 public:
  static shared::Result<std::vector<IotReading>> parse(const shared::Json& settings,
                                                        std::chrono::system_clock::time_point now =
                                                            std::chrono::system_clock::now());
};

enum class DeliveryDisposition { published, retryable_failure, permanent_failure, ambiguous_delivery };

struct DeliveryResult {
  DeliveryDisposition disposition = DeliveryDisposition::permanent_failure;
  int statusCode = 0;
  std::string errorCode;
  std::string externalReference;
  std::string externalStatus;
};

class Adapter {
 public:
  explicit Adapter(AdapterType type) : type_(type) {}
  virtual ~Adapter() = default;
  AdapterType type() const noexcept { return type_; }
  std::string_view name() const noexcept { return adapterTypeName(type_); }
  DeliveryResult fixtureDelivery(const shared::Json& settings, const std::string& outboxId, int attemptCount) const;
  DeliveryResult liveDelivery(const std::string& endpoint, const std::string& apiKey, const shared::Json& payload,
                              const std::string& idempotencyKey, const shared::Json& settings, shared::HttpClientPool& clientPool) const;
  DeliveryResult testConnection(const std::string& endpoint, const std::string& apiKey, const shared::Json& settings,
                                shared::HttpClientPool& clientPool) const;

 private:
  AdapterType type_;
};

std::unique_ptr<Adapter> createAdapter(AdapterType type);
std::unique_ptr<Adapter> createAdapter(const std::string& value);

class AdapterContract {
 public:
  static DeliveryResult fixtureDelivery(const std::string& adapterType, const shared::Json& settings,
                                        const std::string& outboxId, int attemptCount);
  static DeliveryResult liveDelivery(const std::string& adapterType, const std::string& endpoint, const std::string& apiKey,
                                     const shared::Json& payload, const std::string& idempotencyKey, const shared::Json& settings,
                                     shared::HttpClientPool& clientPool);
  static DeliveryResult testConnection(const std::string& adapterType, const std::string& endpoint, const std::string& apiKey,
                                       const shared::Json& settings, shared::HttpClientPool& clientPool);
  static shared::Result<shared::Json> liveIotReadings(const std::string& endpoint, const std::string& apiKey,
                                                      const std::string& cursor, shared::HttpClientPool& clientPool);
  static shared::Result<std::string> liveWorkflowStatus(const std::string& endpoint, const std::string& apiKey,
                                                        const std::string& executionId, shared::HttpClientPool& clientPool);
};

}  // namespace edgefleet::infrastructure
