#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace edgefleet::shared {

using Json = nlohmann::json;

struct Error {
  std::string code;
  std::string message;
  int status = 500;
  Json details = Json::array();
};

template <typename T>
struct Result {
  std::optional<T> value;
  std::optional<Error> error;
  bool ok() const { return value.has_value(); }
  static Result success(T result) { return {std::move(result), std::nullopt}; }
  static Result failure(Error result) { return {std::nullopt, std::move(result)}; }
};

class Uuid {
 public:
  Uuid() = default;
  explicit Uuid(std::string value) : value_(std::move(value)) {}
  static Uuid generate();
  bool isValid() const;
  const std::string& str() const { return value_; }
  bool operator==(const Uuid& other) const { return value_ == other.value_; }
  bool operator!=(const Uuid& other) const { return !(*this == other); }
  bool operator<(const Uuid& other) const { return value_ < other.value_; }

 private:
  std::string value_;
};

enum class Role { admin, release_manager, approver, viewer, device };
std::string toString(Role role);
std::optional<Role> roleFromString(std::string_view value);

struct TenantContext {
  std::string tenantId;
  std::string actorId;
  Role role = Role::viewer;
  bool authenticated = false;
};

}  // namespace edgefleet::shared
