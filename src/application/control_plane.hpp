#pragma once

#include <memory>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "infrastructure/storage.hpp"
#include "shared/config.hpp"
#include "web/http_server.hpp"

namespace edgefleet::application {

class ControlPlane {
 public:
  explicit ControlPlane(shared::Config config);

  bool initialize(const std::string& migrationDirectory);
  web::HttpResponse handle(const web::HttpRequest& request);
  infrastructure::Storage* storage() { return storage_.get(); }
  const shared::Config& config() const { return config_; }

 private:
  struct AuthResult {
    shared::TenantContext context;
    shared::Json principal;
    std::string traceId;
  };

  AuthResult authenticate(const web::HttpRequest& request);
  web::HttpResponse dispatch(const web::HttpRequest& request, const AuthResult& auth);
  web::HttpResponse health(const web::HttpRequest& request) const;
  std::optional<web::HttpResponse> rateLimit(const web::HttpRequest& request, const AuthResult& auth);
  web::HttpResponse metrics() const;
  web::HttpResponse releaseRoute(const web::HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts);
  web::HttpResponse simulationRoute(const web::HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts);
  web::HttpResponse evidenceRoute(const web::HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts);
  web::HttpResponse agentRoute(const web::HttpRequest& request, const AuthResult& auth, const std::vector<std::string>& parts);
  web::HttpResponse htmlRoute(const web::HttpRequest& request, const AuthResult& auth);

  shared::Config config_;
  std::unique_ptr<infrastructure::Storage> storage_;
  bool initialized_ = false;
  struct RateBucket {
    std::chrono::steady_clock::time_point windowStarted;
    int requests = 0;
  };
  mutable std::mutex rateLimitMutex_;
  std::map<std::string, RateBucket> rateLimits_;
  std::atomic<std::uint64_t> requestCount_{0};
  std::atomic<std::uint64_t> desiredStateRequests_{0};
  std::atomic<std::uint64_t> deviceReportRequests_{0};
  std::atomic<std::uint64_t> evidenceAppendFailures_{0};
};

}  // namespace edgefleet::application
