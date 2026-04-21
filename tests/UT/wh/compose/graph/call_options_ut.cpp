#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/call_options.hpp"

TEST_CASE("graph call scope resolves paths designations and subscriptions",
          "[UT][wh/compose/graph/call_options.hpp][graph_call_scope::absolute_path][condition][branch][boundary]") {
  wh::compose::graph_call_options options{};
  options.designated_top_level_nodes = {"node-a"};
  options.designated_node_paths = {wh::compose::make_node_path({"root", "node-a"})};
  options.stream_subscriptions.push_back(
      {.kind = wh::compose::graph_stream_channel_kind::custom,
       .custom_channel = "trace",
       .enabled = true});

  auto scope =
      wh::compose::graph_call_scope::root(options).with_trace({.trace_id = "t"});
  REQUIRE(scope.trace()->trace_id == "t");

  auto absolute = scope.absolute_path(wh::compose::make_node_path({"root", "node-a"}));
  REQUIRE(absolute.to_string() == "root/node-a");

  auto relative = scope.relative_path(absolute);
  REQUIRE(relative.has_value());
  REQUIRE(relative->to_string() == "root/node-a");
  REQUIRE(scope
              .relative_path(wh::compose::make_node_path({"other", "node-a"}))
              .has_value());

  wh::compose::graph_call_scope nested_scope{
      options, wh::compose::make_node_path({"root"})};
  auto nested_relative = nested_scope.relative_path(
      wh::compose::make_node_path({"root", "node-a"}));
  REQUIRE(nested_relative.has_value());
  REQUIRE(nested_relative->to_string() == "node-a");
  REQUIRE_FALSE(nested_scope
                    .relative_path(
                        wh::compose::make_node_path({"other", "node-a"}))
                    .has_value());

  REQUIRE(wh::compose::has_graph_stream_subscription(
      scope, wh::compose::graph_stream_channel_kind::custom, "trace"));
  REQUIRE_FALSE(wh::compose::has_graph_stream_subscription(
      scope, wh::compose::graph_stream_channel_kind::custom, "other"));
  REQUIRE(wh::compose::is_graph_node_designated(
      options, "node-a", wh::compose::make_node_path({"root", "node-a"})));
  REQUIRE_FALSE(wh::compose::is_graph_node_designated(
      options, "node-b", wh::compose::make_node_path({"root", "node-b"})));
}

TEST_CASE("graph call options resolve debug emission interrupt policy and component overrides",
          "[UT][wh/compose/graph/call_options.hpp][resolve_external_interrupt_policy][condition][branch]") {
  wh::compose::graph_call_options options{};
  options.isolate_debug_stream = false;
  options.stream_subscriptions.push_back(
      {.kind = wh::compose::graph_stream_channel_kind::message, .enabled = true});
  options.interrupt_timeout = std::chrono::milliseconds{0};
  options.component_defaults.insert_or_assign("temperature",
                                              wh::compose::graph_value{0.1});
  options.component_overrides.push_back({
      .path = wh::compose::make_node_path({"root", "node"}),
      .values = {{"temperature", wh::compose::graph_value{0.5}},
                 {"label", wh::compose::graph_value{std::string{"hot"}}}},
  });

  auto scope = wh::compose::graph_call_scope::root(options);
  REQUIRE(wh::compose::should_emit_graph_debug_event(scope));

  auto resolved_policy = wh::compose::resolve_external_interrupt_policy(scope);
  REQUIRE(resolved_policy.timeout == std::optional<std::chrono::milliseconds>{
                                       std::chrono::milliseconds{0}});
  REQUIRE(resolved_policy.mode ==
          wh::compose::graph_interrupt_timeout_mode::immediate_rerun);

  auto resolved_options = wh::compose::resolve_graph_component_option_map(
      scope, wh::compose::make_node_path({"root", "node"}));
  REQUIRE(resolved_options.size() == 2U);
  REQUIRE(resolved_options.at("temperature").from_override);
  REQUIRE(*wh::core::any_cast<double>(&resolved_options.at("temperature").value) ==
          0.5);
  REQUIRE(*wh::core::any_cast<std::string>(&resolved_options.at("label").value) ==
          "hot");

  auto extracted_temperature =
      wh::compose::extract_graph_component_option<double>(resolved_options,
                                                          "temperature");
  REQUIRE(extracted_temperature.has_value());
  REQUIRE(extracted_temperature.value().has_value());
  REQUIRE(*extracted_temperature.value() == 0.5);

  auto wrong_type = wh::compose::extract_graph_component_option<int>(
      resolved_options, "temperature");
  REQUIRE(wrong_type.has_error());
  REQUIRE(wrong_type.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("graph call options freeze external interrupt policy once and dispatch matching debug observers",
          "[UT][wh/compose/graph/call_options.hpp][freeze_external_interrupt_policy][branch][boundary]") {
  wh::compose::graph_external_interrupt_policy_latch latch{};
  auto first = wh::compose::freeze_external_interrupt_policy(
      latch, wh::compose::graph_external_interrupt_policy{
                 .timeout = std::chrono::milliseconds{5}});
  auto second = wh::compose::freeze_external_interrupt_policy(
      latch, wh::compose::graph_external_interrupt_policy{
                 .timeout = std::chrono::milliseconds{99}});
  REQUIRE(first.timeout == std::optional<std::chrono::milliseconds>{
                               std::chrono::milliseconds{5}});
  REQUIRE(second.timeout == first.timeout);

  wh::compose::graph_call_options options{};
  int graph_hits = 0;
  int node_hits = 0;
  options.graph_debug_observer =
      [&graph_hits](const wh::compose::graph_debug_stream_event &,
                    wh::core::run_context &) { ++graph_hits; };
  options.node_path_debug_observers.push_back(
      {.path = wh::compose::make_node_path({"root", "node"}),
       .include_descendants = true,
       .callback = [&node_hits](const wh::compose::graph_debug_stream_event &,
                                wh::core::run_context &) { ++node_hits; }});

  wh::compose::graph_debug_stream_event event{
      .path = wh::compose::make_node_path({"root", "node", "child"})};
  wh::core::run_context context{};
  wh::compose::dispatch_graph_debug_observers(
      wh::compose::graph_call_scope::root(options), event, context);
  REQUIRE(graph_hits == 1);
  REQUIRE(node_hits == 1);
}
