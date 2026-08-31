#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edgefleet::web {

struct HttpRequest {
  std::string method;
  std::string target;
  std::map<std::string, std::string> headers;
  std::string body;
  // Large request bodies are spooled before dispatch so handlers can process them
  // without retaining the complete upload in the request heap.
  std::string bodyFilePath = {};
  std::size_t bodySize = 0;
};

struct HttpResponse {
  int status = 200;
  std::string contentType = "application/json";
  std::string body;
  std::map<std::string, std::string> headers;
  // A response may reference an immutable file range for bounded-memory downloads.
  std::string bodyFilePath = {};
  std::uintmax_t bodyFileOffset = 0;
  std::uintmax_t bodyFileLength = 0;
};

using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
  HttpServer(std::string host, int port, RequestHandler handler, std::string bodySpoolDirectory = {}, std::size_t maxBodyBytes = 100ULL * 1024ULL * 1024ULL);
  ~HttpServer();
  bool start();
  void stop();
  bool running() const { return running_; }
  int boundPort() const { return boundPort_; }

 private:
  void serveConnection(std::uintptr_t socket, std::string peerAddress);

  std::string host_;
  int port_;
  RequestHandler handler_;
  std::string bodySpoolDirectory_;
  std::size_t maxBodyBytes_;
  std::atomic<bool> running_{false};
  std::atomic<std::uintptr_t> listenSocket_{0};
  std::atomic<int> boundPort_{0};
  std::mutex clientsMutex_;
  std::vector<std::thread> clientThreads_;
};

}  // namespace edgefleet::web
