#include "shared/tenant_clock.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace edgefleet::shared {

std::chrono::system_clock::time_point TenantClock::now() { return std::chrono::system_clock::now(); }

std::string TenantClock::nowIso8601() {
  const auto current = now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(current);
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(current - seconds).count();
  const auto raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &raw);
#else
  gmtime_r(&raw, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(6) << std::setfill('0') << micros << "Z";
  return output.str();
}

}  // namespace edgefleet::shared
