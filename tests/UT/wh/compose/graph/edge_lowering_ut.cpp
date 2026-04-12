#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/edge_lowering.hpp"

TEST_CASE("edge lowering exposes stable string labels",
          "[UT][wh/compose/graph/edge_lowering.hpp][to_string][condition][branch][boundary]") {
  REQUIRE(wh::compose::to_string(wh::compose::edge_lowering_kind::none) == "none");
  REQUIRE(wh::compose::to_string(
              wh::compose::edge_lowering_kind::value_to_stream) ==
          "value_to_stream");
  REQUIRE(wh::compose::to_string(
              wh::compose::edge_lowering_kind::stream_to_value) ==
          "stream_to_value");
  REQUIRE(wh::compose::to_string(wh::compose::edge_lowering_kind::custom) ==
          "custom");
}

TEST_CASE("edge lowering kind keeps compact enum representation and distinct labels",
          "[UT][wh/compose/graph/edge_lowering.hpp][edge_lowering_kind][condition][branch][boundary]") {
  STATIC_REQUIRE(std::same_as<std::underlying_type_t<wh::compose::edge_lowering_kind>,
                              std::uint8_t>);
  REQUIRE(wh::compose::to_string(wh::compose::edge_lowering_kind::none) !=
          wh::compose::to_string(wh::compose::edge_lowering_kind::custom));
}
