#pragma once

#include <cstddef>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "shared/types.hpp"

namespace edgefleet::shared {

struct HttpClientRequest {
  std::string method = "GET";
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  int timeoutSeconds = 10;
};

struct HttpClientResponse {
  int statusCode = 0;
  std::map<std::string, std::string> headers;
  std::string body;
};

class HttpClientPool {
 public:
  explicit HttpClientPool(std::size_t capacity = 8);
  ~HttpClientPool();
  HttpClientPool(const HttpClientPool&) = delete;
  HttpClientPool& operator=(const HttpClientPool&) = delete;
  void close();
  bool closed() const;
  std::size_t capacity() const { return capacity_; }
  Result<HttpClientResponse> request(const HttpClientRequest& request);

 private:
  struct RequestPermit {
    HttpClientPool* pool;
    ~RequestPermit() noexcept;
  };
  struct Impl;
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable capacityCondition_;
  bool closed_ = false;
  std::size_t inFlight_ = 0;
  std::unique_ptr<Impl> impl_;
};

}  // namespace edgefleet::shared
