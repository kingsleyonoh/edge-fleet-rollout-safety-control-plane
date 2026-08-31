#pragma once

#include <string>
#include <vector>

#include "shared/types.hpp"

namespace edgefleet::application {

struct FleetImportRow {
  std::string stableKey;
  std::string hardwareModel;
  std::string architecture;
  std::string environment;
};

class Interoperability {
 public:
  static shared::Result<std::vector<FleetImportRow>> parseFleetCsv(const std::string& content);
  static shared::Result<std::vector<shared::Json>> parseEvidenceNdjson(const std::string& content);
};

}  // namespace edgefleet::application
