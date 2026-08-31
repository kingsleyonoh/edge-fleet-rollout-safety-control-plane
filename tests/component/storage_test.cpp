#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <vector>

#include "infrastructure/sqlite_storage.hpp"
#include "shared/types.hpp"

#if defined(EDGEFLEET_HAS_POSTGRES)
#include "infrastructure/postgres_storage.hpp"
#endif

TEST_CASE("SQLite migrations create the tenant-scoped operational schema", "[component]") {
  const auto dbPath = std::filesystem::temp_directory_path() / "edgefleet-storage-test.db";
  std::filesystem::remove(dbPath);
  {
    edgefleet::infrastructure::SqliteStorage storage(dbPath.string());
    REQUIRE(storage.open());
    REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
    REQUIRE(storage.schemaVersion() >= 1);

    const auto tenantA = storage.createTenant("Tenant A", "A Legal", "A", "UTC", "prefix-a", "hash-a");
    const auto tenantB = storage.createTenant("Tenant B", "B Legal", "B", "UTC", "prefix-b", "hash-b");
    REQUIRE(tenantA.has_value());
    REQUIRE(tenantB.has_value());
    REQUIRE(storage.createFleet(tenantA->at("id"), "fleet-a", "Fleet A", "production").has_value());
    REQUIRE(storage.listFleets(tenantA->at("id")).size() == 1);
    REQUIRE(storage.listFleets(tenantB->at("id")).empty());
  }
  std::filesystem::remove(dbPath);
}

TEST_CASE("SQLite schema enforces tenant relationships and immutable fact tables", "[component][security]") {
  const auto dbPath = std::filesystem::temp_directory_path() / ("edgefleet-schema-contract-" + edgefleet::shared::Uuid::generate().str() + ".db");
  {
    edgefleet::infrastructure::SqliteStorage storage(dbPath.string());
    REQUIRE(storage.open());
    REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));

    const std::vector<std::string> tables{
        "tenants", "operator_credentials", "fleets", "devices", "device_credentials", "artifact_signing_keys", "artifacts",
        "rollout_policies", "releases", "release_memberships", "release_stages", "release_assignments", "rollout_commands",
        "device_reports", "health_samples", "health_gate_evaluations", "approval_requests", "simulation_runs", "replay_runs",
        "benchmark_runs", "benchmark_results", "evidence_events", "evidence_checkpoints", "evidence_exports", "operator_notices",
        "integration_configs", "outbox_deliveries", "idempotency_records", "job_leases", "browser_sessions"};
    const auto actual = storage.query("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    for (const auto& table : tables) REQUIRE(std::any_of(actual.begin(), actual.end(), [&](const auto& row) { return row.value("name", "") == table; }));

    const auto assignmentForeignKeys = storage.query("PRAGMA foreign_key_list(release_assignments)");
    REQUIRE(std::any_of(assignmentForeignKeys.begin(), assignmentForeignKeys.end(), [](const auto& row) { return row.value("table", "") == "releases"; }));
    REQUIRE(std::any_of(assignmentForeignKeys.begin(), assignmentForeignKeys.end(), [](const auto& row) { return row.value("table", "") == "devices"; }));
    const auto triggers = storage.query("SELECT name FROM sqlite_master WHERE type='trigger'");
    REQUIRE(std::any_of(triggers.begin(), triggers.end(), [](const auto& row) { return row.value("name", "") == "prevent_evidence_event_update"; }));
    REQUIRE(std::any_of(triggers.begin(), triggers.end(), [](const auto& row) { return row.value("name", "") == "prevent_rollout_command_delete"; }));

    const auto tenantA = storage.createTenant("Tenant A", "A Legal", "A", "UTC", "schema-a", "hash-a");
    const auto tenantB = storage.createTenant("Tenant B", "B Legal", "B", "UTC", "schema-b", "hash-b");
    REQUIRE(tenantA.has_value());
    REQUIRE(tenantB.has_value());
    const auto fleetB = storage.createFleet(tenantB->at("id"), "fleet-b", "Fleet B", "production");
    REQUIRE(fleetB.has_value());
    REQUIRE_FALSE(storage.createDevice(tenantA->at("id"), fleetB->at("id"), {{"stable_key", "cross-tenant"}}, "hash").has_value());

    const auto event = storage.appendEvidence(tenantA->at("id"), "schema.test", "tenant", tenantA->at("id"), {{"immutable", true}});
    REQUIRE(event.has_value());
    REQUIRE_FALSE(storage.execute("UPDATE evidence_events SET event_type='tampered' WHERE tenant_id=? AND id=?", {tenantA->at("id"), event->at("id").get<std::string>()}));
    REQUIRE_FALSE(storage.execute("DELETE FROM evidence_events WHERE tenant_id=? AND id=?", {tenantA->at("id"), event->at("id").get<std::string>()}));
  }
  std::filesystem::remove(dbPath);
}

TEST_CASE("evidence verification detects the first broken event", "[component]") {
  const auto dbPath = std::filesystem::temp_directory_path() / "edgefleet-evidence-test.db";
  std::filesystem::remove(dbPath);
  {
    edgefleet::infrastructure::SqliteStorage storage(dbPath.string());
    REQUIRE(storage.open());
    REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
    const auto tenant = storage.createTenant("Evidence Tenant", "Evidence Ltd", "Evidence", "UTC", "evidence-prefix", "evidence-hash");
    REQUIRE(tenant.has_value());
    REQUIRE(storage.appendEvidence(tenant->at("id"), "test", "aggregate", "event", nlohmann::json{{"safe", true}}).has_value());
    REQUIRE(storage.verifyEvidence(tenant->at("id")).at("valid").get<bool>());
    REQUIRE(storage.corruptFirstEvidenceForTest(tenant->at("id")));
    const auto result = storage.verifyEvidence(tenant->at("id"));
    REQUIRE_FALSE(result.at("valid").get<bool>());
    REQUIRE(result.at("first_broken_sequence").get<int>() == 1);
  }
  std::filesystem::remove(dbPath);
}

TEST_CASE("storage transactions roll back business rows and evidence together", "[component]") {
  const auto dbPath = std::filesystem::temp_directory_path() / ("edgefleet-transaction-test-" + edgefleet::shared::Uuid::generate().str() + ".db");
  {
    edgefleet::infrastructure::SqliteStorage storage(dbPath.string());
    REQUIRE(storage.open());
    REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "sqlite").string()));
    const auto tenant = storage.createTenant("Transaction", "Transaction Ltd", "Transaction", "UTC", "transaction-prefix", "transaction-hash");
    REQUIRE(tenant.has_value());
    const auto tenantId = tenant->at("id").get<std::string>();
    REQUIRE_FALSE(storage.transaction([&] {
      REQUIRE(storage.createFleet(tenantId, "rollback", "Rollback", "development").has_value());
      REQUIRE(storage.appendEvidence(tenantId, "transaction.test", "tenant", tenantId, nlohmann::json{{"committed", false}}).has_value());
      return false;
    }));
    REQUIRE(storage.listFleets(tenantId).empty());
    REQUIRE(storage.query("SELECT id FROM evidence_events WHERE tenant_id=?", {tenantId}).empty());
  }
  std::filesystem::remove(dbPath);
}

#if defined(EDGEFLEET_HAS_POSTGRES)
TEST_CASE("PostgreSQL storage satisfies the shared tenant and evidence contract", "[component][postgres][security]") {
  const auto* connectionString = std::getenv("EDGEFLEET_TEST_DATABASE_URL");
  if (connectionString == nullptr || std::string(connectionString).empty()) SKIP("EDGEFLEET_TEST_DATABASE_URL is not configured");
  edgefleet::infrastructure::PostgresStorage storage(connectionString);
  REQUIRE(storage.open());
  REQUIRE(storage.migrate((std::filesystem::path(EDGEFLEET_SOURCE_DIR) / "migrations" / "postgres").string()));
  const auto suffix = edgefleet::shared::Uuid::generate().str().substr(0, 8);
  const auto tenantA = storage.createTenant("Postgres Tenant A", "A Legal", "A", "UTC", "pg-prefix-a-" + suffix, "hash-a");
  const auto tenantB = storage.createTenant("Postgres Tenant B", "B Legal", "B", "UTC", "pg-prefix-b-" + suffix, "hash-b");
  REQUIRE(tenantA.has_value());
  REQUIRE(tenantB.has_value());
  const auto fleetA = storage.createFleet(tenantA->at("id"), "pg-fleet-a", "Fleet A", "production");
  const auto fleetB = storage.createFleet(tenantB->at("id"), "pg-fleet-b", "Fleet B", "production");
  REQUIRE(fleetA.has_value());
  REQUIRE(fleetB.has_value());
  REQUIRE(storage.createDevice(tenantA->at("id"), fleetB->at("id"), {{"stable_key", "cross-tenant"}}, "hash").has_value() == false);
  REQUIRE(storage.createDevice(tenantA->at("id"), fleetA->at("id"), {{"stable_key", "device-a"}}, "hash").has_value());
  REQUIRE(storage.listFleets(tenantA->at("id")).size() == 1);
  const auto event = storage.appendEvidence(tenantA->at("id"), "postgres.test", "tenant", tenantA->at("id"), {{"immutable", true}});
  REQUIRE(event.has_value());
  REQUIRE(storage.verifyEvidence(tenantA->at("id")).at("valid").get<bool>());
  REQUIRE_FALSE(storage.execute("UPDATE evidence_events SET event_type='tampered' WHERE tenant_id=? AND id=?", {tenantA->at("id"), event->at("id").get<std::string>()}));
  REQUIRE(storage.verifyEvidence(tenantA->at("id")).at("valid").get<bool>());
}
#endif
