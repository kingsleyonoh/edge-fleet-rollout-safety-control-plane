#include "application/interoperability.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace edgefleet::application {
namespace {

shared::Error invalid(std::string message, int line = 0) {
  (void)line;
  return {"INVALID_FIXTURE", std::move(message), 422};
}

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}

shared::Result<std::vector<std::string>> csvRecord(const std::string& line, int lineNumber) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  bool closedQuote = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const auto character = line[index];
    if (quoted) {
      if (character == '"') {
        if (index + 1 < line.size() && line[index + 1] == '"') { field.push_back('"'); ++index; }
        else { quoted = false; closedQuote = true; }
      } else field.push_back(character);
      continue;
    }
    if (closedQuote) {
      if (character == ',') { fields.push_back(field); field.clear(); closedQuote = false; }
      else return shared::Result<std::vector<std::string>>::failure(invalid("CSV data follows a closed quoted field.", lineNumber));
      continue;
    }
    if (character == ',') fields.push_back(field), field.clear();
    else if (character == '"' && field.empty()) quoted = true;
    else field.push_back(character);
  }
  if (quoted) return shared::Result<std::vector<std::string>>::failure(invalid("CSV quoted field is not closed.", lineNumber));
  fields.push_back(field);
  return shared::Result<std::vector<std::string>>::success(std::move(fields));
}

bool validToken(const std::string& value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum) return false;
  return std::all_of(value.begin(), value.end(), [](const char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_' || character == '.' || character == ':';
  });
}

}  // namespace

shared::Result<std::vector<FleetImportRow>> Interoperability::parseFleetCsv(const std::string& content) {
  if (content.empty() || content.size() > 16ULL * 1024ULL * 1024ULL) return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV is empty or exceeds 16 MiB."));
  std::istringstream input(content);
  std::string line;
  int lineNumber = 0;
  if (!std::getline(input, line)) return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV has no header."));
  ++lineNumber;
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) line.erase(0, 3);
  const auto header = csvRecord(line, lineNumber);
  if (!header.ok() || *header.value != std::vector<std::string>{"stable_key", "hardware_model", "architecture", "environment"}) return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV header must be stable_key,hardware_model,architecture,environment.", lineNumber));
  std::vector<FleetImportRow> rows;
  std::unordered_set<std::string> stableKeys;
  while (std::getline(input, line)) {
    ++lineNumber;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV cannot contain blank records.", lineNumber));
    const auto record = csvRecord(line, lineNumber);
    if (!record.ok() || record.value->size() != 4) return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV records must contain exactly four fields.", lineNumber));
    FleetImportRow row{trim((*record.value)[0]), trim((*record.value)[1]), trim((*record.value)[2]), trim((*record.value)[3])};
    if (!validToken(row.stableKey, 256) || !validToken(row.hardwareModel, 128) || !validToken(row.architecture, 128) ||
        (row.environment != "development" && row.environment != "staging" && row.environment != "production") || !stableKeys.insert(row.stableKey).second) {
      return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV contains an invalid or duplicate device row.", lineNumber));
    }
    rows.push_back(std::move(row));
    if (rows.size() > 100000) return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV contains more than 100000 rows.", lineNumber));
  }
  if (rows.empty()) return shared::Result<std::vector<FleetImportRow>>::failure(invalid("Fleet CSV must contain at least one device row."));
  return shared::Result<std::vector<FleetImportRow>>::success(std::move(rows));
}

shared::Result<std::vector<shared::Json>> Interoperability::parseEvidenceNdjson(const std::string& content) {
  if (content.empty() || content.size() > 64ULL * 1024ULL * 1024ULL) return shared::Result<std::vector<shared::Json>>::failure(invalid("Evidence NDJSON is empty or exceeds 64 MiB."));
  std::istringstream input(content);
  std::string line;
  int lineNumber = 0;
  std::vector<shared::Json> events;
  while (std::getline(input, line)) {
    ++lineNumber;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.size() > 1024ULL * 1024ULL) return shared::Result<std::vector<shared::Json>>::failure(invalid("Evidence NDJSON contains a blank or oversized line.", lineNumber));
    try {
      const auto event = shared::Json::parse(line);
      if (!event.is_object() || event.size() != 5 || event.value("schema_version", "") != "v1" || !event.contains("event_type") || !event.at("event_type").is_string() ||
          !event.contains("aggregate_type") || !event.at("aggregate_type").is_string() || !event.contains("aggregate_id") || !event.at("aggregate_id").is_string() ||
          !event.contains("payload") || !event.at("payload").is_object() || !validToken(event.value("event_type", ""), 128) ||
          !validToken(event.value("aggregate_type", ""), 128) || !validToken(event.value("aggregate_id", ""), 256)) {
        return shared::Result<std::vector<shared::Json>>::failure(invalid("Evidence event does not match the v1 envelope.", lineNumber));
      }
      static const std::unordered_set<std::string> fields{"schema_version", "event_type", "aggregate_type", "aggregate_id", "payload"};
      for (const auto& item : event.items()) if (!fields.contains(item.key())) return shared::Result<std::vector<shared::Json>>::failure(invalid("Evidence event contains an unknown field.", lineNumber));
      events.push_back(event);
    } catch (const std::exception&) {
      return shared::Result<std::vector<shared::Json>>::failure(invalid("Evidence NDJSON contains invalid JSON.", lineNumber));
    }
    if (events.size() > 100000) return shared::Result<std::vector<shared::Json>>::failure(invalid("Evidence NDJSON contains more than 100000 events.", lineNumber));
  }
  if (events.empty()) return shared::Result<std::vector<shared::Json>>::failure(invalid("Evidence NDJSON must contain at least one event."));
  return shared::Result<std::vector<shared::Json>>::success(std::move(events));
}

}  // namespace edgefleet::application
