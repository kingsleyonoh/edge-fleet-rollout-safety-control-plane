#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "shared/types.hpp"

namespace edgefleet::web {

struct RoutePermission {
  std::string method;
  std::string prefix;
  std::string role;
  std::string resource;
  std::string permission;
};

const std::vector<RoutePermission>& routePermissions();
bool requireRole(const shared::TenantContext& context, std::string_view resource, std::string_view permission);

}  // namespace edgefleet::web
