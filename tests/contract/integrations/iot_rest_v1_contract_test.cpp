#include <catch2/catch_test_macros.hpp>

#include "infrastructure/integrations.hpp"

TEST_CASE("iot_rest_v1 factory exposes the supported fixture contract", "[contract][integration]") {
  const auto adapter = edgefleet::infrastructure::createAdapter(edgefleet::infrastructure::AdapterType::iot_rest_v1);
  REQUIRE(adapter);
  REQUIRE(adapter->name() == "iot_rest_v1");
  const auto result = adapter->fixtureDelivery({{"fixture_mode", true}, {"fixture_status", 200}}, "iot-contract", 0);
  REQUIRE(result.disposition == edgefleet::infrastructure::DeliveryDisposition::published);
}
