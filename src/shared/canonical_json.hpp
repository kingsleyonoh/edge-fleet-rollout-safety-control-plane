#pragma once

#include "shared/types.hpp"

namespace edgefleet::shared {

class CanonicalJson {
 public:
  static std::string serialize(const Json& value);
  static Json normalize(const Json& value);
};

}  // namespace edgefleet::shared
