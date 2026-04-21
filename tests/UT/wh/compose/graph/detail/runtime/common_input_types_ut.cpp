#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/common_input_types.hpp"

TEST_CASE("common input runtime types materialize values keep move ownership and track io storage",
          "[UT][wh/compose/graph/detail/runtime/"
          "common_input_types.hpp][resolved_input::materialize][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  wh::compose::graph_value direct_value{7};
  auto borrowed = resolved_input::borrow_value(direct_value);
  auto borrowed_value = std::move(borrowed).materialize();
  REQUIRE(borrowed_value.has_value());
  auto *borrowed_int = wh::core::any_cast<int>(&borrowed_value.value());
  REQUIRE(borrowed_int != nullptr);
  REQUIRE(*borrowed_int == 7);

  std::string borrowed_text_source = "seed";
  wh::compose::graph_value borrowed_text{wh::core::any::ref(borrowed_text_source)};
  auto borrowed_text_input = resolved_input::borrow_value(borrowed_text);
  auto borrowed_text_value = std::move(borrowed_text_input).materialize();
  REQUIRE(borrowed_text_value.has_value());
  auto *owned_text = wh::core::any_cast<std::string>(&borrowed_text_value.value());
  REQUIRE(owned_text != nullptr);
  REQUIRE(*owned_text == "seed");
  borrowed_text_source = "mutated";
  REQUIRE(*owned_text == "seed");

  auto owned = resolved_input::own_value(wh::compose::graph_value{9});
  auto owned_value = std::move(owned).materialize();
  REQUIRE(owned_value.has_value());
  auto *owned_int = wh::core::any_cast<int>(&owned_value.value());
  REQUIRE(owned_int != nullptr);
  REQUIRE(*owned_int == 9);

  auto owned_reader = wh::compose::make_single_value_stream_reader(wh::compose::graph_value{3});
  REQUIRE(owned_reader.has_value());
  auto reader_input = resolved_input::own_reader(std::move(owned_reader).value());
  auto reader_value = std::move(reader_input).materialize();
  REQUIRE(reader_value.has_value());
  REQUIRE(wh::core::any_cast<wh::compose::graph_stream_reader>(&reader_value.value()) != nullptr);
  REQUIRE_FALSE(reader_value.value().borrowed());

  auto borrowed_reader = wh::compose::make_single_value_stream_reader(wh::compose::graph_value{21});
  REQUIRE(borrowed_reader.has_value());
  auto borrowed_reader_input = resolved_input::borrow_reader(borrowed_reader.value());
  auto borrowed_reader_value = std::move(borrowed_reader_input).materialize();
  REQUIRE(borrowed_reader_value.has_value());
  REQUIRE_FALSE(borrowed_reader_value.value().borrowed());
  auto *borrowed_reader_copy =
      wh::core::any_cast<wh::compose::graph_stream_reader>(&borrowed_reader_value.value());
  REQUIRE(borrowed_reader_copy != nullptr);

  auto borrowed_reader_values =
      wh::compose::collect_graph_stream_reader(std::move(borrowed_reader).value());
  REQUIRE(borrowed_reader_values.has_value());
  REQUIRE(borrowed_reader_values.value().size() == 1U);
  auto *borrowed_reader_chunk = wh::core::any_cast<int>(&borrowed_reader_values.value().front());
  REQUIRE(borrowed_reader_chunk != nullptr);
  REQUIRE(*borrowed_reader_chunk == 21);

  auto copied_reader_values =
      wh::compose::collect_graph_stream_reader(std::move(*borrowed_reader_copy));
  REQUIRE(copied_reader_values.has_value());
  REQUIRE(copied_reader_values.value().size() == 1U);
  auto *copied_reader_chunk = wh::core::any_cast<int>(&copied_reader_values.value().front());
  REQUIRE(copied_reader_chunk != nullptr);
  REQUIRE(*copied_reader_chunk == 21);

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

  std::vector<dag_node_phase> progress(3U, dag_node_phase::pending);
  REQUIRE(progress.size() >= 3U);
  for (std::size_t index = 0U; index < 3U; ++index) {
    REQUIRE(progress[index] == dag_node_phase::pending);
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

  auto output_reader = wh::compose::make_single_value_stream_reader(wh::compose::graph_value{21});
  REQUIRE(output_reader.has_value());
  storage.mark_final_output_reader(1U, std::move(output_reader).value());
  REQUIRE(storage.output_valid.test(1U));
  REQUIRE(storage.final_output_reader.has_value());
}

TEST_CASE("common input runtime helper structs reset flags and preserve default sentinel states",
          "[UT][wh/compose/graph/detail/runtime/"
          "common_input_types.hpp][runtime_io_storage::reset][condition][branch][boundary]") {
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
  storage.mark_final_output_reader(1U, wh::compose::graph_stream_reader{});
  REQUIRE(storage.output_valid.test(0U));
  REQUIRE(storage.final_output_reader.has_value());
  storage.edge_value_valid.set(1U);
  storage.edge_reader_valid.set(1U);
  storage.merged_reader_valid.set(1U);
  storage.merged_reader_lane_states[1U] = reader_lane_state::attached;

  storage.reset(1U, 1U);
  REQUIRE_FALSE(storage.output_valid.test(0U));
  REQUIRE_FALSE(storage.final_output_reader.has_value());
  REQUIRE_FALSE(storage.edge_value_valid.test(0U));
  REQUIRE_FALSE(storage.edge_reader_valid.test(0U));
  REQUIRE_FALSE(storage.merged_reader_valid.test(0U));
  REQUIRE(storage.merged_reader_lane_states[0U] == reader_lane_state::unseen);

  std::vector<dag_node_phase> progress(2U, dag_node_phase::pending);
  progress[1U] = dag_node_phase::executed;
  progress.resize(1U);
  REQUIRE(progress[0U] == dag_node_phase::pending);
}
