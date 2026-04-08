#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/common_input_types.hpp"

TEST_CASE("common input runtime types materialize values keep move ownership and track io storage",
          "[UT][wh/compose/graph/detail/runtime/common_input_types.hpp][resolved_input::materialize][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  wh::compose::graph_value direct_value{7};
  auto borrowed = resolved_input::borrow_value(direct_value);
  auto borrowed_value = std::move(borrowed).materialize();
  REQUIRE(borrowed_value.has_value());
  auto *borrowed_int = wh::core::any_cast<int>(&borrowed_value.value());
  REQUIRE(borrowed_int != nullptr);
  REQUIRE(*borrowed_int == 7);

  auto owned = resolved_input::own_value(wh::compose::graph_value{9});
  auto owned_value = std::move(owned).materialize();
  REQUIRE(owned_value.has_value());
  auto *owned_int = wh::core::any_cast<int>(&owned_value.value());
  REQUIRE(owned_int != nullptr);
  REQUIRE(*owned_int == 9);

  auto owned_reader =
      wh::compose::make_single_value_stream_reader(wh::compose::graph_value{3});
  REQUIRE(owned_reader.has_value());
  auto reader_input = resolved_input::own_reader(std::move(owned_reader).value());
  auto reader_value = std::move(reader_input).materialize();
  REQUIRE(reader_value.has_value());
  REQUIRE(wh::core::any_cast<wh::compose::graph_stream_reader>(&reader_value.value()) !=
          nullptr);

  auto empty = resolved_input{};
  auto empty_value = std::move(empty).materialize();
  REQUIRE(empty_value.has_value());
  REQUIRE_FALSE(empty_value.value().has_value());

  value_input borrowed_input{};
  borrowed_input.source_id = 1U;
  borrowed_input.edge_id = 2U;
  borrowed_input.borrowed = &direct_value;
  REQUIRE(borrowed_input.value() == &direct_value);

  value_input owned_input{};
  owned_input.owned = wh::compose::graph_value{17};
  auto moved_input = std::move(owned_input);
  REQUIRE(moved_input.value() != nullptr);
  auto *moved_int = wh::core::any_cast<int>(moved_input.value());
  REQUIRE(moved_int != nullptr);
  REQUIRE(*moved_int == 17);

  runtime_progress_state progress{};
  progress.reset(3U);
  REQUIRE(progress.node_states.size() >= 3U);
  for (std::size_t index = 0U; index < 3U; ++index) {
    REQUIRE(progress.node_states[index] == runtime_node_state::pending);
  }

  runtime_io_storage storage{};
  storage.reset(2U, 3U);
  REQUIRE(storage.node_values.size() >= 2U);
  REQUIRE(storage.edge_values.size() >= 3U);
  REQUIRE(storage.merged_reader_lane_states.size() >= 3U);
  REQUIRE_FALSE(storage.output_valid.test(0U));
  storage.mark_value_output(0U, wh::compose::graph_value{5});
  REQUIRE(storage.output_valid.test(0U));
  auto *stored_value = wh::core::any_cast<int>(&storage.node_values[0]);
  REQUIRE(stored_value != nullptr);
  REQUIRE(*stored_value == 5);

  auto output_reader =
      wh::compose::make_single_value_stream_reader(wh::compose::graph_value{21});
  REQUIRE(output_reader.has_value());
  storage.mark_reader_output(1U, std::move(output_reader).value());
  REQUIRE(storage.output_valid.test(1U));
}

TEST_CASE("common input runtime helper structs reset flags and preserve default sentinel states",
          "[UT][wh/compose/graph/detail/runtime/common_input_types.hpp][runtime_io_storage::reset][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  input_lane lane{};
  REQUIRE(lane.edge_id == 0U);
  REQUIRE(lane.source_id == 0U);
  REQUIRE(lane.status == input_edge_status::disabled);
  REQUIRE_FALSE(lane.output_ready);

  reader_lowering lowering{};
  REQUIRE(lowering.project == nullptr);
  REQUIRE(lowering.limits.max_items == 0U);

  value_batch batch{};
  REQUIRE(batch.form == value_input_form::direct);
  REQUIRE_FALSE(batch.single.has_value());
  REQUIRE(batch.fan_in.empty());

  runtime_io_storage storage{};
  storage.reset(2U, 2U);
  storage.mark_value_output(0U, wh::compose::graph_value{5});
  REQUIRE(storage.output_valid.test(0U));
  storage.edge_value_valid.set(1U);
  storage.edge_reader_valid.set(1U);
  storage.reader_copy_ready.set(1U);
  storage.merged_reader_valid.set(1U);
  storage.merged_reader_lane_states[1U] = reader_lane_state::attached;

  storage.reset(1U, 1U);
  REQUIRE_FALSE(storage.output_valid.test(0U));
  REQUIRE_FALSE(storage.edge_value_valid.test(0U));
  REQUIRE_FALSE(storage.edge_reader_valid.test(0U));
  REQUIRE_FALSE(storage.reader_copy_ready.test(0U));
  REQUIRE_FALSE(storage.merged_reader_valid.test(0U));
  REQUIRE(storage.merged_reader_lane_states[0U] == reader_lane_state::unseen);

  runtime_progress_state progress{};
  progress.reset(2U);
  progress.node_states[1U] = runtime_node_state::executed;
  progress.reset(1U);
  REQUIRE(progress.node_states[0U] == runtime_node_state::pending);
}
