#include "shared/secret_resolver.hpp"

#include <cctype>
#include <cstdlib>

namespace edgefleet::shared {

std::optional<std::string> SecretResolver::environment(const std::string& name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name.c_str()) != 0 || value == nullptr || length == 0 || *value == '\0') {
    if (value != nullptr) free(value);
    return std::nullopt;
  }
  std::string result(value, length - 1);
  free(value);
  return result;
#else
  const auto* value = std::getenv(name.c_str());
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
#endif
}

std::string SecretResolver::redact(std::string value) {
  if (value.empty()) return {};
  if (value.size() <= 8) return "[REDACTED]";
  return value.substr(0, 4) + "..." + value.substr(value.size() - 4);
}

std::optional<Error> SecretResolver::validateReference(const std::string& name) {
  if (name.empty() || name.size() > 128 || (!std::isalpha(static_cast<unsigned char>(name.front())) && name.front() != '_')) return Error{"INVALID_SECRET_REFERENCE", "A secret reference name is required.", 422};
  for (const auto character : name) if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') return Error{"INVALID_SECRET_REFERENCE", "A secret reference name is required.", 422};
  return std::nullopt;
}

}  // namespace edgefleet::shared
