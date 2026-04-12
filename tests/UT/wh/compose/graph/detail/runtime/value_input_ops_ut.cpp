#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/value_input_ops.hpp"

TEST_CASE("value input ops append materialize and build keyed fan-in maps",
          "[UT][wh/compose/graph/detail/runtime/"
          "value_input_ops.hpp][build_value_input_map][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  value_batch direct{};
  direct.form = value_input_form::direct;
  value_input first{};
  first.source_id = 1U;
  first.edge_id = 2U;
  first.owned = wh::compose::graph_value{3};
  REQUIRE(append_value_input(direct, std::move(first)).has_value());
  REQUIRE(direct.single.has_value());

  value_input duplicate{};
  duplicate.owned = wh::compose::graph_value{4};
  auto duplicate_status = append_value_input(direct, std::move(duplicate));
  REQUIRE(duplicate_status.has_error());
  REQUIRE(duplicate_status.error() == wh::core::errc::contract_violation);

  value_batch fan_in{};
  fan_in.form = value_input_form::fan_in;
  value_input fan_entry{};
  fan_entry.source_id = 3U;
  fan_entry.edge_id = 4U;
  fan_entry.owned = wh::compose::graph_value{5};
  REQUIRE(append_value_input(fan_in, std::move(fan_entry)).has_value());
  REQUIRE(fan_in.fan_in.size() == 1U);

  auto materialized = materialize_value_input(*direct.single);
  REQUIRE(materialized.has_value());
  auto *materialized_int = wh::core::any_cast<int>(&materialized.value());
  REQUIRE(materialized_int != nullptr);
  REQUIRE(*materialized_int == 3);

  std::string borrowed_source = "seed";
  wh::compose::graph_value borrowed_value{wh::core::any::ref(borrowed_source)};
  value_input borrowed{};
  borrowed.borrowed = &borrowed_value;
  auto borrowed_materialized = materialize_value_input(borrowed);
  REQUIRE(borrowed_materialized.has_value());
  auto *borrowed_text = wh::core::any_cast<std::string>(&borrowed_materialized.value());
  REQUIRE(borrowed_text != nullptr);
  REQUIRE(*borrowed_text == "seed");
  borrowed_source = "mutated";
  REQUIRE(*borrowed_text == "seed");

  value_input missing{};
  auto missing_status = materialize_value_input(missing);
  REQUIRE(missing_status.has_error());
  REQUIRE(missing_status.error() == wh::core::errc::not_found);

  value_input invalid{};
  auto invalid_reader = wh::compose::make_single_value_stream_reader(wh::compose::graph_value{8});
  REQUIRE(invalid_reader.has_value());
  invalid.owned = wh::compose::graph_value{std::move(invalid_reader).value()};
  auto invalid_status = materialize_value_input(invalid);
  REQUIRE(invalid_status.has_error());
  REQUIRE(invalid_status.error() == wh::core::errc::contract_violation);

  std::vector<value_input> entries{};
  value_input left{};
  left.edge_id = 10U;
  left.owned = wh::compose::graph_value{1};
  entries.push_back(std::move(left));
  value_input right{};
  right.edge_id = 11U;
  right.owned = wh::compose::graph_value{2};
  entries.push_back(std::move(right));

  auto map = build_value_input_map(entries, [](const value_input &entry) {
    return entry.edge_id == 10U ? std::string_view{"left"} : std::string_view{"right"};
  });
  REQUIRE(map.has_value());
  REQUIRE(map->size() == 2U);
  REQUIRE(wh::core::any_cast<int>(&map->at("left")) != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&map->at("right")) == 2);
}

TEST_CASE("value input ops reject invalid batch forms and keep the last duplicate keyed value",
          "[UT][wh/compose/graph/detail/runtime/"
          "value_input_ops.hpp][append_value_input][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  value_batch invalid{};
  invalid.form = static_cast<value_input_form>(0xFF);
  auto invalid_status = append_value_input(invalid, value_input{});
  REQUIRE(invalid_status.has_error());
  REQUIRE(invalid_status.error() == wh::core::errc::internal_error);

  std::vector<value_input> entries{};
  value_input first{};
  first.edge_id = 1U;
  first.owned = wh::compose::graph_value{10};
  entries.push_back(std::move(first));

  value_input second{};
  second.edge_id = 2U;
  second.owned = wh::compose::graph_value{20};
  entries.push_back(std::move(second));

  auto map =
      build_value_input_map(entries, [](const value_input &) { return std::string_view{"same"}; });
  REQUIRE(map.has_value());
  REQUIRE(map->size() == 1U);
  REQUIRE(*wh::core::any_cast<int>(&map->at("same")) == 20);
}
