#include "application/evidence_export.hpp"

#include "shared/canonical_json.hpp"
#include "shared/digest_service.hpp"

namespace edgefleet::application {

EvidenceExport EvidenceExporter::build(const shared::Json& tenant, const shared::Json& events) {
  std::string ndjson;
  for (const auto& source : events) {
    auto event = source;
    if (event.contains("payload_json")) {
      event["payload"] = event.at("payload_json").is_string() ? shared::Json::parse(event.at("payload_json").get<std::string>()) : event.at("payload_json");
      event.erase("payload_json");
    }
    ndjson += shared::CanonicalJson::serialize(event) + "\n";
  }
  const auto digest = shared::DigestService::sha256Hex(ndjson);
  const auto first = events.empty() ? 0 : events.front().value("sequence_no", 0);
  const auto last = events.empty() ? 0 : events.back().value("sequence_no", 0);
  const auto manifestJson = shared::Json{{"format", "edgefleet-evidence-ndjson-v1"}, {"tenant", tenant}, {"event_count", events.size()}, {"sequence_from", first}, {"sequence_to", last}, {"sha256", digest}};
  return {std::move(ndjson), shared::CanonicalJson::serialize(manifestJson), digest};
}

}  // namespace edgefleet::application
