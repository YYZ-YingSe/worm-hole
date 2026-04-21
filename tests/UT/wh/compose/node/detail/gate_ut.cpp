#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/node/detail/gate.hpp"

TEST_CASE("value_gate exposes empty and exact metadata views",
          "[UT][wh/compose/node/detail/gate.hpp][value_gate][condition][branch][boundary]") {
  const wh::compose::value_gate empty{};
  REQUIRE(empty.empty());
  REQUIRE(empty.key() == wh::core::any_type_key{});
  REQUIRE(empty.name().empty());

  const auto exact = wh::compose::value_gate::exact<int>();
  REQUIRE_FALSE(exact.empty());
  REQUIRE(exact.key() == wh::core::any_info_v<int>.key);
  REQUIRE(exact.name() == wh::core::any_info_v<int>.name);
}

TEST_CASE("input_gate factories distinguish open exact and reader contracts",
          "[UT][wh/compose/node/detail/gate.hpp][input_gate][condition][branch][boundary]") {
  const auto exact = wh::compose::value_gate::exact<int>();
  const auto open = wh::compose::input_gate::open();
  REQUIRE(open.kind == wh::compose::input_gate_kind::value_open);
  REQUIRE(open.value.empty());

  const auto exact_input = wh::compose::input_gate::exact<int>();
  REQUIRE(exact_input.kind == wh::compose::input_gate_kind::value_exact);
  REQUIRE(exact_input.value == exact);

  const auto reader_input = wh::compose::input_gate::reader();
  REQUIRE(reader_input.kind == wh::compose::input_gate_kind::reader);
  REQUIRE(reader_input.value.empty());
}

TEST_CASE("output_gate factories distinguish dynamic exact passthrough and reader contracts",
          "[UT][wh/compose/node/detail/gate.hpp][output_gate][condition][branch][boundary]") {
  const auto exact = wh::compose::value_gate::exact<int>();
  const auto dynamic = wh::compose::output_gate::dynamic();
  REQUIRE(dynamic.kind == wh::compose::output_gate_kind::value_dynamic);
  REQUIRE(dynamic.value.empty());

  const auto exact_output = wh::compose::output_gate::exact<int>();
  REQUIRE(exact_output.kind == wh::compose::output_gate_kind::value_exact);
  REQUIRE(exact_output.value == exact);

  const auto passthrough = wh::compose::output_gate::passthrough();
  REQUIRE(passthrough.kind == wh::compose::output_gate_kind::value_passthrough);
  REQUIRE(passthrough.value.empty());

  const auto reader_output = wh::compose::output_gate::reader();
  REQUIRE(reader_output.kind == wh::compose::output_gate_kind::reader);
  REQUIRE(reader_output.value.empty());
}

TEST_CASE("gate_name covers all known gate enums and unknown fallbacks",
          "[UT][wh/compose/node/detail/gate.hpp][gate_name][condition][branch]") {
  REQUIRE(wh::compose::gate_name(wh::compose::input_gate_kind::value_open) == "value_open");
  REQUIRE(wh::compose::gate_name(wh::compose::input_gate_kind::value_exact) == "value_exact");
  REQUIRE(wh::compose::gate_name(wh::compose::input_gate_kind::reader) == "reader");
  REQUIRE(wh::compose::gate_name(static_cast<wh::compose::input_gate_kind>(255)) == "value_open");

  REQUIRE(wh::compose::gate_name(wh::compose::output_gate_kind::value_dynamic) == "value_dynamic");
  REQUIRE(wh::compose::gate_name(wh::compose::output_gate_kind::value_exact) == "value_exact");
  REQUIRE(wh::compose::gate_name(wh::compose::output_gate_kind::value_passthrough) ==
          "value_passthrough");
  REQUIRE(wh::compose::gate_name(wh::compose::output_gate_kind::reader) == "reader");
  REQUIRE(wh::compose::gate_name(static_cast<wh::compose::output_gate_kind>(255)) ==
          "value_dynamic");
}
