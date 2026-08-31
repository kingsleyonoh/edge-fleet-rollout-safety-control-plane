#include "shared/canonical_json.hpp"

#include <algorithm>

namespace edgefleet::shared {
namespace {

Json normalizeValue(const Json& value) {
  if (value.is_array()) {
    Json result = Json::array();
    for (const auto& item : value) result.push_back(normalizeValue(item));
    return result;
  }
  if (!value.is_object()) return value;
  std::vector<std::string> keys;
  for (const auto& [key, ignored] : value.items()) keys.push_back(key);
  std::sort(keys.begin(), keys.end());
  Json result = Json::object();
  for (const auto& key : keys) result[key] = normalizeValue(value.at(key));
  return result;
}

}  // namespace

Json CanonicalJson::normalize(const Json& value) { return normalizeValue(value); }

std::string CanonicalJson::serialize(const Json& value) { return normalize(value).dump(-1, ' ', false, Json::error_handler_t::strict); }

}  // namespace edgefleet::shared
