#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/call_options.hpp"

namespace {

using wh::testing::helper::invoke_graph_sync;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::read_any;

template <typename value_t>
[[nodiscard]] auto any_get(const wh::core::any &value) noexcept
    -> const value_t * {
  return wh::core::any_cast<value_t>(&value);
}

} // namespace

TEST_CASE("compose graph call options support downscope and resolved overlays",
          "[core][compose][graph][condition]") {
  wh::compose::graph_call_options options{};
  options.component_defaults.insert_or_assign("threshold", wh::core::any(1));
  options.component_defaults.insert_or_assign(
      "label", wh::core::any(std::string{"common"}));
  options.component_overrides.push_back(
      wh::compose::graph_component_override{
          .path = wh::compose::make_node_path({"child", "leaf"}),
          .values = wh::compose::graph_value_map{
              {"threshold", wh::core::any(9)},
          },
      });
  options.component_overrides.push_back(
      wh::compose::graph_component_override{
          .path = wh::compose::make_node_path({"other"}),
          .values = wh::compose::graph_value_map{
              {"threshold", wh::core::any(3)},
          },
      });
  options.node_path_debug_observers.push_back(
      wh::compose::graph_node_path_debug_callback{
          .path = wh::compose::make_node_path({"child", "leaf"}),
          .include_descendants = false,
          .callback = wh::compose::graph_debug_callback{
              [](const wh::compose::graph_debug_stream_event &,
                 wh::core::run_context &) {}},
      });

  const auto scoped = wh::compose::graph_call_scope{
      options, wh::compose::make_node_path({"child"})};
  auto relative_leaf =
      scoped.relative_path(wh::compose::make_node_path({"child", "leaf"}));
  REQUIRE(relative_leaf.has_value());
  REQUIRE(relative_leaf.value() == wh::compose::make_node_path({"leaf"}));
  auto other_path =
      scoped.relative_path(wh::compose::make_node_path({"other"}));
  REQUIRE_FALSE(other_path.has_value());

  const auto resolved = wh::compose::resolve_graph_component_values(
      scoped, wh::compose::make_node_path({"leaf"}));
  const auto threshold_iter = resolved.find("threshold");
  REQUIRE(threshold_iter != resolved.end());
  auto threshold = read_any<int>(threshold_iter->second);
  REQUIRE(threshold.has_value());
  REQUIRE(threshold.value() == 9);
  const auto label_iter = resolved.find("label");
  REQUIRE(label_iter != resolved.end());
  auto label = read_any<std::string>(label_iter->second);
  REQUIRE(label.has_value());
  REQUIRE(label.value() == "common");

  std::size_t observer_hits = 0U;
  options.node_path_debug_observers.front().callback =
      wh::compose::graph_debug_callback{
          [&observer_hits](const wh::compose::graph_debug_stream_event &,
                           wh::core::run_context &) { ++observer_hits; }};
  const auto dispatch_scope = wh::compose::graph_call_scope{
      options, wh::compose::make_node_path({"child"})};
  wh::core::run_context context{};
  wh::compose::dispatch_graph_debug_observers(
      dispatch_scope,
      wh::compose::graph_debug_stream_event{
          .decision = wh::compose::graph_debug_stream_event::decision_kind::enqueue,
          .node_key = "leaf",
          .path = wh::compose::make_node_path({"leaf"}),
          .step = 1U,
      },
      context);
  REQUIRE(observer_hits == 1U);
}

TEST_CASE("compose graph call options support designation stream and interrupt policy",
          "[core][compose][graph][condition]") {
  wh::compose::graph_call_options options{};
  options.designated_top_level_nodes.push_back("planner");
  options.designated_node_paths.push_back(
      wh::compose::make_node_path({"planner", "tool"}));
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::state_snapshot});
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::debug});
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::custom,
      .custom_channel = "metrics"});
  options.interrupt_timeout = std::chrono::milliseconds{0};

  const auto designation = wh::compose::resolve_graph_designation(
      options, "planner", wh::compose::make_node_path({"planner", "tool"}));
  REQUIRE(designation.top_level_hit);
  REQUIRE(designation.node_path_hit);
  REQUIRE(designation.matched());
  REQUIRE(wh::compose::has_graph_stream_subscription(
      options, wh::compose::graph_stream_channel_kind::debug));
  REQUIRE(wh::compose::has_graph_stream_subscription(
      options, wh::compose::graph_stream_channel_kind::custom, "metrics"));

  const auto scoped = wh::compose::graph_call_scope{
      options, wh::compose::make_node_path({"planner"})};
  auto relative_tool =
      scoped.relative_path(wh::compose::make_node_path({"planner", "tool"}));
  REQUIRE(relative_tool.has_value());
  REQUIRE(relative_tool.value() == wh::compose::make_node_path({"tool"}));
  const auto scoped_designation = wh::compose::resolve_graph_designation(
      scoped, "planner", wh::compose::make_node_path({"tool"}));
  REQUIRE_FALSE(scoped_designation.top_level_hit);
  REQUIRE(scoped_designation.node_path_hit);

  auto namespace_info = wh::compose::make_graph_event_scope(
      "risk_graph", "planner", wh::compose::make_node_path({"planner", "tool"}));
  REQUIRE(namespace_info.graph == "risk_graph");
  REQUIRE(namespace_info.node == "planner");
  REQUIRE(namespace_info.path == "planner/tool");

  auto resolved_policy = wh::compose::resolve_external_interrupt_policy(options);
  REQUIRE(resolved_policy.timeout.has_value());
  REQUIRE(resolved_policy.timeout.value() == std::chrono::milliseconds{0});
  REQUIRE(resolved_policy.mode ==
          wh::compose::graph_interrupt_timeout_mode::immediate_rerun);

  wh::compose::graph_external_interrupt_policy_latch latch{};
  wh::compose::graph_external_interrupt_policy first{};
  first.timeout = std::chrono::milliseconds{20};
  auto frozen = wh::compose::freeze_external_interrupt_policy(latch, first);
  REQUIRE(frozen.timeout.has_value());
  REQUIRE(frozen.timeout.value() == std::chrono::milliseconds{20});

  wh::compose::graph_external_interrupt_policy second{};
  second.timeout = std::chrono::milliseconds{5};
  auto refrozen = wh::compose::freeze_external_interrupt_policy(latch, second);
  REQUIRE(refrozen.timeout.has_value());
  REQUIRE(refrozen.timeout.value() == std::chrono::milliseconds{20});
}

TEST_CASE("compose graph component option extraction enforces scoped type semantics",
          "[core][compose][graph][condition]") {
  wh::compose::graph_call_options options{};
  options.component_defaults.insert_or_assign(
      "threshold", wh::core::any(std::string{"common-bad-type"}));
  options.component_overrides.push_back(
      wh::compose::graph_component_override{
          .path = wh::compose::make_node_path({"worker"}),
          .values = wh::compose::graph_value_map{
              {"threshold", wh::core::any(std::string{"targeted-bad-type"})},
              {"limit", wh::core::any(9)},
          },
      });

  const auto common_resolved = wh::compose::resolve_graph_component_option_map(
      options, wh::compose::make_node_path({"other"}));
  auto common_threshold =
      wh::compose::extract_graph_component_option<int>(common_resolved, "threshold");
  REQUIRE(common_threshold.has_value());
  REQUIRE_FALSE(common_threshold.value().has_value());

  const auto targeted_resolved = wh::compose::resolve_graph_component_option_map(
      options, wh::compose::make_node_path({"worker"}));
  auto targeted_threshold =
      wh::compose::extract_graph_component_option<int>(targeted_resolved, "threshold");
  REQUIRE(targeted_threshold.has_error());
  REQUIRE(targeted_threshold.error() == wh::core::errc::type_mismatch);

  auto targeted_limit =
      wh::compose::extract_graph_component_option<int>(targeted_resolved, "limit");
  REQUIRE(targeted_limit.has_value());
  REQUIRE(targeted_limit.value().has_value());
  REQUIRE(targeted_limit.value().value() == 9);
}

TEST_CASE("compose graph component option extraction reads option map",
          "[core][compose][graph][condition]") {
  wh::compose::graph_component_option_map resolved{};
  resolved.insert_or_assign(
      "limit", wh::compose::graph_component_option{
                   .value = wh::core::any(11),
                   .from_override = true,
               });

  auto limit = wh::compose::extract_graph_component_option<int>(resolved, "limit");
  REQUIRE(limit.has_value());
  REQUIRE(limit.value().has_value());
  REQUIRE(limit.value().value() == 11);

  auto missing = wh::compose::extract_graph_component_option<int>(resolved, "missing");
  REQUIRE(missing.has_value());
  REQUIRE_FALSE(missing.value().has_value());
}

TEST_CASE("compose graph passes call options directly to node lambdas",
          "[core][compose][graph][condition]") {
  wh::compose::graph graph{};
  auto reader = wh::compose::make_lambda_node(
      "reader",
      [](const wh::compose::graph_value &, wh::core::run_context &,
         const wh::compose::graph_call_scope &call_options)
          -> wh::core::result<wh::compose::graph_value> {
        if (call_options.interrupt_timeout() !=
            std::optional<std::chrono::milliseconds>{
                std::chrono::milliseconds{25}}) {
          return wh::core::result<wh::compose::graph_value>::failure(
              wh::core::errc::contract_violation);
        }
        auto resolved = wh::compose::resolve_graph_component_values(
            call_options, wh::compose::make_node_path({"reader"}));
        const auto threshold_iter = resolved.find("threshold");
        if (threshold_iter == resolved.end()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              wh::core::errc::not_found);
        }
        auto threshold = read_any<int>(threshold_iter->second);
        if (threshold.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              threshold.error());
        }
        return wh::core::any(threshold.value());
      });
  REQUIRE(graph.add_lambda(std::move(reader)).has_value());
  REQUIRE(graph.add_entry_edge("reader").has_value());
  REQUIRE(graph.add_exit_edge("reader").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  wh::compose::graph_call_options call_options{};
  call_options.component_defaults.insert_or_assign(
      "threshold", wh::core::any(2));
  call_options.component_overrides.push_back(
      wh::compose::graph_component_override{
          .path = wh::compose::make_node_path({"reader"}),
          .values = wh::compose::graph_value_map{
              {"threshold", wh::core::any(7)},
          },
      });
  call_options.interrupt_timeout = std::chrono::milliseconds{25};

  auto output =
      invoke_value_sync(graph, wh::core::any(0), context, std::move(call_options));
  REQUIRE(output.has_value());
  auto typed = read_any<int>(output.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 7);

  wh::core::run_context clean_context{};
  wh::compose::graph_call_options clean_call_options{};
  clean_call_options.component_defaults.insert_or_assign(
      "threshold", wh::core::any(5));
  clean_call_options.interrupt_timeout = std::chrono::milliseconds{25};
  auto clean_output = invoke_value_sync(graph, wh::core::any(0), clean_context,
                                        std::move(clean_call_options));
  REQUIRE(clean_output.has_value());
}

TEST_CASE("compose graph debug observers run without debug stream subscription",
          "[core][compose][graph][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input,
                     wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    return std::move(input);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  std::size_t global_hits = 0U;
  std::size_t scoped_hits = 0U;
  wh::compose::graph_call_options call_options{};
  call_options.graph_debug_observer = wh::compose::graph_debug_callback{
      [&global_hits](const wh::compose::graph_debug_stream_event &,
                     wh::core::run_context &) { ++global_hits; }};
  call_options.node_path_debug_observers.push_back(
      wh::compose::graph_node_path_debug_callback{
          .path = wh::compose::make_node_path({"worker"}),
          .include_descendants = false,
          .callback = wh::compose::graph_debug_callback{
              [&scoped_hits](const wh::compose::graph_debug_stream_event &event,
                             wh::core::run_context &) {
                if (event.node_key == "worker") {
                  ++scoped_hits;
                }
              }},
      });

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(3), context, call_options,
                                   {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto output = read_any<int>(invoked.value().output_status.value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 3);
  REQUIRE(global_hits > 0U);
  REQUIRE(scoped_hits > 0U);
  REQUIRE(invoked.value().report.debug_events.empty());
}

TEST_CASE("compose graph runtime projects nested node-path options into child graphs",
          "[core][compose][graph][condition]") {
  wh::compose::graph child{};
  REQUIRE(child
              .add_lambda("leaf",
                          [](const wh::compose::graph_value &,
                             wh::core::run_context &,
                             const wh::compose::graph_call_scope &call_options)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto resolved =
                                wh::compose::resolve_graph_component_values(
                                    call_options,
                                    wh::compose::make_node_path({"leaf"}));
                            const auto threshold_iter = resolved.find("threshold");
                            if (threshold_iter == resolved.end()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  wh::core::errc::not_found);
                            }
                            auto threshold = read_any<int>(threshold_iter->second);
                            if (threshold.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  threshold.error());
                            }
                            return wh::core::any(threshold.value());
                          })
              .has_value());
  REQUIRE(child.add_entry_edge("leaf").has_value());
  REQUIRE(child.add_exit_edge("leaf").has_value());
  REQUIRE(child.compile().has_value());

  wh::compose::graph parent{};
  REQUIRE(parent.add_subgraph("child", std::move(child)).has_value());
  REQUIRE(parent.add_entry_edge("child").has_value());
  REQUIRE(parent.add_exit_edge("child").has_value());
  REQUIRE(parent.compile().has_value());

  wh::compose::graph_call_options call_options{};
  call_options.component_overrides.push_back(
      wh::compose::graph_component_override{
          .path = wh::compose::make_node_path({"child", "leaf"}),
          .values = wh::compose::graph_value_map{
              {"threshold", wh::core::any(9)},
          },
      });

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(
      parent, wh::core::any(std::monostate{}), context,
      std::move(call_options));
  REQUIRE(invoked.has_value());
  auto typed = read_any<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 9);
}

TEST_CASE("compose graph start-branch selector reads run-scoped call options",
          "[core][compose][graph][branch]") {
  wh::compose::graph graph{};
  int left_count = 0;
  int right_count = 0;
  auto left = wh::compose::make_lambda_node(
      "left", [&left_count](const wh::compose::graph_value &,
                            wh::core::run_context &,
                            const wh::compose::graph_call_scope &)
                  -> wh::core::result<wh::compose::graph_value> {
        ++left_count;
        return wh::core::any(11);
      });
  auto right = wh::compose::make_lambda_node(
      "right", [&right_count](const wh::compose::graph_value &,
                              wh::core::run_context &,
                              const wh::compose::graph_call_scope &)
                   -> wh::core::result<wh::compose::graph_value> {
        ++right_count;
        return wh::core::any(22);
      });
  REQUIRE(graph.add_lambda(std::move(left)).has_value());
  REQUIRE(graph.add_lambda(std::move(right)).has_value());
  REQUIRE(graph.add_exit_edge("left").has_value());
  REQUIRE(graph.add_exit_edge("right").has_value());

  const auto left_id = graph.node_id("left");
  REQUIRE(left_id.has_value());
  const auto right_id = graph.node_id("right");
  REQUIRE(right_id.has_value());
  REQUIRE(graph
              .add_value_branch(wh::compose::graph_value_branch{
                  .from = std::string{wh::compose::graph_start_node_key},
                  .end_nodes = {"left", "right"},
                  .selector_ids =
                      [left = left_id.value(), right = right_id.value()](
                          const wh::compose::graph_value &,
                          wh::core::run_context &,
                          const wh::compose::graph_call_scope &call_options)
                          -> wh::core::result<std::vector<std::uint32_t>> {
                    auto resolved = wh::compose::resolve_graph_component_values(
                        call_options, wh::compose::make_node_path({"left"}));
                    const auto iter = resolved.find("route_left");
                    if (iter == resolved.end()) {
                      return wh::core::result<std::vector<std::uint32_t>>::failure(
                          wh::core::errc::not_found);
                    }
                    auto route_left = read_any<bool>(iter->second);
                    if (route_left.has_error()) {
                      return wh::core::result<std::vector<std::uint32_t>>::failure(
                          route_left.error());
                    }
                    if (route_left.value()) {
                      return std::vector<std::uint32_t>{left};
                    }
                    return std::vector<std::uint32_t>{right};
                  },
              })
              .has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_call_options call_options{};
  call_options.component_overrides.push_back(
      wh::compose::graph_component_override{
          .path = wh::compose::make_node_path({"left"}),
          .values = wh::compose::graph_value_map{
              {"route_left", wh::core::any(true)},
          },
      });

  wh::core::run_context context{};
  auto output =
      invoke_value_sync(graph, wh::core::any(0), context, std::move(call_options));
  REQUIRE(output.has_value());
  auto typed = read_any<wh::compose::graph_value_map>(output.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value().size() == 1U);
  auto left_value = read_any<int>(typed.value().at("left"));
  REQUIRE(left_value.has_value());
  REQUIRE(left_value.value() == 11);
  REQUIRE(left_count == 1);
  REQUIRE(right_count == 0);
}

TEST_CASE("compose graph runtime designation filters execution and emits debug events",
          "[core][compose][graph][condition]") {
  wh::compose::graph graph{};
  int left_count = 0;
  int right_count = 0;
  REQUIRE(graph
              .add_lambda("left",
                          [&left_count](const wh::compose::graph_value &,
                                        wh::core::run_context &,
                                        const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            ++left_count;
                            return wh::core::any(11);
                          })
              .has_value());
  REQUIRE(graph
              .add_lambda("right",
                          [&right_count](const wh::compose::graph_value &,
                                         wh::core::run_context &,
                                         const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            ++right_count;
                            return wh::core::any(22);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("left").has_value());
  REQUIRE(graph.add_entry_edge("right").has_value());
  REQUIRE(graph.add_exit_edge("left").has_value());
  REQUIRE(graph.add_exit_edge("right").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_call_options call_options{};
  call_options.designated_top_level_nodes.push_back("left");
  call_options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::debug});
  wh::core::run_context context{};
  auto invoked =
      invoke_graph_sync(graph, wh::core::any(std::monostate{}), context,
                        call_options, {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed =
      read_any<wh::compose::graph_value_map>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value().size() == 1U);
  auto left_value = read_any<int>(typed.value().at("left"));
  REQUIRE(left_value.has_value());
  REQUIRE(left_value.value() == 11);
  REQUIRE(left_count == 1);
  REQUIRE(right_count == 0);

  const auto &debug_events = invoked.value().report.debug_events;
  REQUIRE_FALSE(debug_events.empty());
  REQUIRE(std::any_of(
      debug_events.begin(), debug_events.end(),
      [](const wh::compose::graph_debug_stream_event &event) {
        return event.decision ==
                   wh::compose::graph_debug_stream_event::decision_kind::enqueue &&
               event.node_key == "left";
      }));
  REQUIRE(std::any_of(
      debug_events.begin(), debug_events.end(),
      [](const wh::compose::graph_debug_stream_event &event) {
        return event.decision ==
                   wh::compose::graph_debug_stream_event::decision_kind::skipped &&
               event.node_key == "right";
      }));
}

TEST_CASE("compose graph runtime validates call-options node targets",
          "[core][compose][graph][branch]") {
  wh::compose::graph graph{};
  int run_count = 0;
  REQUIRE(graph
              .add_lambda("worker",
                          [&run_count](wh::compose::graph_value &input,
                                       wh::core::run_context &,
                                       const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            ++run_count;
                            return std::move(input);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  SECTION("unknown top-level designation fails fast") {
    wh::compose::graph_call_options options{};
    options.designated_top_level_nodes.push_back("missing");
    wh::core::run_context context{};
    auto invoked =
        invoke_value_sync(graph, wh::core::any(1), context, std::move(options));
    REQUIRE(invoked.has_error());
    REQUIRE(invoked.error() == wh::core::errc::not_found);
    REQUIRE(run_count == 0);
  }

  SECTION("empty targeted path fails fast") {
    wh::compose::graph_call_options options{};
    options.component_overrides.push_back(
        wh::compose::graph_component_override{
            .path = wh::compose::node_path{},
            .values = {},
        });
    wh::core::run_context context{};
    auto invoked =
        invoke_value_sync(graph, wh::core::any(1), context, std::move(options));
    REQUIRE(invoked.has_error());
    REQUIRE(invoked.error() == wh::core::errc::invalid_argument);
    REQUIRE(run_count == 0);
  }

  SECTION("multi-segment designation path is accepted by top-level runtime") {
    wh::compose::graph_call_options options{};
    options.designated_node_paths.push_back(
        wh::compose::make_node_path({"worker", "child"}));
    wh::core::run_context context{};
    auto invoked =
        invoke_value_sync(graph, wh::core::any(1), context, std::move(options));
    REQUIRE(invoked.has_value());
    REQUIRE(run_count == 1);
  }

  SECTION("component default key with slash fails fast") {
    wh::compose::graph_call_options options{};
    options.component_defaults.insert_or_assign(
        "tool/timeout", wh::core::any(5));
    wh::core::run_context context{};
    auto invoked =
        invoke_value_sync(graph, wh::core::any(1), context, std::move(options));
    REQUIRE(invoked.has_error());
    REQUIRE(invoked.error() == wh::core::errc::invalid_argument);
    REQUIRE(run_count == 0);
  }

  SECTION("component override key with slash fails fast") {
    wh::compose::graph_call_options options{};
    options.component_overrides.push_back(
        wh::compose::graph_component_override{
            .path = wh::compose::make_node_path({"worker"}),
            .values = wh::compose::graph_value_map{
                {"tool/timeout", wh::core::any(5)},
            },
        });
    wh::core::run_context context{};
    auto invoked =
        invoke_value_sync(graph, wh::core::any(1), context, std::move(options));
    REQUIRE(invoked.has_error());
    REQUIRE(invoked.error() == wh::core::errc::invalid_argument);
    REQUIRE(run_count == 0);
  }
}
