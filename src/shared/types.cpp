#include "shared/types.hpp"

#include <array>
#include <iomanip>
#include <sstream>

namespace edgefleet::shared {

Uuid Uuid::generate() {
  std::random_device device;
  std::mt19937_64 generator(device());
  std::array<std::uint64_t, 2> words{generator(), generator()};
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << words[0] << std::setw(16) << words[1];
  return Uuid(output.str());
}

bool Uuid::isValid() const { return value_.size() == 32 && value_.find_first_not_of("0123456789abcdef") == std::string::npos; }

std::string toString(Role role) {
  switch (role) {
    case Role::admin: return "admin";
    case Role::release_manager: return "release_manager";
    case Role::approver: return "approver";
    case Role::viewer: return "viewer";
    case Role::device: return "device";
  }
  return "viewer";
}

std::optional<Role> roleFromString(std::string_view value) {
  for (const auto role : {Role::admin, Role::release_manager, Role::approver, Role::viewer, Role::device}) {
    if (value == toString(role)) return role;
  }
  return std::nullopt;
}

}  // namespace edgefleet::shared
