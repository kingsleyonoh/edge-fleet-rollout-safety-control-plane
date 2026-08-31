#include <filesystem>
#include <fstream>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <sqlite3.h>

#include "application/control_plane.hpp"
#include "application/interoperability.hpp"
#include "domain/safety.hpp"
#include "domain/artifact.hpp"
#include "domain/benchmark.hpp"
#include "application/jobs.hpp"
#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

namespace {

void printUsage() {
  std::cout << "edgefleet <setup|serve|worker|migrate|simulate|benchmark|artifact-key|backup|evidence|recovery-check|replay-recovery|fixture-validate|fake-device>\n"
               "  setup                 initialize SQLite and create the default tenant\n"
               "  serve                 run the HTTP control plane\n"
               "  worker                run one maintenance worker pass\n"
               "  migrate               apply the selected storage migrations and exit\n"
               "  simulate              run a deterministic scenario from --scenario or stdin JSON\n"
               "  benchmark             run the frozen v1 benchmark corpus\n"
               "  artifact-key          generate an Ed25519 signing key pair\n"
               "  backup --out PATH    create a consistent SQLite backup\n"
               "  evidence --verify     verify every tenant evidence chain\n"
               "  recovery-check        print the latest completed decision digest per tenant\n"
               "  replay-recovery       verify evidence and resume queued replay jobs\n"
               "  fixture-validate      validate --fleet-csv or --evidence-ndjson fixtures\n"
               "  fake-device           send one signed observation to a local HTTP server\n";
}

bool backupSqlite(const std::string& sourcePath, const std::string& destinationPath) {
  sqlite3* source = nullptr;
  sqlite3* destination = nullptr;
  if (sqlite3_open_v2(sourcePath.c_str(), &source, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (source != nullptr) sqlite3_close(source);
    return false;
  }
  if (sqlite3_open_v2(destinationPath.c_str(), &destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    sqlite3_close(source);
    if (destination != nullptr) sqlite3_close(destination);
    return false;
  }
  auto* backup = sqlite3_backup_init(destination, "main", source, "main");
  const auto result = backup == nullptr ? SQLITE_ERROR : sqlite3_backup_step(backup, -1);
  if (backup != nullptr) sqlite3_backup_finish(backup);
  const bool ok = result == SQLITE_DONE && sqlite3_errcode(destination) == SQLITE_OK;
  sqlite3_close(destination);
  sqlite3_close(source);
  return ok;
}

edgefleet::application::ControlPlane makeApp() {
  auto config = edgefleet::shared::Config::load();
  return edgefleet::application::ControlPlane(std::move(config));
}

struct DeviceUrl {
  std::string host;
  std::string port;
  std::string path;
};

bool parseDeviceUrl(const std::string& value, DeviceUrl& result) {
  constexpr std::string_view prefix = "http://";
  if (!value.starts_with(prefix)) return false;
  const auto pathStart = value.find('/', prefix.size());
  const auto authority = value.substr(prefix.size(), pathStart == std::string::npos ? std::string::npos : pathStart - prefix.size());
  if (authority.empty() || authority.find('@') != std::string::npos || authority.find('[') != std::string::npos || authority.find(']') != std::string::npos) return false;
  const auto portStart = authority.rfind(':');
  if (portStart != std::string::npos) {
    if (authority.find(':') != portStart) return false;
    result.host = authority.substr(0, portStart);
    result.port = authority.substr(portStart + 1);
  } else {
    result.host = authority;
    result.port = "8080";
  }
  result.path = pathStart == std::string::npos ? "/api/agent/v1/reports" : value.substr(pathStart);
  if (result.host.empty() || result.port.empty() || result.path != "/api/agent/v1/reports") return false;
  try {
    const auto port = std::stoul(result.port);
    if (port == 0 || port > 65535) return false;
  } catch (const std::exception&) { return false; }
  return true;
}

#ifdef _WIN32
using DeviceSocket = SOCKET;
constexpr DeviceSocket invalidDeviceSocket = INVALID_SOCKET;
#else
using DeviceSocket = int;
constexpr DeviceSocket invalidDeviceSocket = -1;
#endif

void closeDeviceSocket(DeviceSocket socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

bool sendDeviceBytes(DeviceSocket socket, const std::string& bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const auto count = ::send(socket, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
    if (count <= 0) return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

int fakeDeviceReport(const std::string& url, const std::string& deviceId, const std::string& secret, int keyVersion, std::int64_t sequence,
                     std::int64_t generation, const std::string& digest, const std::string& releaseId, const std::string& commandId, const std::string& reportType) {
  DeviceUrl target;
  if (!parseDeviceUrl(url, target) || deviceId.empty() || secret.empty() || keyVersion < 1 || sequence < 1 || generation < 0) {
    std::cerr << "fake-device requires a local http:// URL, device credentials, and positive key/sequence values\n";
    return 2;
  }
  edgefleet::shared::Json report{{"device_id", deviceId}, {"report_id", edgefleet::shared::Uuid::generate().str()}, {"report_sequence", sequence},
                                 {"report_type", reportType}, {"observed_generation", generation}, {"health", edgefleet::shared::Json::object()}};
  if (!digest.empty()) report["observed_artifact_digest"] = digest;
  if (!releaseId.empty()) report["release_id"] = releaseId;
  if (!commandId.empty()) report["command_id"] = commandId;
  const auto body = edgefleet::shared::CanonicalJson::serialize(report);
  const auto signature = edgefleet::shared::DigestService::hmacSha256Hex(secret, "POST " + target.path + " " + std::to_string(sequence) + " " + edgefleet::shared::DigestService::sha256Hex(body));
#ifdef _WIN32
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::cerr << "could not initialize sockets\n"; return 1; }
#endif
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  if (getaddrinfo(target.host.c_str(), target.port.c_str(), &hints, &addresses) != 0) {
#ifdef _WIN32
    WSACleanup();
#endif
    std::cerr << "could not resolve the control-plane host\n";
    return 1;
  }
  DeviceSocket socket = invalidDeviceSocket;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    socket = static_cast<DeviceSocket>(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (socket == invalidDeviceSocket) continue;
    if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) break;
    closeDeviceSocket(socket);
    socket = invalidDeviceSocket;
  }
  freeaddrinfo(addresses);
  if (socket == invalidDeviceSocket) {
#ifdef _WIN32
    WSACleanup();
#endif
    std::cerr << "could not connect to the control plane\n";
    return 1;
  }
  const auto request = "POST " + target.path + " HTTP/1.1\r\nHost: " + target.host + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\nX-Device-Id: " + deviceId + "\r\nX-Device-Secret: " + secret + "\r\nX-Device-Key-Version: " + std::to_string(keyVersion) +
                       "\r\nX-Device-Sequence: " + std::to_string(sequence) + "\r\nX-Device-Signature: " + signature + "\r\n\r\n" + body;
  if (!sendDeviceBytes(socket, request)) {
    closeDeviceSocket(socket);
#ifdef _WIN32
    WSACleanup();
#endif
    std::cerr << "could not send the device report\n";
    return 1;
  }
  std::string response;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto count = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (count <= 0) break;
    response.append(buffer.data(), static_cast<std::size_t>(count));
    if (response.size() > 2ULL * 1024ULL * 1024ULL) break;
  }
  closeDeviceSocket(socket);
#ifdef _WIN32
  WSACleanup();
#endif
  const auto lineEnd = response.find("\r\n");
  int status = 1;
  if (lineEnd != std::string::npos) {
    try {
      const auto firstSpace = response.find(' ');
      status = std::stoi(response.substr(firstSpace + 1, 3));
    } catch (const std::exception&) { status = 1; }
  }
  const auto bodyStart = response.find("\r\n\r\n");
  std::cout << (bodyStart == std::string::npos ? response : response.substr(bodyStart + 4)) << "\n";
  return status >= 200 && status < 300 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { printUsage(); return 2; }
  const std::string command = argv[1];
  if (command == "artifact-key") {
    const auto key = edgefleet::domain::ArtifactSigner::generateKeyPair();
    if (!key.ok()) { std::cerr << key.error->message << "\n"; return 1; }
    std::string outputPath;
    for (int index = 2; index + 1 < argc; ++index) if (std::string(argv[index]) == "--out") outputPath = argv[index + 1];
    if (!outputPath.empty()) {
      const auto path = std::filesystem::path(outputPath);
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      std::ofstream output(path, std::ios::binary);
      output << key.value->privateKeyPem;
      output.close();
      if (error || !output) { std::cerr << "could not write the signing key\n"; return 1; }
      std::cout << edgefleet::shared::CanonicalJson::serialize({{"algorithm", "ed25519"}, {"fingerprint_sha256", key.value->fingerprintSha256}, {"public_key_pem", key.value->publicKeyPem}, {"private_key_path", path.string()}}) << "\n";
    } else {
      std::cout << edgefleet::shared::CanonicalJson::serialize({{"algorithm", "ed25519"}, {"fingerprint_sha256", key.value->fingerprintSha256}, {"public_key_pem", key.value->publicKeyPem}, {"private_key_pem", key.value->privateKeyPem}}) << "\n";
    }
    return 0;
  }
  if (command == "fixture-validate") {
    std::string fleetPath;
    std::string evidencePath;
    for (int index = 2; index + 1 < argc; ++index) {
      if (std::string(argv[index]) == "--fleet-csv") fleetPath = argv[index + 1];
      if (std::string(argv[index]) == "--evidence-ndjson") evidencePath = argv[index + 1];
    }
    if (fleetPath.empty() == evidencePath.empty()) { std::cerr << "fixture-validate requires exactly one fixture path\n"; return 2; }
    std::ifstream input(fleetPath.empty() ? evidencePath : fleetPath, std::ios::binary);
    if (!input.is_open()) { std::cerr << "fixture could not be opened\n"; return 1; }
    const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!fleetPath.empty()) {
      const auto result = edgefleet::application::Interoperability::parseFleetCsv(content);
      if (!result.ok()) { std::cerr << result.error->message << "\n"; return 1; }
      std::cout << edgefleet::shared::CanonicalJson::serialize({{"status", "valid"}, {"fixture", "fleet.csv"}, {"row_count", result.value->size()}}) << "\n";
    } else {
      const auto result = edgefleet::application::Interoperability::parseEvidenceNdjson(content);
      if (!result.ok()) { std::cerr << result.error->message << "\n"; return 1; }
      std::cout << edgefleet::shared::CanonicalJson::serialize({{"status", "valid"}, {"fixture", "evidence.ndjson"}, {"event_count", result.value->size()}}) << "\n";
    }
    return 0;
  }
  if (command == "fake-device") {
    std::string url;
    std::string deviceId;
    std::string secret;
    std::string digest;
    std::string releaseId;
    std::string commandId;
    std::string reportType = "observation";
    int keyVersion = 1;
    std::int64_t sequence = 1;
    std::int64_t generation = 0;
    for (int index = 2; index + 1 < argc; ++index) {
      const auto option = std::string(argv[index]);
      if (option == "--url") url = argv[index + 1];
      else if (option == "--device-id") deviceId = argv[index + 1];
      else if (option == "--secret") secret = argv[index + 1];
      else if (option == "--key-version") keyVersion = std::stoi(argv[index + 1]);
      else if (option == "--sequence") sequence = std::stoll(argv[index + 1]);
      else if (option == "--generation") generation = std::stoll(argv[index + 1]);
      else if (option == "--digest") digest = argv[index + 1];
      else if (option == "--release-id") releaseId = argv[index + 1];
      else if (option == "--command-id") commandId = argv[index + 1];
      else if (option == "--report-type") reportType = argv[index + 1];
    }
    try { return fakeDeviceReport(url, deviceId, secret, keyVersion, sequence, generation, digest, releaseId, commandId, reportType); }
    catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 2; }
  }
  auto app = makeApp();
  const auto root = std::filesystem::path(argv[0]).parent_path().parent_path().parent_path();
  const auto migrationDirectory = app.config().storageBackend == "postgres" ? "postgres" : "sqlite";
  const auto migrationPath = std::filesystem::exists(root / "migrations" / migrationDirectory) ? root / "migrations" / migrationDirectory : std::filesystem::path("migrations") / migrationDirectory;
  if (!app.initialize(migrationPath.string())) { std::cerr << "initialization failed: " << app.storage()->lastError() << "\n"; return 1; }

  if (command == "setup") {
    edgefleet::web::HttpRequest request{"POST", "/api/setup", {}, "{\"name\":\"" + app.config().defaultTenantName + "\"}"};
    const auto response = app.handle(request);
    std::cout << response.body << "\n";
    return response.status >= 400 ? 1 : 0;
  }
  if (command == "serve") {
    edgefleet::web::HttpServer server(app.config().host, app.config().port, [&](const auto& request) {
      if (request.target == "/metrics" || request.target.starts_with("/metrics?")) return edgefleet::web::HttpResponse{404, "application/json", R"({"error":{"code":"NOT_FOUND","message":"Metrics are served on the configured metrics listener.","details":[],"trace_id":"server"}})", {}};
      return app.handle(request);
    }, app.config().artifactTempPath, static_cast<std::size_t>(app.config().artifactMaxBytes));
    edgefleet::web::HttpServer metricsServer(app.config().metricsHost, app.config().metricsPort, [&](const auto& request) {
      const auto path = request.target.substr(0, request.target.find('?'));
      if (request.method != "GET" || path != "/metrics") return edgefleet::web::HttpResponse{404, "application/json", R"({"error":{"code":"NOT_FOUND","message":"Only GET /metrics is available on this listener.","details":[],"trace_id":"metrics"}})", {}};
      return app.handle(request);
    }, app.config().artifactTempPath, 1024 * 1024);
    std::thread metricsThread([&] { metricsServer.start(); });
    for (int attempt = 0; attempt < 200 && metricsServer.boundPort() == 0; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (metricsServer.boundPort() == 0) {
      metricsServer.stop();
      if (metricsThread.joinable()) metricsThread.join();
      std::cerr << "metrics server failed to start\n";
      return 1;
    }
    std::atomic<bool> workerStop{false};
    std::thread worker;
    if (app.config().workerEnabled) {
      worker = std::thread([&] {
        while (!workerStop.load()) {
          const auto tenants = app.storage()->query("SELECT id FROM tenants WHERE is_active=1");
          for (const auto& tenant : tenants) edgefleet::application::WorkerCoordinator::run(*app.storage(), tenant.at("id").get<std::string>(), app.config().traceStorePath, app.config().exportStorePath, app.config().artifactTempPath, "serve-worker");
          for (int tick = 0; tick < 50 && !workerStop.load(); ++tick) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      });
    }
    const bool started = server.start();
    metricsServer.stop();
    if (metricsThread.joinable()) metricsThread.join();
    workerStop = true;
    if (worker.joinable()) worker.join();
    if (!started) { std::cerr << "server failed to start\n"; return 1; }
    return 0;
  }
  if (command == "worker") {
    const auto tenants = app.storage()->query("SELECT id FROM tenants WHERE is_active=1");
    int processed = 0;
    for (const auto& tenant : tenants) {
      edgefleet::application::WorkerCoordinator::run(*app.storage(), tenant.at("id").get<std::string>(), app.config().traceStorePath, app.config().exportStorePath, app.config().artifactTempPath, "cli-worker");
      ++processed;
    }
    std::cout << edgefleet::shared::CanonicalJson::serialize({{"status", "ok"}, {"tenants_processed", processed}}) << "\n";
    return 0;
  }
  if (command == "migrate") { std::cout << edgefleet::shared::CanonicalJson::serialize({{"status", "ok"}, {"schema_version", app.storage()->schemaVersion()}}) << "\n"; return 0; }
  if (command == "simulate") {
    std::string input;
    std::uint64_t seed = 1;
    for (int index = 2; index < argc; ++index) {
      if (std::string(argv[index]) == "--scenario" && index + 1 < argc) { std::ifstream file(argv[++index]); input.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()); }
      if (std::string(argv[index]) == "--seed" && index + 1 < argc) { try { seed = std::stoull(argv[++index]); } catch (const std::exception&) { std::cerr << "invalid seed\n"; return 2; } }
    }
    if (input.empty()) input.assign((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    try {
      const auto parsed = input.empty() ? edgefleet::shared::Json{{"device_count", 100}} : edgefleet::shared::Json::parse(input);
      const auto result = edgefleet::domain::Simulator::run(parsed, seed);
      if (!result.ok()) { std::cerr << result.error->message << "\n"; return 1; }
      std::cout << edgefleet::shared::CanonicalJson::serialize({{"metrics", result.value->metrics}, {"result_digest", result.value->resultDigest}, {"trace_digest", result.value->traceDigest}}) << "\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
  }
  if (command == "benchmark") {
    std::string corpus = app.config().benchmarkCorpusPath;
    if (argc > 3 && std::string(argv[2]) == "--corpus") corpus = argv[3];
    edgefleet::shared::Json manifest;
    try {
      if (!std::filesystem::exists(corpus)) { std::cerr << "benchmark manifest could not be opened\n"; return 1; }
      std::ifstream file(corpus);
      manifest = edgefleet::shared::Json::parse(file);
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
    const auto report = edgefleet::domain::BenchmarkRunner::runManifest(manifest);
    if (!report.ok()) { std::cerr << report.error->message << "\n"; return 1; }
    edgefleet::shared::Json cells = edgefleet::shared::Json::array();
    for (const auto& cell : report.value->cells) cells.push_back({{"scenario", cell.scenario}, {"seed", cell.seed}, {"strategy", cell.strategy}, {"metrics", cell.metrics}, {"digest", cell.digest}});
    std::cout << edgefleet::shared::CanonicalJson::serialize({{"corpus_version", report.value->corpusVersion}, {"cell_count", report.value->cells.size()}, {"result_digest", report.value->digest}, {"cells", cells}}) << "\n";
    return 0;
  }
  if (command == "backup") {
    if (app.config().storageBackend != "sqlite") { std::cerr << "SQLite backup is unavailable for the PostgreSQL backend; use a PostgreSQL-native backup.\n"; return 1; }
    std::string outputPath;
    for (int index = 2; index + 1 < argc; ++index) if (std::string(argv[index]) == "--out") outputPath = argv[index + 1];
    if (outputPath.empty()) { std::cerr << "backup requires --out PATH\n"; return 2; }
    const auto path = std::filesystem::path(outputPath);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error || !backupSqlite(app.config().sqlitePath, path.string())) { std::cerr << "SQLite backup failed\n"; return 1; }
    std::cout << edgefleet::shared::CanonicalJson::serialize({{"status", "ok"}, {"path", path.string()}}) << "\n";
    return 0;
  }
  if (command == "evidence" && argc > 2 && (std::string(argv[2]) == "--verify" || std::string(argv[2]) == "verify")) {
    const auto rows = app.storage()->query("SELECT id FROM tenants WHERE is_active=1");
    bool valid = true;
    for (const auto& row : rows) { const auto result = app.storage()->verifyEvidence(row.at("id")); std::cout << result.dump() << "\n"; valid = valid && result.at("valid").get<bool>(); }
    return valid ? 0 : 1;
  }
  if (command == "recovery-check") {
    bool requireCompleted = false;
    for (int index = 2; index < argc; ++index) if (std::string(argv[index]) == "--require-completed") requireCompleted = true;
    const auto tenants = app.storage()->query("SELECT id FROM tenants WHERE is_active=1 ORDER BY id");
    edgefleet::shared::Json tenantResults = edgefleet::shared::Json::array();
    bool valid = true;
    for (const auto& tenant : tenants) {
      const auto tenantId = tenant.at("id").get<std::string>();
      const auto evidence = app.storage()->verifyEvidence(tenantId);
      const auto releases = app.storage()->query("SELECT id,name,status,version,membership_digest,ended_at FROM releases WHERE tenant_id=? AND status='completed' ORDER BY ended_at DESC,created_at DESC,id DESC LIMIT 1", {tenantId});
      edgefleet::shared::Json result{{"tenant_id", tenantId}, {"evidence", evidence}, {"latest_completed_release", nullptr}};
      if (!releases.empty()) {
        const auto& release = releases.front();
        const auto evaluations = app.storage()->query("SELECT stage_id,decision,evidence_digest,evaluated_at FROM health_gate_evaluations WHERE tenant_id=? AND release_id=? ORDER BY evaluated_at DESC,id DESC LIMIT 1", {tenantId, release.at("id").get<std::string>()});
        const auto gateDecision = evaluations.empty() ? std::string() : evaluations.front().value("decision", "");
        const auto gateEvidence = evaluations.empty() ? std::string() : evaluations.front().value("evidence_digest", "");
        const auto membershipDigest = release.at("membership_digest").is_string() ? release.at("membership_digest").get<std::string>() : std::string();
        const auto decisionInput = edgefleet::shared::Json{{"release_id", release.at("id")}, {"status", release.at("status")}, {"version", release.at("version")},
                                                           {"membership_digest", membershipDigest}, {"gate_decision", gateDecision}, {"gate_evidence_digest", gateEvidence}};
        result["latest_completed_release"] = { {"id", release.at("id")}, {"name", release.at("name")}, {"status", release.at("status")},
                                                 {"version", release.at("version")}, {"ended_at", release.at("ended_at")}, {"decision_digest", edgefleet::shared::DigestService::sha256Hex(edgefleet::shared::CanonicalJson::serialize(decisionInput))},
                                                 {"gate_decision", gateDecision}, {"gate_evidence_digest", gateEvidence} };
      } else if (requireCompleted) {
        valid = false;
      }
      valid = valid && evidence.value("valid", false);
      tenantResults.push_back(result);
    }
    std::cout << edgefleet::shared::CanonicalJson::serialize({{"status", valid ? "ok" : "failed"}, {"tenants", tenantResults}}) << "\n";
    return valid ? 0 : 1;
  }
  if (command == "replay-recovery") {
    const auto tenants = app.storage()->query("SELECT id FROM tenants WHERE is_active=1");
    int processed = 0;
    bool valid = true;
    edgefleet::shared::Json replayStatuses = edgefleet::shared::Json::array();
    for (const auto& tenant : tenants) {
      const auto tenantId = tenant.at("id").get<std::string>();
      const auto before = app.storage()->verifyEvidence(tenantId);
      if (!before.value("valid", false)) { valid = false; continue; }
      edgefleet::application::WorkerCoordinator::run(*app.storage(), tenantId, app.config().traceStorePath, app.config().exportStorePath, app.config().artifactTempPath, "cli-replay-recovery");
      const auto rows = app.storage()->query("SELECT id,status,actual_decision_digest,divergence_json FROM replay_runs WHERE tenant_id=? ORDER BY created_at", {tenantId});
      for (const auto& row : rows) replayStatuses.push_back({{"id", row.value("id", "")}, {"status", row.value("status", "")}, {"actual_decision_digest", row.value("actual_decision_digest", "")}, {"divergence_json", row.value("divergence_json", "{}")}});
      const auto after = app.storage()->verifyEvidence(tenantId);
      valid = valid && after.value("valid", false);
      ++processed;
    }
    std::cout << edgefleet::shared::CanonicalJson::serialize({{"status", valid ? "ok" : "failed"}, {"tenants_processed", processed}, {"replays", replayStatuses}}) << "\n";
    return valid ? 0 : 1;
  }
  printUsage();
  return 2;
}
