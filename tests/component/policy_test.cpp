#include <catch2/catch_test_macros.hpp>

#include "shared/types.hpp"
#include "web/policy_registry.hpp"

namespace {

edgefleet::shared::TenantContext context(edgefleet::shared::Role role) {
  edgefleet::shared::TenantContext result;
  result.authenticated = true;
  result.tenantId = "tenant";
  result.role = role;
  return result;
}

}  // namespace

TEST_CASE("the role matrix permits only the documented resource operations", "[component][security]") {
  const auto admin = context(edgefleet::shared::Role::admin);
  const auto manager = context(edgefleet::shared::Role::release_manager);
  const auto approver = context(edgefleet::shared::Role::approver);
  const auto viewer = context(edgefleet::shared::Role::viewer);

  REQUIRE(edgefleet::web::requireRole(admin, "anything", "anything"));
  REQUIRE(edgefleet::web::requireRole(manager, "fleets", "write"));
  REQUIRE(edgefleet::web::requireRole(manager, "release_drafts", "write"));
  REQUIRE(edgefleet::web::requireRole(manager, "live_release", "control"));
  REQUIRE(edgefleet::web::requireRole(manager, "evidence", "read"));
  REQUIRE_FALSE(edgefleet::web::requireRole(manager, "live_release", "write"));
  REQUIRE_FALSE(edgefleet::web::requireRole(manager, "integrations", "write"));
  REQUIRE(edgefleet::web::requireRole(approver, "approvals", "approve"));
  REQUIRE(edgefleet::web::requireRole(approver, "releases", "read"));
  REQUIRE_FALSE(edgefleet::web::requireRole(approver, "releases", "write"));
  REQUIRE(edgefleet::web::requireRole(viewer, "devices", "read"));
  REQUIRE(edgefleet::web::requireRole(viewer, "tenant", "read"));
  REQUIRE_FALSE(edgefleet::web::requireRole(viewer, "devices", "write"));
  REQUIRE(edgefleet::web::routePermissions().size() >= 70);
}
