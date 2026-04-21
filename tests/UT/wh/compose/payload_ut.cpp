#include <catch2/catch_test_macros.hpp>

#include "wh/compose/payload.hpp"

TEST_CASE("compose payload facade exposes keyed packing helpers",
          "[UT][wh/compose/payload.hpp][pack_keyed_payload][condition][branch][boundary]") {
  auto payload = wh::compose::pack_keyed_payload("value", wh::compose::graph_value{3});
  auto unpacked = wh::compose::unpack_keyed_payload(payload, "value");
  auto missing = wh::compose::unpack_keyed_payload(payload, "other");

  REQUIRE(unpacked.has_value());
  REQUIRE(*wh::core::any_cast<int>(&unpacked.value()) == 3);
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("compose payload facade exposes map-payload conversion helpers",
          "[UT][wh/compose/payload.hpp][payload_to_value_map][condition][branch][boundary]") {
  wh::compose::graph_value_map map{};
  wh::compose::write_keyed_output(map, "answer", 42);

  auto payload = wh::compose::value_map_to_payload(map);
  auto cref = wh::compose::payload_to_value_map_cref(payload);
  auto copied = wh::compose::payload_to_value_map(payload);
  auto extracted = wh::compose::extract_keyed_input(map, "answer");

  REQUIRE(cref.has_value());
  REQUIRE(cref->get().contains("answer"));
  REQUIRE(copied.has_value());
  REQUIRE(copied->contains("answer"));
  REQUIRE(extracted.has_value());
  REQUIRE(*wh::core::any_cast<int>(&extracted.value()) == 42);
}
