#include "shared/http_client_pool.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include "shared/types.hpp"

#if defined(EDGEFLEET_HAS_DROGON)
#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>
#endif

namespace edgefleet::shared {

struct HttpClientPool::Impl {
#if defined(EDGEFLEET_HAS_DROGON)
  struct ClientSlot {
    std::mutex mutex;
    drogon::HttpClientPtr client;
  };
  trantor::EventLoopThread eventLoop{"edgefleet-adapters"};
  std::mutex clientsMutex;
  std::unordered_map<std::string, std::shared_ptr<ClientSlot>> clients;
#endif
};

namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return value;
}

#if defined(EDGEFLEET_HAS_DROGON)
struct ParsedUrl {
  std::string origin;
  std::string path;
  bool secure = false;
};

std::optional<ParsedUrl> parseUrl(const std::string& value) {
  const auto schemeEnd = value.find("://");
  if (schemeEnd == std::string::npos) return std::nullopt;
  const auto scheme = lower(value.substr(0, schemeEnd));
  if (scheme != "http" && scheme != "https") return std::nullopt;
  const auto authorityStart = schemeEnd + 3;
  const auto pathStart = value.find_first_of("/?#", authorityStart);
  const auto authority = value.substr(authorityStart, pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);
  if (authority.empty() || authority.find('@') != std::string::npos || authority.find_first_of("\\\r\n\t ") != std::string::npos) return std::nullopt;
  const auto path = pathStart == std::string::npos ? std::string("/") : value.substr(pathStart);
  if (path.find('#') != std::string::npos) return std::nullopt;
  return ParsedUrl{scheme + "://" + authority, path, scheme == "https"};
}

std::optional<drogon::HttpMethod> methodFromString(const std::string& method) {
  if (method == "GET") return drogon::Get;
  if (method == "POST") return drogon::Post;
  if (method == "PUT") return drogon::Put;
  if (method == "PATCH") return drogon::Patch;
  if (method == "DELETE") return drogon::Delete;
  return std::nullopt;
}
#endif

}  // namespace

HttpClientPool::RequestPermit::~RequestPermit() noexcept {
  if (pool == nullptr) return;
  std::lock_guard lock(pool->mutex_);
  if (pool->inFlight_ > 0) --pool->inFlight_;
  pool->capacityCondition_.notify_one();
}

HttpClientPool::HttpClientPool(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity), impl_(std::make_unique<Impl>()) {
#if defined(EDGEFLEET_HAS_DROGON)
  impl_->eventLoop.run();
#endif
}

HttpClientPool::~HttpClientPool() { close(); }

void HttpClientPool::close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  capacityCondition_.notify_all();
}

bool HttpClientPool::closed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

Result<HttpClientResponse> HttpClientPool::request(const HttpClientRequest& request) {
  {
    std::unique_lock lock(mutex_);
    if (closed_) return Result<HttpClientResponse>::failure({"HTTP_CLIENT_POOL_CLOSED", "The adapter HTTP client pool is closed.", 503});
    capacityCondition_.wait(lock, [this] { return closed_ || inFlight_ < capacity_; });
    if (closed_) return Result<HttpClientResponse>::failure({"HTTP_CLIENT_POOL_CLOSED", "The adapter HTTP client pool is closed.", 503});
    ++inFlight_;
  }
  RequestPermit permit{this};
#if !defined(EDGEFLEET_HAS_DROGON)
  (void)request;
  return Result<HttpClientResponse>::failure({"HTTP_CLIENT_UNAVAILABLE", "The native HTTPS client is not available in this build.", 503});
#else
  if (request.timeoutSeconds < 1 || request.timeoutSeconds > 120) return Result<HttpClientResponse>::failure({"HTTP_CLIENT_TIMEOUT_INVALID", "The adapter timeout is out of bounds.", 422});
  const auto parsed = parseUrl(request.url);
  const auto method = methodFromString(request.method);
  if (!parsed.has_value() || !method.has_value()) return Result<HttpClientResponse>::failure({"HTTP_CLIENT_REQUEST_INVALID", "The adapter request URL or method is invalid.", 422});
  std::shared_ptr<Impl::ClientSlot> slot;
  {
    std::lock_guard lock(impl_->clientsMutex);
    const auto existing = impl_->clients.find(parsed->origin);
    if (existing != impl_->clients.end()) slot = existing->second;
    if (!slot) {
      slot = std::make_shared<Impl::ClientSlot>();
      slot->client = drogon::HttpClient::newHttpClient(parsed->origin, impl_->eventLoop.getLoop(), false, true);
      if (slot->client) impl_->clients.emplace(parsed->origin, slot);
    }
  }
  if (!slot || !slot->client) return Result<HttpClientResponse>::failure({"HTTP_CLIENT_CREATE_FAILED", "The adapter HTTP client could not be created.", 503});
  std::lock_guard clientLock(slot->mutex);
  const auto outgoing = drogon::HttpRequest::newHttpRequest();
  outgoing->setMethod(*method);
  outgoing->setPath(parsed->path);
  outgoing->setPathEncode(false);
  // The embedded HTTP server closes each adapter connection after one response.
  // Making that contract explicit also prevents Drogon's origin pool from
  // reusing a socket that the peer has already closed.
  outgoing->removeHeader("connection");
  outgoing->addHeader("Connection", "close");
  if (!request.body.empty()) outgoing->setBody(request.body);
  for (const auto& [key, value] : request.headers) outgoing->addHeader(key, value);
  const auto response = slot->client->sendRequest(outgoing, request.timeoutSeconds);
  if (response.first != drogon::ReqResult::Ok || !response.second) {
    const auto code = response.first == drogon::ReqResult::Timeout ? "HTTP_TIMEOUT" : response.first == drogon::ReqResult::InvalidCertificate ? "TLS_VALIDATION_FAILED" : response.first == drogon::ReqResult::HandshakeError ? "TLS_HANDSHAKE_FAILED" : "HTTP_TRANSPORT_FAILED";
    return Result<HttpClientResponse>::failure({code, "The adapter request did not complete successfully.", 503});
  }
  HttpClientResponse result;
  result.statusCode = static_cast<int>(response.second->getStatusCode());
  result.body = std::string(response.second->body());
  for (const auto& [key, value] : response.second->getHeaders()) result.headers.emplace(key, value);
  return Result<HttpClientResponse>::success(std::move(result));
#endif
}

}  // namespace edgefleet::shared
