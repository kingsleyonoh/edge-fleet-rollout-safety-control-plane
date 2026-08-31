#pragma once

#include <string_view>

#include "shared/types.hpp"

namespace edgefleet::shared {

class Logger {
 public:
  static void configure(std::string_view level, std::string_view format);
  static void event(std::string_view level, std::string_view eventName, Json fields = Json::object());
};

}  // namespace edgefleet::shared
