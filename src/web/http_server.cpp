#include "web/http_server.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
constexpr SocketType kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketType = int;
constexpr SocketType kInvalidSocket = -1;
#endif

namespace edgefleet::web {
namespace {

void closeSocket(SocketType socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return value;
}

HttpRequest parseRequest(const std::string& raw) {
  HttpRequest request;
  const auto separator = raw.find("\r\n\r\n");
  const auto headerText = raw.substr(0, separator);
  request.body = separator == std::string::npos ? "" : raw.substr(separator + 4);
  std::istringstream stream(headerText);
  std::string line;
  if (std::getline(stream, line)) {
    std::istringstream first(line);
    first >> request.method >> request.target;
  }
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto key = lower(line.substr(0, colon));
    auto value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    request.headers[key] = value;
  }
  return request;
}

std::string reason(int status) {
  switch (status) { case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 204: return "No Content"; case 206: return "Partial Content"; case 301: return "Moved Permanently"; case 302: return "Found"; case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 409: return "Conflict"; case 413: return "Payload Too Large"; case 416: return "Range Not Satisfiable"; case 422: return "Unprocessable Entity"; case 429: return "Too Many Requests"; case 500: return "Internal Server Error"; case 503: return "Service Unavailable"; case 507: return "Insufficient Storage"; default: return "Internal Server Error"; }
}

}  // namespace

HttpServer::HttpServer(std::string host, int port, RequestHandler handler, std::string bodySpoolDirectory, std::size_t maxBodyBytes)
    : host_(std::move(host)), port_(port), handler_(std::move(handler)), bodySpoolDirectory_(std::move(bodySpoolDirectory)), maxBodyBytes_(maxBodyBytes) {}
HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
#ifdef _WIN32
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
  const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) return false;
  int enabled = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<unsigned short>(port_));
  if (host_ == "0.0.0.0") address.sin_addr.s_addr = htonl(INADDR_ANY);
  else if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) { closeSocket(socket); return false; }
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(socket, 32) != 0) { closeSocket(socket); return false; }
  sockaddr_in bound{};
#ifdef _WIN32
  int boundLength = sizeof(bound);
#else
  socklen_t boundLength = sizeof(bound);
#endif
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) { closeSocket(socket); return false; }
  boundPort_ = ntohs(bound.sin_port);
  listenSocket_.store(static_cast<std::uintptr_t>(socket));
  running_ = true;
  while (running_) {
    sockaddr_storage peer{};
#ifdef _WIN32
    int peerLength = sizeof(peer);
#else
    socklen_t peerLength = sizeof(peer);
#endif
    const auto client = ::accept(socket, reinterpret_cast<sockaddr*>(&peer), &peerLength);
    if (client == kInvalidSocket) { if (running_) continue; break; }
    char peerText[INET6_ADDRSTRLEN]{};
    const void* peerBytes = peer.ss_family == AF_INET ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr) : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_addr);
    const auto peerAddress = inet_ntop(peer.ss_family, peerBytes, peerText, sizeof(peerText)) == nullptr ? std::string("unknown") : std::string(peerText);
    std::lock_guard lock(clientsMutex_);
    clientThreads_.emplace_back(&HttpServer::serveConnection, this, static_cast<std::uintptr_t>(client), peerAddress);
  }
  closeSocket(socket);
  listenSocket_.store(0);
  boundPort_ = 0;
  {
    std::lock_guard lock(clientsMutex_);
    for (auto& clientThread : clientThreads_) if (clientThread.joinable()) clientThread.join();
    clientThreads_.clear();
  }
#ifdef _WIN32
  WSACleanup();
#endif
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) return;
  const auto socket = listenSocket_.load();
  if (socket != 0) {
#ifdef _WIN32
    shutdown(static_cast<SocketType>(socket), SD_BOTH);
#else
    shutdown(static_cast<SocketType>(socket), SHUT_RDWR);
#endif
    // The serving thread owns the listening descriptor and closes it after
    // accept() wakes. Keeping stop() to shutdown() avoids a double-close and
    // prevents a recycled descriptor from being closed by the old server.
  }
}

void HttpServer::serveConnection(std::uintptr_t rawSocket, std::string peerAddress) {
  const auto socket = static_cast<SocketType>(rawSocket);
  std::string input;
  std::array<char, 8192> buffer{};
  const auto sendAll = [socket](const std::string& value) {
    std::size_t sent = 0;
    while (sent < value.size()) {
      const auto written = ::send(socket, value.data() + sent, static_cast<int>(std::min<std::size_t>(value.size() - sent, static_cast<std::size_t>((std::numeric_limits<int>::max)()))), 0);
      if (written <= 0) return false;
      sent += static_cast<std::size_t>(written);
    }
    return true;
  };
  const auto sendError = [&](int status, const char* code, const char* message) {
    const std::string body = std::string(R"({"error":{"code":")") + code + R"(","message":")" + message + R"(","details":[],"trace_id":"server"}})";
    std::ostringstream response;
    response << "HTTP/1.1 " << status << " " << reason(status) << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n" << body;
    sendAll(response.str());
  };
  while (input.find("\r\n\r\n") == std::string::npos) {
    const auto received = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0) { closeSocket(socket); return; }
    input.append(buffer.data(), received);
    if (input.size() > 64ULL * 1024ULL) { sendError(413, "HEADERS_TOO_LARGE", "Request headers exceed the configured limit."); closeSocket(socket); return; }
  }
  const auto separator = input.find("\r\n\r\n");
  const auto bodyStart = separator + 4;
  const auto headerText = input.substr(0, separator);
  std::size_t expectedBody = 0;
  const auto lowerHeader = lower(headerText);
  const auto marker = lowerHeader.find("content-length:");
  if (marker != std::string::npos) {
    const auto lineEnd = headerText.find('\n', marker);
    auto value = headerText.substr(marker + 15, lineEnd == std::string::npos ? std::string::npos : lineEnd - marker - 15);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    const auto result = std::from_chars(value.data(), value.data() + value.size(), expectedBody);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) { sendError(400, "INVALID_CONTENT_LENGTH", "Content-Length must be a valid non-negative integer."); closeSocket(socket); return; }
  }
  if (lowerHeader.find("transfer-encoding:") != std::string::npos) { sendError(400, "UNSUPPORTED_TRANSFER_ENCODING", "Chunked request bodies are not supported; send Content-Length."); closeSocket(socket); return; }
  if (expectedBody > maxBodyBytes_) { sendError(413, "REQUEST_BODY_TOO_LARGE", "The request body exceeds the configured limit."); closeSocket(socket); return; }
  std::filesystem::path spoolPath;
  const auto cleanupSpool = [&] { if (!spoolPath.empty()) { std::error_code error; std::filesystem::remove(spoolPath, error); } };
  constexpr std::size_t spoolThreshold = 1ULL * 1024ULL * 1024ULL;
  const bool spool = expectedBody > spoolThreshold;
  std::ofstream spoolOutput;
  if (spool) {
    const auto directory = bodySpoolDirectory_.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(bodySpoolDirectory_);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    static std::atomic<std::uint64_t> nextSpoolId{0};
    spoolPath = directory / ("edgefleet-http-body-" + std::to_string(++nextSpoolId) + ".tmp");
    spoolOutput.open(spoolPath, std::ios::binary | std::ios::trunc);
    if (error || !spoolOutput) { sendError(507, "REQUEST_SPOOL_UNAVAILABLE", "The request body could not be spooled safely."); cleanupSpool(); closeSocket(socket); return; }
  }
  std::string body;
  if (!spool) body.reserve(expectedBody);
  std::size_t receivedBody = 0;
  const auto consume = [&](const char* bytes, std::size_t count) {
    if (count == 0) return;
    if (spool) spoolOutput.write(bytes, static_cast<std::streamsize>(count));
    else body.append(bytes, count);
    receivedBody += count;
  };
  const auto initialBody = (std::min)(expectedBody, input.size() - bodyStart);
  consume(input.data() + bodyStart, initialBody);
  while (receivedBody < expectedBody) {
    const auto received = ::recv(socket, buffer.data(), static_cast<int>(std::min<std::size_t>(buffer.size(), expectedBody - receivedBody)), 0);
    if (received <= 0) { cleanupSpool(); closeSocket(socket); return; }
    consume(buffer.data(), static_cast<std::size_t>(received));
  }
  if (spool) {
    spoolOutput.close();
    if (!spoolOutput) { cleanupSpool(); closeSocket(socket); return; }
  }
  HttpResponse response;
  try {
    auto request = parseRequest(input.substr(0, bodyStart) + (spool ? std::string{} : body));
    request.headers["x-peer-address"] = std::move(peerAddress);
    request.bodyFilePath = spoolPath.string();
    request.bodySize = expectedBody;
    response = handler_(request);
  } catch (const std::exception&) {
    response = {500, "application/json", R"({"error":{"code":"INTERNAL_ERROR","message":"Request failed safely.","trace_id":"server"}})", {}, {}, 0, 0};
  }
  cleanupSpool();
  response.headers["Content-Type"] = response.contentType;
  std::uintmax_t responseLength = response.body.size();
  if (!response.bodyFilePath.empty()) {
    std::error_code error;
    const auto size = std::filesystem::file_size(response.bodyFilePath, error);
    if (error || response.bodyFileOffset > size) { closeSocket(socket); return; }
    responseLength = response.bodyFileLength == 0 ? size - response.bodyFileOffset : response.bodyFileLength;
    if (response.bodyFileOffset + responseLength > size) { closeSocket(socket); return; }
  }
  response.headers["Content-Length"] = std::to_string(responseLength);
  response.headers["Connection"] = "close";
  response.headers["X-Content-Type-Options"] = "nosniff";
  response.headers["Content-Security-Policy"] = "default-src 'self'; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'";
  response.headers["Referrer-Policy"] = "no-referrer";
  std::ostringstream output;
  output << "HTTP/1.1 " << response.status << " " << reason(response.status) << "\r\n";
  for (const auto& [key, value] : response.headers) output << key << ": " << value << "\r\n";
  output << "\r\n";
  if (!sendAll(output.str())) { closeSocket(socket); return; }
  if (response.bodyFilePath.empty()) {
    sendAll(response.body);
  } else {
    std::ifstream file(response.bodyFilePath, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(response.bodyFileOffset));
    std::uintmax_t remaining = responseLength;
    while (file && remaining > 0) {
      const auto chunk = static_cast<std::streamsize>(std::min<std::uintmax_t>(buffer.size(), remaining));
      file.read(buffer.data(), chunk);
      const auto count = file.gcount();
      if (count <= 0 || !sendAll(std::string(buffer.data(), static_cast<std::size_t>(count)))) break;
      remaining -= static_cast<std::uintmax_t>(count);
    }
  }
  closeSocket(socket);
}

}  // namespace edgefleet::web
