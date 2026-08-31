#pragma once

#include <string>

#include "shared/types.hpp"

namespace edgefleet::application {

struct EvidenceExport {
  std::string ndjson;
  std::string manifest;
  std::string sha256;
};

class EvidenceExporter {
 public:
  static EvidenceExport build(const shared::Json& tenant, const shared::Json& events);
};

}  // namespace edgefleet::application
