#include <catch2/catch_test_macros.hpp>

#include "wh/compose/payload/map.hpp"

TEST_CASE("payload map helpers convert payloads and read keyed entries",
          "[UT][wh/compose/payload/map.hpp][payload_to_value_map][condition][branch][boundary]") {
  wh::compose::graph_value_map map{};
  map.insert_or_assign("id", wh::compose::graph_value{7});

  wh::compose::graph_value payload = wh::compose::value_map_to_payload(map);
  auto converted = wh::compose::payload_to_value_map(payload);
  REQUIRE(converted.has_value());
  REQUIRE(*wh::core::any_cast<int>(&converted.value().at("id")) == 7);

  auto borrowed = wh::compose::payload_to_value_map_cref(payload);
  REQUIRE(borrowed.has_value());
  REQUIRE(*wh::core::any_cast<int>(&borrowed.value().get().at("id")) == 7);

  auto keyed = wh::compose::extract_keyed_input(map, "id");
  REQUIRE(keyed.has_value());
  REQUIRE(*wh::core::any_cast<int>(&keyed.value()) == 7);

  auto moved = wh::compose::payload_to_value_map(std::move(payload));
  REQUIRE(moved.has_value());
  REQUIRE(*wh::core::any_cast<int>(&moved.value().at("id")) == 7);
}

TEST_CASE("payload map helpers reject non-map payloads and missing keys",
          "[UT][wh/compose/payload/map.hpp][extract_keyed_input][condition][branch][boundary]") {
  auto mismatch = wh::compose::payload_to_value_map(wh::compose::graph_value{7});
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);

  auto cref_mismatch = wh::compose::payload_to_value_map_cref(wh::compose::graph_value{9});
  REQUIRE(cref_mismatch.has_error());
  REQUIRE(cref_mismatch.error() == wh::core::errc::type_mismatch);

  wh::compose::graph_value_map map{};
  auto missing = wh::compose::extract_keyed_input(map, "missing");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  auto moved_missing = wh::compose::extract_keyed_input(std::move(map), "missing");
  REQUIRE(moved_missing.has_error());
  REQUIRE(moved_missing.error() == wh::core::errc::not_found);
}

TEST_CASE("payload map helpers overwrite keyed outputs and preserve rvalue payload wrapping",
          "[UT][wh/compose/payload/map.hpp][write_keyed_output][condition][branch][boundary]") {
  wh::compose::graph_value_map map{};
  wh::compose::write_keyed_output(map, "name", std::string{"first"});
  wh::compose::write_keyed_output(map, "name", wh::compose::graph_value{11});
  REQUIRE(map.size() == 1U);
  REQUIRE(*wh::core::any_cast<int>(&map.at("name")) == 11);

  auto payload = wh::compose::value_map_to_payload(std::move(map));
  auto converted = wh::compose::payload_to_value_map(std::move(payload));
  REQUIRE(converted.has_value());
  REQUIRE(converted.value().size() == 1U);
  REQUIRE(*wh::core::any_cast<int>(&converted.value().at("name")) == 11);
}
