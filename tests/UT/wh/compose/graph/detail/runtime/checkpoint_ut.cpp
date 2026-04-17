#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/runtime/checkpoint.hpp"

TEST_CASE("runtime checkpoint helpers resolve serializers backends and validation errors",
          "[UT][wh/compose/graph/detail/runtime/checkpoint.hpp][validate_runtime_configuration][condition][branch][boundary]") {
  using namespace wh::compose::detail::checkpoint_runtime;

  wh::compose::detail::runtime_state::invoke_config config{};
  auto serializer = resolve_serializer(config);
  REQUIRE(serializer.has_value());
  REQUIRE(serializer.value() != nullptr);

  wh::compose::checkpoint_serializer invalid_serializer{};
  config.checkpoint_serializer = &invalid_serializer;
  auto invalid = resolve_serializer(config);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_backend backend{};
  config = {};
  config.checkpoint_store = &store;
  config.checkpoint_backend = &backend;
  auto resolved_backend = resolve_runtime_backend(config);
  REQUIRE(resolved_backend.has_error());
  REQUIRE(resolved_backend.error() == wh::core::errc::invalid_argument);

  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "cp-1";
  config = {};
  config.checkpoint_load = load_options;
  auto validated = validate_runtime_configuration(config, outputs);
  REQUIRE(validated.has_error());
  REQUIRE(validated.error() == wh::core::errc::contract_violation);
  REQUIRE(outputs.checkpoint_error.has_value());
  REQUIRE(outputs.checkpoint_error->checkpoint_id == "cp-1");
  REQUIRE(resolve_forwarded_restore_key(
              "graph", wh::compose::make_node_path({"root", "child"})) ==
          "root/child");
}

TEST_CASE("runtime checkpoint helpers roundtrip stream runtime payload codecs for save and load",
          "[UT][wh/compose/graph/detail/runtime/checkpoint.hpp][apply_stream_codecs_for_save][condition][branch][boundary]") {
  using namespace wh::compose::detail::checkpoint_runtime;

  wh::compose::checkpoint_stream_codecs codecs{};
  const auto start_key = std::string{wh::compose::graph_start_node_key};
  const auto end_key = std::string{wh::compose::graph_end_node_key};
  codecs.emplace(start_key,
                 wh::compose::make_default_stream_codec());
  codecs.emplace("worker", wh::compose::make_default_stream_codec());
  codecs.emplace(end_key, wh::compose::make_default_stream_codec());

  const std::vector<std::string> node_keys{
      std::string{wh::compose::graph_start_node_key},
      "worker",
      std::string{wh::compose::graph_end_node_key},
  };
  const std::vector<wh::compose::detail::graph_core::indexed_edge> indexed_edges{
      wh::compose::detail::graph_core::indexed_edge{
          .from = 0U,
          .to = 1U,
          .source_output = wh::compose::node_contract::stream,
          .target_input = wh::compose::node_contract::stream,
      },
  };

  wh::compose::checkpoint_state checkpoint{};
  checkpoint.runtime.dag = wh::compose::checkpoint_dag_runtime_state{};
  checkpoint.runtime.dag->pending_inputs.entry = wh::compose::graph_value{
      wh::compose::make_single_value_stream_reader(std::string{"start"}).value()};
  checkpoint.runtime.dag->pending_inputs.nodes.push_back(
      wh::compose::checkpoint_node_input{
      .node_id = 1U,
      .key = "worker",
      .input = wh::compose::graph_value{
          wh::compose::make_single_value_stream_reader(std::string{"chunk"})
              .value()},
  });
  checkpoint.runtime.dag->edge_readers.push_back(wh::compose::checkpoint_runtime_slot{
      .slot_id = 0U,
      .value = wh::compose::graph_value{
          wh::compose::make_single_value_stream_reader(std::string{"edge"})
              .value()},
  });
  checkpoint.runtime.dag->merged_readers.push_back(wh::compose::checkpoint_runtime_slot{
      .slot_id = 1U,
      .value = wh::compose::graph_value{
          wh::compose::make_single_value_stream_reader(std::string{"merged"})
              .value()},
  });
  checkpoint.runtime.dag->final_output_reader = wh::compose::graph_value{
      wh::compose::make_single_value_stream_reader(std::string{"final"})
          .value()};
  REQUIRE(checkpoint.runtime.dag->edge_readers.front().value.template has_value<
          wh::compose::graph_stream_reader>());

  wh::core::run_context context{};
  REQUIRE(apply_stream_codecs_for_save(checkpoint, context, &codecs, node_keys,
                                       indexed_edges, 2U)
              .has_value());
  REQUIRE(wh::core::any_cast<wh::compose::checkpoint_stream_value_payload>(
              &*checkpoint.runtime.dag->pending_inputs.entry) !=
          nullptr);
  REQUIRE(wh::core::any_cast<wh::compose::checkpoint_stream_value_payload>(
              &checkpoint.runtime.dag->pending_inputs.nodes.front().input) != nullptr);
  REQUIRE(wh::core::any_cast<wh::compose::checkpoint_stream_value_payload>(
              &checkpoint.runtime.dag->edge_readers.front().value) != nullptr);
  REQUIRE(wh::core::any_cast<wh::compose::checkpoint_stream_value_payload>(
              &checkpoint.runtime.dag->merged_readers.front().value) != nullptr);
  REQUIRE(wh::core::any_cast<wh::compose::checkpoint_stream_value_payload>(
              &*checkpoint.runtime.dag->final_output_reader) != nullptr);

  REQUIRE(apply_stream_codecs_for_load(checkpoint, context, &codecs, node_keys,
                                       indexed_edges, 2U)
              .has_value());
  auto start_reader = wh::testing::helper::read_graph_value<
      wh::compose::graph_stream_reader>(
      std::move(*checkpoint.runtime.dag->pending_inputs.entry));
  REQUIRE(start_reader.has_value());
  auto start_chunks =
      wh::compose::collect_graph_stream_reader(std::move(start_reader).value());
  REQUIRE(start_chunks.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&start_chunks.value().front()) ==
          "start");

  auto edge_reader = wh::testing::helper::read_graph_value<
      wh::compose::graph_stream_reader>(
      std::move(checkpoint.runtime.dag->edge_readers.front().value));
  REQUIRE(edge_reader.has_value());
  auto edge_chunks =
      wh::compose::collect_graph_stream_reader(std::move(edge_reader).value());
  REQUIRE(edge_chunks.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&edge_chunks.value().front()) ==
          "edge");
}
