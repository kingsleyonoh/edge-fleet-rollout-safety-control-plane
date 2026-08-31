#include "web/policy_registry.hpp"

namespace edgefleet::web {

const std::vector<RoutePermission>& routePermissions() {
  static const std::vector<RoutePermission> routes{
      {"GET", "/api/tenants/me", "viewer", "tenant", "read"}, {"PATCH", "/api/tenants/me", "admin", "tenant", "write"},
      {"GET", "/api/credentials", "admin", "tenant", "write"}, {"POST", "/api/credentials", "admin", "tenant", "write"},
      {"POST", "/api/credentials/{id}/revoke", "admin", "tenant", "write"}, {"POST", "/api/credentials/{id}/rotate", "admin", "tenant", "write"},
      {"GET", "/api/fleets", "viewer", "fleets", "read"}, {"POST", "/api/fleets", "release_manager", "fleets", "write"},
      {"GET", "/api/fleets/{id}", "viewer", "fleets", "read"}, {"PATCH", "/api/fleets/{id}", "release_manager", "fleets", "write"},
      {"POST", "/api/fleets/{id}/pause", "admin", "tenant", "write"}, {"POST", "/api/fleets/{id}/retire", "admin", "tenant", "write"},
      {"GET", "/api/fleets/{id}/devices", "viewer", "devices", "read"}, {"POST", "/api/fleets/{id}/devices", "release_manager", "devices", "write"},
      {"GET", "/api/devices", "viewer", "devices", "read"}, {"GET", "/api/devices/{id}", "viewer", "devices", "read"},
      {"PATCH", "/api/devices/{id}", "release_manager", "devices", "write"}, {"POST", "/api/devices/{id}/quarantine", "release_manager", "devices", "write"},
      {"POST", "/api/devices/{id}/reactivate", "release_manager", "devices", "write"}, {"POST", "/api/devices/{id}/decommission", "admin", "devices", "write"},
      {"POST", "/api/devices/{id}/credentials/rotate", "admin", "devices", "write"},
      {"GET", "/api/artifact-signing-keys", "viewer", "artifacts", "read"}, {"POST", "/api/artifact-signing-keys", "admin", "artifacts", "write"},
      {"POST", "/api/artifact-signing-keys/{id}/retire", "admin", "artifacts", "write"}, {"POST", "/api/artifact-signing-keys/{id}/compromise", "admin", "artifacts", "write"},
      {"GET", "/api/artifacts", "viewer", "artifacts", "read"}, {"POST", "/api/artifacts", "release_manager", "artifacts", "write"},
      {"GET", "/api/artifacts/{id}", "viewer", "artifacts", "read"}, {"POST", "/api/artifacts/{id}/validate", "release_manager", "artifacts", "write"},
      {"POST", "/api/artifacts/{id}/retire", "admin", "artifacts", "write"}, {"GET", "/api/artifacts/{id}/download", "viewer", "artifacts", "read"},
      {"GET", "/api/policies", "viewer", "policies", "read"}, {"POST", "/api/policies", "release_manager", "policies", "write"},
      {"GET", "/api/policies/{id}", "viewer", "policies", "read"}, {"PATCH", "/api/policies/{id}", "release_manager", "policies", "write"},
      {"POST", "/api/policies/{id}/validate", "release_manager", "policies", "write"}, {"POST", "/api/policies/{id}/activate", "admin", "policies", "write"}, {"POST", "/api/policies/{id}/archive", "admin", "policies", "write"},
      {"GET", "/api/releases", "viewer", "release_drafts", "read"}, {"POST", "/api/releases", "release_manager", "release_drafts", "write"},
      {"GET", "/api/releases/{id}", "viewer", "release_drafts", "read"}, {"PATCH", "/api/releases/{id}", "release_manager", "release_drafts", "write"},
      {"POST", "/api/releases/{id}/validate", "release_manager", "release_drafts", "write"}, {"GET", "/api/releases/{id}/membership", "viewer", "release_drafts", "read"},
      {"POST", "/api/releases/{id}/submit", "release_manager", "release_drafts", "write"}, {"POST", "/api/releases/{id}/schedule", "release_manager", "live_release", "control"},
      {"POST", "/api/releases/{id}/start", "release_manager", "live_release", "control"}, {"POST", "/api/releases/{id}/cancel", "release_manager", "live_release", "control"},
      {"POST", "/api/releases/{id}/pause", "release_manager", "live_release", "control"}, {"POST", "/api/releases/{id}/resume", "release_manager", "live_release", "control"},
      {"POST", "/api/releases/{id}/abort", "release_manager", "live_release", "control"}, {"POST", "/api/releases/{id}/rollback", "release_manager", "live_release", "control"},
      {"GET", "/api/releases/{id}/gates", "viewer", "live_release", "read"}, {"POST", "/api/releases/{id}/gates/{evaluation_id}/override", "release_manager", "live_release", "control"},
      {"GET", "/api/releases/{id}/assignments", "viewer", "live_release", "read"}, {"POST", "/api/releases/{id}/replays", "release_manager", "simulation", "write"},
      {"GET", "/api/approvals", "approver", "approvals", "read"}, {"POST", "/api/approvals/{id}/approve", "approver", "approvals", "approve"}, {"POST", "/api/approvals/{id}/reject", "approver", "approvals", "approve"},
      {"GET", "/api/simulations", "viewer", "simulation", "read"}, {"POST", "/api/simulations", "release_manager", "simulation", "write"}, {"GET", "/api/simulations/{id}", "viewer", "simulation", "read"},
      {"POST", "/api/simulations/{id}/cancel", "release_manager", "simulation", "write"}, {"POST", "/api/simulations/{id}/replay", "release_manager", "simulation", "write"},
      {"GET", "/api/benchmarks", "viewer", "simulation", "read"}, {"POST", "/api/benchmarks", "release_manager", "simulation", "write"}, {"GET", "/api/benchmarks/{id}", "viewer", "simulation", "read"},
      {"GET", "/api/replays", "viewer", "simulation", "read"}, {"GET", "/api/replays/{id}", "viewer", "simulation", "read"},
      {"GET", "/api/evidence", "viewer", "evidence", "read"}, {"POST", "/api/evidence", "admin", "evidence", "write"}, {"POST", "/api/evidence/verify", "viewer", "evidence", "read"},
      {"POST", "/api/evidence/exports", "release_manager", "evidence_exports", "write"}, {"GET", "/api/evidence/exports/{id}", "viewer", "evidence", "read"}, {"GET", "/api/evidence/exports/{id}/download", "viewer", "evidence", "read"},
      {"GET", "/api/notices", "viewer", "evidence", "read"}, {"POST", "/api/notices/{id}/acknowledge", "viewer", "evidence", "read"},
      {"GET", "/api/integrations", "viewer", "integrations", "read"}, {"PUT", "/api/integrations/{adapter}", "admin", "integrations", "write"},
      {"POST", "/api/integrations/{adapter}/test", "admin", "integrations", "write"}, {"POST", "/api/integrations/{adapter}/enable", "admin", "integrations", "write"}, {"POST", "/api/integrations/{adapter}/disable", "admin", "integrations", "write"},
      {"GET", "/api/outbox", "admin", "tenant", "write"}, {"POST", "/api/outbox/{id}/retry", "admin", "tenant", "write"},
      {"GET", "/api/agent/v1/desired-state", "device", "device_protocol", "self"}, {"GET", "/api/agent/v1/artifacts/{digest}", "device", "device_protocol", "self"},
      {"POST", "/api/agent/v1/reports", "device", "device_protocol", "log"}, {"POST", "/api/agent/v1/credential-rotation/ack", "device", "device_protocol", "self"},
  };
  return routes;
}

bool requireRole(const shared::TenantContext& context, std::string_view resource, std::string_view permission) {
  if (!context.authenticated) return false;
  if (context.role == shared::Role::admin) return true;
  if (context.role == shared::Role::viewer) return permission == "read";
  if (context.role == shared::Role::approver) return permission == "read" || (resource == "approvals" && permission == "approve");
  if (context.role == shared::Role::release_manager) {
    if (permission == "read") return true;
    if (permission == "write") return resource == "fleets" || resource == "devices" || resource == "artifacts" || resource == "policies" || resource == "release_drafts" || resource == "simulation" || resource == "evidence_exports";
    return permission == "control" && resource == "live_release";
  }
  return false;
}

}  // namespace edgefleet::web
