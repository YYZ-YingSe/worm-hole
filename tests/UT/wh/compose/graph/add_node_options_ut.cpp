#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/add_node_options.hpp"

TEST_CASE("graph add node state options require and bind phases while exposing metadata",
          "[UT][wh/compose/graph/add_node_options.hpp][graph_node_state_options::metadata][condition][branch][boundary]") {
  wh::compose::graph_node_state_options state{};
  REQUIRE_FALSE(state.any());
  REQUIRE(state.authored_handlers() == nullptr);

  state.bind_pre<int>([](const wh::compose::graph_state_cause &,
                         wh::compose::graph_process_state &, int &value,
                         wh::core::run_context &) -> wh::core::result<void> {
    ++value;
    return {};
  });
  state.require_stream_post();
  REQUIRE(state.any());

  const auto metadata = state.metadata();
  REQUIRE(metadata.pre);
  REQUIRE_FALSE(metadata.post);
  REQUIRE_FALSE(metadata.stream_pre);
  REQUIRE(metadata.stream_post);
  REQUIRE(state.authored_handlers() != nullptr);
  REQUIRE(state.pre().active());
  REQUIRE(state.stream_post().active());
  REQUIRE_FALSE(state.post().active());

  wh::compose::graph_add_node_options options{};
  options.name = "node";
  options.state = state;
  REQUIRE(options.name == "node");
  REQUIRE(options.state.metadata().pre);
}

TEST_CASE("graph add node state options typed handlers accept matching payloads and reject mismatches",
          "[UT][wh/compose/graph/add_node_options.hpp][graph_node_state_options::bind_pre][condition][branch]") {
  wh::compose::graph_node_state_options state{};
  state.bind_pre<int>([](const wh::compose::graph_state_cause &cause,
                         wh::compose::graph_process_state &, int &value,
                         wh::core::run_context &) -> wh::core::result<void> {
    value += static_cast<int>(cause.step);
    return {};
  });

  auto *handlers = state.authored_handlers();
  REQUIRE(handlers != nullptr);
  REQUIRE(static_cast<bool>(handlers->pre));

  wh::compose::graph_state_cause cause{.step = 3U};
  wh::compose::graph_process_state process{};
  wh::core::run_context context{};

  wh::compose::graph_value good_payload{4};
  auto good_status = handlers->pre(cause, process, good_payload, context);
  REQUIRE(good_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&good_payload) == 7);

  wh::compose::graph_value bad_payload{std::string{"oops"}};
  auto bad_status = handlers->pre(cause, process, bad_payload, context);
  REQUIRE(bad_status.has_error());
  REQUIRE(bad_status.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("graph add node options store observation overrides and compile snapshot metadata",
          "[UT][wh/compose/graph/add_node_options.hpp][graph_add_node_options][branch][boundary]") {
  wh::compose::graph_add_node_options options{};
  options.name = "node";
  options.type = "lambda";
  options.input_key = "input";
  options.output_key = "output";
  options.observation.callbacks_enabled = false;
  options.observation.allow_invoke_override = false;
  options.label = "Node Label";
  options.allow_no_control = true;
  options.allow_no_data = true;
  options.sync_dispatch = wh::compose::sync_dispatch::inline_control;
  options.retry_budget_override = 2U;
  options.max_parallel_override = 4U;
  options.subgraph_compile_info = wh::compose::graph_compile_info{.name = "sub"};

  REQUIRE(options.name == "node");
  REQUIRE(options.type == "lambda");
  REQUIRE(options.input_key == "input");
  REQUIRE(options.output_key == "output");
  REQUIRE_FALSE(options.observation.callbacks_enabled);
  REQUIRE_FALSE(options.observation.allow_invoke_override);
  REQUIRE(options.label == "Node Label");
  REQUIRE(options.allow_no_control);
  REQUIRE(options.allow_no_data);
  REQUIRE(options.sync_dispatch == wh::compose::sync_dispatch::inline_control);
  REQUIRE(options.retry_budget_override == std::optional<std::size_t>{2U});
  REQUIRE(options.max_parallel_override == std::optional<std::size_t>{4U});
  REQUIRE(options.subgraph_compile_info.has_value());
  REQUIRE(options.subgraph_compile_info->name == "sub");
}
