#include <string>

#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/payload.hpp"

namespace {

using wh::testing::helper::read_graph_value;

} // namespace

TEST_CASE("compose payload map and keyed helpers bridge payload shapes",
          "[core][compose][payload][functional]") {
  wh::compose::graph_value_map map_input{};
  map_input.insert_or_assign("left", wh::core::any(7));

  auto payload = wh::compose::value_map_to_payload(map_input);
  auto round_trip = wh::compose::payload_to_value_map(payload);
  REQUIRE(round_trip.has_value());
  REQUIRE(round_trip.value().contains("left"));
  auto left_value = read_graph_value<int>(round_trip.value().at("left"));
  REQUIRE(left_value.has_value());
  REQUIRE(left_value.value() == 7);

  auto moved_round_trip = wh::compose::payload_to_value_map(
      wh::compose::value_map_to_payload(wh::compose::graph_value_map{
          {"right", wh::core::any(std::string{"ok"})},
      }));
  REQUIRE(moved_round_trip.has_value());
  auto right_value = read_graph_value<std::string>(moved_round_trip.value().at("right"));
  REQUIRE(right_value.has_value());
  REQUIRE(right_value.value() == "ok");

  auto extracted = wh::compose::extract_keyed_input(round_trip.value(), "left");
  REQUIRE(extracted.has_value());
  auto extracted_value = read_graph_value<int>(extracted.value());
  REQUIRE(extracted_value.has_value());
  REQUIRE(extracted_value.value() == 7);

  wh::compose::graph_value_map writable{};
  wh::compose::write_keyed_output(writable, "int", 9);
  wh::compose::write_keyed_output(writable, "payload", wh::core::any(std::string{"v"}));
  REQUIRE(writable.contains("int"));
  REQUIRE(writable.contains("payload"));
  REQUIRE(read_graph_value<int>(writable.at("int")).value() == 9);
  REQUIRE(read_graph_value<std::string>(writable.at("payload")).value() == "v");

  auto packed = wh::compose::pack_keyed_payload("wrapped", wh::core::any(42));
  auto unpacked = wh::compose::unpack_keyed_payload(std::move(packed), "wrapped");
  REQUIRE(unpacked.has_value());
  REQUIRE(read_graph_value<int>(std::move(unpacked).value()).value() == 42);
}
