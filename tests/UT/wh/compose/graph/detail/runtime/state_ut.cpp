#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "wh/compose/graph/detail/runtime/state.hpp"

TEST_CASE("runtime state merge nested outputs appends streams and preserves first terminal detail",
          "[UT][wh/compose/graph/detail/runtime/state.hpp][merge_nested_outputs][condition][branch][boundary]") {
  using wh::compose::compose_error_phase;
  using wh::compose::detail::runtime_state::invoke_outputs;
  using wh::compose::detail::runtime_state::merge_nested_outputs;

  invoke_outputs target{};
  target.publish_transition_log = false;
  target.completed_node_keys = {"keep"};
  target.graph_run_error = wh::compose::graph_run_error_detail{
      .phase = compose_error_phase::execute,
      .node = "existing",
      .code = wh::core::errc::already_exists,
      .raw_error = wh::core::errc::already_exists,
      .message = "existing-error",
  };

  invoke_outputs nested{};
  nested.publish_transition_log = true;
  nested.transition_log.push_back(wh::compose::graph_state_transition_event{
      .kind = wh::compose::graph_state_transition_kind::node_enter,
      .cause = {.run_id = 1U, .step = 2U, .node_key = "worker"},
      .lifecycle = wh::compose::graph_node_lifecycle_state::running,
  });
  nested.debug_events.push_back(wh::compose::graph_debug_stream_event{
      .decision = wh::compose::graph_debug_stream_event::decision_kind::enqueue,
      .node_key = "worker",
      .path = wh::compose::make_node_path({"worker"}),
      .step = 2U,
  });
  nested.runtime_message_events.push_back(wh::compose::graph_runtime_message_event{
      .scope = wh::compose::make_graph_event_scope(
          "graph", "worker", wh::compose::make_node_path({"worker"})),
      .step = 2U,
      .text = "runtime-message",
  });
  nested.custom_events.push_back(wh::compose::graph_custom_event{
      .scope = wh::compose::make_graph_event_scope(
          "graph", "worker", wh::compose::make_node_path({"worker"})),
      .step = 2U,
      .channel = "audit",
      .payload = wh::compose::graph_value{7},
  });
  nested.completed_node_keys = {"nested"};
  nested.node_timeout_error = wh::compose::graph_node_timeout_error_detail{
      .node = "worker",
      .attempt = 1U,
      .timeout = std::chrono::milliseconds{5},
      .elapsed = std::chrono::milliseconds{9},
  };
  nested.checkpoint_error = wh::compose::checkpoint_error_detail{
      .code = wh::core::errc::invalid_argument,
      .checkpoint_id = "cp-1",
      .operation = "save",
  };

  merge_nested_outputs(target, std::move(nested));

  REQUIRE(target.publish_transition_log);
  REQUIRE(target.completed_node_keys == std::vector<std::string>{"keep"});
  REQUIRE(target.transition_log.size() == 1U);
  REQUIRE(target.debug_events.size() == 1U);
  REQUIRE(target.runtime_message_events.size() == 1U);
  REQUIRE(target.custom_events.size() == 1U);
  REQUIRE(target.node_timeout_error.has_value());
  REQUIRE(target.node_timeout_error->node == "worker");
  REQUIRE(target.graph_run_error.has_value());
  REQUIRE(target.graph_run_error->node == "existing");
  REQUIRE(target.checkpoint_error.has_value());
  REQUIRE(target.checkpoint_error->checkpoint_id == "cp-1");
}

TEST_CASE("runtime state carriers default initialize caches scopes and invoke config",
          "[UT][wh/compose/graph/detail/runtime/state.hpp][invoke_config][boundary]") {
  wh::compose::detail::runtime_state::invoke_config config{};
  REQUIRE(config.state_handlers == nullptr);
  REQUIRE(config.checkpoint_store == nullptr);
  REQUIRE(config.checkpoint_backend == nullptr);
  REQUIRE_FALSE(config.checkpoint_load.has_value());
  REQUIRE_FALSE(config.checkpoint_save.has_value());
  REQUIRE(config.reinterrupt_unmatched);
  REQUIRE(config.branch_merge == wh::compose::graph_branch_merge::set_union);

  wh::compose::detail::runtime_state::graph_trace_state trace{};
  REQUIRE(trace.trace_id.empty());
  REQUIRE(trace.parent_span_id.empty());
  REQUIRE(trace.graph_span_id.empty());
  REQUIRE(trace.next_span_sequence == 1U);

  wh::compose::detail::runtime_state::node_cache_state cache{};
  REQUIRE(cache.runtime_node_paths.empty());
  REQUIRE(cache.runtime_stream_scopes.empty());
  REQUIRE(cache.runtime_node_execution_addresses.empty());
  REQUIRE(cache.resolved_state_handlers.empty());
  REQUIRE_FALSE(cache.has_component_option_overrides);
  REQUIRE_FALSE(cache.emit_debug_events);
  REQUIRE_FALSE(cache.collect_transition_log);

  wh::compose::detail::runtime_state::node_scope scope{};
  REQUIRE(scope.path.empty());
  REQUIRE(scope.component_options == nullptr);
  REQUIRE(scope.observation == nullptr);
  REQUIRE(scope.local_process_state == nullptr);
}
