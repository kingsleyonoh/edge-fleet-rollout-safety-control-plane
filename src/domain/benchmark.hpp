#pragma once

#include <string>
#include <vector>

#include "shared/types.hpp"

namespace edgefleet::domain {

struct BenchmarkCell {
  std::string scenario;
  int seed = 0;
  std::string strategy;
  shared::Json metrics;
  std::string digest;
};

struct BenchmarkReport {
  std::string corpusVersion = "v1";
  std::vector<BenchmarkCell> cells;
  std::string digest;
};

class BenchmarkRunner {
 public:
  static shared::Json frozenManifest();
  static shared::Result<BenchmarkReport> run(std::string_view corpusVersion, int deviceCount = 1000);
  static shared::Result<BenchmarkReport> runManifest(const shared::Json& manifest, int deviceCountOverride = 0);
};

}  // namespace edgefleet::domain
