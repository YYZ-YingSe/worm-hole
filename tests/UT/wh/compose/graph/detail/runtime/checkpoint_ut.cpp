#include <catch2/catch_test_macros.hpp>

#include <string>

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

TEST_CASE("runtime checkpoint helpers roundtrip stream rerun payload codecs for save and load",
          "[UT][wh/compose/graph/detail/runtime/checkpoint.hpp][apply_stream_codecs_for_save][condition][branch][boundary]") {
  using namespace wh::compose::detail::checkpoint_runtime;

  wh::compose::checkpoint_stream_codecs codecs{};
  const auto start_key = std::string{wh::compose::graph_start_node_key};
  codecs.emplace(start_key,
                 wh::compose::make_default_stream_codec());
  codecs.emplace("worker", wh::compose::make_default_stream_codec());

  wh::compose::checkpoint_state checkpoint{};
  checkpoint.rerun_inputs.emplace(
      start_key,
      wh::compose::graph_value{
          wh::compose::make_single_value_stream_reader(std::string{"start"})
              .value()});
  checkpoint.rerun_inputs.emplace(
      "worker",
      wh::compose::graph_value{
          wh::compose::make_single_value_stream_reader(std::string{"chunk"})
              .value()});

  wh::core::run_context context{};
  REQUIRE(apply_stream_codecs_for_save(checkpoint, context, &codecs).has_value());
  REQUIRE(wh::core::any_cast<wh::compose::checkpoint_stream_value_payload>(
              &checkpoint.rerun_inputs.at(start_key)) !=
          nullptr);
  REQUIRE(wh::core::any_cast<wh::compose::checkpoint_stream_value_payload>(
              &checkpoint.rerun_inputs.at("worker")) != nullptr);

  REQUIRE(apply_stream_codecs_for_load(checkpoint, context, &codecs).has_value());
  auto start_reader = wh::testing::helper::read_graph_value<
      wh::compose::graph_stream_reader>(
      std::move(checkpoint.rerun_inputs.at(start_key)));
  REQUIRE(start_reader.has_value());
  auto start_chunks =
      wh::compose::collect_graph_stream_reader(std::move(start_reader).value());
  REQUIRE(start_chunks.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&start_chunks.value().front()) ==
          "start");
}
