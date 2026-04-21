#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/stream.hpp"

TEST_CASE("stream runtime emits debug and transition events only for subscribed channels",
          "[UT][wh/compose/graph/detail/runtime/"
          "stream.hpp][append_state_transition][condition][branch][boundary]") {
  wh::compose::graph_call_options options{};
  options.isolate_debug_stream = false;
  options.stream_subscriptions.push_back({.kind = wh::compose::graph_stream_channel_kind::debug,
                                          .custom_channel = {},
                                          .enabled = true});
  options.stream_subscriptions.push_back({.kind = wh::compose::graph_stream_channel_kind::message,
                                          .custom_channel = {},
                                          .enabled = true});
  options.stream_subscriptions.push_back({.kind = wh::compose::graph_stream_channel_kind::custom,
                                          .custom_channel = "audit",
                                          .enabled = true});

  std::size_t observed_steps = 0U;
  options.graph_debug_observer =
      [&observed_steps](const wh::compose::graph_debug_stream_event &event,
                        wh::core::run_context &) { observed_steps += event.step; };

  auto scope = wh::compose::graph_call_scope::root(options);
  auto path = wh::compose::make_node_path({"worker"});
  auto event_scope = wh::compose::make_graph_event_scope("graph", "worker", path);

  wh::core::run_context context{};
  wh::compose::detail::runtime_state::invoke_outputs outputs{};

  auto debug_event = wh::compose::graph_debug_stream_event{
      .decision = wh::compose::graph_debug_stream_event::decision_kind::enqueue,
      .node_key = "worker",
      .path = path,
      .step = 3U,
  };
  wh::compose::detail::stream_runtime::emit_debug_event(context, outputs, scope, debug_event,
                                                        event_scope);

  REQUIRE(observed_steps == 3U);
  REQUIRE(outputs.debug_events.size() == 1U);
  REQUIRE(outputs.runtime_message_events.size() == 1U);
  REQUIRE(outputs.runtime_message_events.front().text == "debug-decision");

  wh::compose::graph_transition_log log{};
  auto transition = wh::compose::graph_state_transition_event{
      .kind = wh::compose::graph_state_transition_kind::node_enter,
      .cause =
          wh::compose::graph_state_cause{
              .run_id = 9U,
              .step = 4U,
              .node_key = "worker",
          },
      .lifecycle = wh::compose::graph_node_lifecycle_state::running,
  };

  wh::compose::detail::stream_runtime::append_state_transition(
      log, outputs, scope, transition, event_scope, true, true, true, true, true);

  REQUIRE(log.size() == 1U);
  REQUIRE(outputs.state_snapshot_events.size() == 1U);
  REQUIRE(outputs.state_delta_events.size() == 1U);
  REQUIRE(outputs.custom_events.size() == 1U);
  REQUIRE(outputs.custom_events.front().channel == "audit");
  REQUIRE(outputs.runtime_message_events.size() == 2U);
  REQUIRE(outputs.runtime_message_events.back().text == "state-transition");
}

TEST_CASE("stream runtime ignores unsubscribed debug events and isolated message mirroring",
          "[UT][wh/compose/graph/detail/runtime/stream.hpp][emit_debug_event][condition][branch]") {
  wh::compose::graph_call_options options{};
  auto scope = wh::compose::graph_call_scope::root(options);

  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  auto path = wh::compose::make_node_path({"worker"});
  auto event_scope = wh::compose::make_graph_event_scope("graph", "worker", path);
  auto event = wh::compose::graph_debug_stream_event{
      .decision = wh::compose::graph_debug_stream_event::decision_kind::retry,
      .node_key = "worker",
      .path = path,
      .step = 8U,
  };

  wh::compose::detail::stream_runtime::append_debug_event(outputs, scope, event);
  REQUIRE(outputs.debug_events.empty());

  wh::core::run_context context{};
  wh::compose::detail::stream_runtime::emit_debug_event(context, outputs, scope, event,
                                                        event_scope);
  REQUIRE(outputs.debug_events.empty());
  REQUIRE(outputs.runtime_message_events.empty());
}
