#pragma once

#include <optional>
#include <string>

#include "shared/types.hpp"

namespace edgefleet::shared {

class SecretResolver {
 public:
  static std::optional<std::string> environment(const std::string& name);
  static std::string redact(std::string value);
  static std::optional<Error> validateReference(const std::string& name);
};

}  // namespace edgefleet::shared
