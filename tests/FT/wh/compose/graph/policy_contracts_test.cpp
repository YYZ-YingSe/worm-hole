#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/authored.hpp"
#include "wh/compose/graph/dag.hpp"
#include "wh/compose/graph/policy.hpp"
#include "wh/compose/graph/pregel.hpp"

namespace {

using wh::testing::helper::invoke_graph_sync;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_int_add_node;
using wh::testing::helper::read_any;

} // namespace

TEST_CASE("compose graph executes runtime branch routing and enforces whitelist",
          "[core][compose][graph][branch]") {
  wh::compose::graph graph{};

  auto route = wh::compose::make_lambda_node(
      "route", [](wh::compose::graph_value &input, wh::core::run_context &,
                  const wh::compose::graph_call_scope &)
                   -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      });

  int left_count = 0;
  int right_count = 0;
  auto left = wh::compose::make_lambda_node(
      "left", [&left_count](const wh::compose::graph_value &input,
                            wh::core::run_context &,
                            const wh::compose::graph_call_scope &)
                  -> wh::core::result<wh::compose::graph_value> {
        ++left_count;
        auto typed = read_any<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              typed.error());
        }
        return wh::core::any(typed.value() + 10);
      });
  auto right = wh::compose::make_lambda_node(
      "right", [&right_count](const wh::compose::graph_value &input,
                              wh::core::run_context &,
                              const wh::compose::graph_call_scope &)
                   -> wh::core::result<wh::compose::graph_value> {
        ++right_count;
        auto typed = read_any<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              typed.error());
        }
        return wh::core::any(typed.value() + 100);
      });

  REQUIRE(graph.add_lambda(std::move(route)).has_value());
  REQUIRE(graph.add_lambda(std::move(left)).has_value());
  REQUIRE(graph.add_lambda(std::move(right)).has_value());
  REQUIRE(graph.add_entry_edge("route").has_value());
  REQUIRE(graph.add_exit_edge("left").has_value());
  REQUIRE(graph.add_exit_edge("right").has_value());
  auto left_id = graph.node_id("left");
  REQUIRE(left_id.has_value());
  auto right_id = graph.node_id("right");
  REQUIRE(right_id.has_value());
  REQUIRE(graph
              .add_value_branch(wh::compose::graph_value_branch{
                  .from = "route",
                  .end_nodes = {"left", "right"},
                  .selector_ids =
                      [left = left_id.value(), right = right_id.value()](
                          const wh::compose::graph_value &output,
                          wh::core::run_context &,
                          const wh::compose::graph_call_scope &)
                          -> wh::core::result<std::vector<std::uint32_t>> {
                    auto typed = read_any<int>(output);
                    if (typed.has_error()) {
                      return wh::core::result<std::vector<std::uint32_t>>::failure(
                          typed.error());
                    }
                    if ((typed.value() % 2) == 0) {
                      return std::vector<std::uint32_t>{left};
                    }
                    return std::vector<std::uint32_t>{right};
                  },
              })
              .has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto odd_out = invoke_value_sync(graph, wh::core::any(1), context);
  REQUIRE(odd_out.has_value());
  auto odd_value = read_any<wh::compose::graph_value_map>(odd_out.value());
  REQUIRE(odd_value.has_value());
  REQUIRE(odd_value.value().size() == 1U);
  auto odd_right = read_any<int>(odd_value.value().at("right"));
  REQUIRE(odd_right.has_value());
  REQUIRE(odd_right.value() == 101);
  REQUIRE(left_count == 0);
  REQUIRE(right_count == 1);

  auto even_out = invoke_value_sync(graph, wh::core::any(2), context);
  REQUIRE(even_out.has_value());
  auto even_value = read_any<wh::compose::graph_value_map>(even_out.value());
  REQUIRE(even_value.has_value());
  REQUIRE(even_value.value().size() == 1U);
  auto even_left = read_any<int>(even_value.value().at("left"));
  REQUIRE(even_left.has_value());
  REQUIRE(even_left.value() == 12);
  REQUIRE(left_count == 1);
  REQUIRE(right_count == 1);

  wh::compose::graph invalid_graph{};
  REQUIRE(invalid_graph.add_lambda(make_int_add_node("route", 0)).has_value());
  REQUIRE(invalid_graph.add_lambda(make_int_add_node("left", 1)).has_value());
  REQUIRE(invalid_graph.add_entry_edge("route").has_value());
  REQUIRE(invalid_graph.add_exit_edge("left").has_value());
  REQUIRE(invalid_graph
              .add_value_branch(wh::compose::graph_value_branch{
                  .from = "route",
                  .end_nodes = {"left"},
                  .selector_ids =
                      [](const wh::compose::graph_value &,
                         wh::core::run_context &,
                         const wh::compose::graph_call_scope &)
                          -> wh::core::result<std::vector<std::uint32_t>> {
                    return std::vector<std::uint32_t>{
                        std::numeric_limits<std::uint32_t>::max()};
                  },
              })
              .has_value());
  REQUIRE(invalid_graph.compile().has_value());
  auto invalid_invoked =
      invoke_value_sync(invalid_graph, wh::core::any(1), context);
  REQUIRE(invalid_invoked.has_error());
  REQUIRE(invalid_invoked.error() == wh::core::errc::contract_violation);
}

TEST_CASE("compose graph trigger mode controls readiness policy",
          "[core][compose][graph][condition]") {
  auto build_case = [](wh::compose::graph_trigger_mode trigger_mode)
      -> wh::compose::graph {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = trigger_mode;
    wh::compose::graph graph{std::move(options)};
    auto route = wh::compose::make_passthrough_node("route");
    auto left = make_int_add_node("left", 10);
    auto right = make_int_add_node("right", 20);
    auto target =
        wh::compose::make_lambda_node(
            "target",
            [](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_value> {
              if (auto merged = read_any<wh::compose::graph_value_map>(input);
                  merged.has_value()) {
                auto left_value = read_any<int>(merged.value().at("left"));
                if (left_value.has_error()) {
                  return wh::core::result<wh::compose::graph_value>::failure(
                      left_value.error());
                }
                return wh::core::any(left_value.value() + 1);
              }
              auto typed = read_any<int>(input);
              if (typed.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    typed.error());
              }
              return wh::core::any(typed.value() + 1);
            });
    REQUIRE(graph.add_passthrough(std::move(route)).has_value());
    REQUIRE(graph.add_lambda(std::move(left)).has_value());
    REQUIRE(graph.add_lambda(std::move(right)).has_value());
    REQUIRE(graph.add_lambda(std::move(target)).has_value());
    REQUIRE(graph.add_entry_edge("route").has_value());
    REQUIRE(graph.add_edge("left", "target").has_value());
    REQUIRE(graph.add_edge("right", "target").has_value());
    REQUIRE(graph.add_exit_edge("target").has_value());
    auto left_id = graph.node_id("left");
    REQUIRE(left_id.has_value());
    REQUIRE(graph
                .add_value_branch(wh::compose::graph_value_branch{
                    .from = "route",
                    .end_nodes = {"left", "right"},
                    .selector_ids =
                        [left = left_id.value()](
                            const wh::compose::graph_value &,
                            wh::core::run_context &,
                            const wh::compose::graph_call_scope &)
                            -> wh::core::result<std::vector<std::uint32_t>> {
                      return std::vector<std::uint32_t>{left};
                    },
                })
                .has_value());
    REQUIRE(graph.compile().has_value());
    return graph;
  };

  auto any_graph = build_case(wh::compose::graph_trigger_mode::any_predecessor);
  auto all_graph = build_case(wh::compose::graph_trigger_mode::all_predecessors);

  wh::core::run_context context{};
  auto any_out = invoke_value_sync(any_graph, wh::core::any(5), context);
  REQUIRE(any_out.has_value());
  auto any_value = read_any<int>(any_out.value());
  REQUIRE(any_value.has_value());
  REQUIRE(any_value.value() == 16);

  auto all_out = invoke_value_sync(all_graph, wh::core::any(5), context);
  REQUIRE(all_out.has_error());
  REQUIRE(all_out.error() == wh::core::errc::contract_violation);
}

TEST_CASE("compose graph fan-in policy require-all merges active inputs",
          "[core][compose][graph][condition]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};
  std::size_t merged_count = 0U;
  REQUIRE(graph
              .add_lambda("a", [](const wh::compose::graph_value &,
                                  wh::core::run_context &,
                                  const wh::compose::graph_call_scope &)
                            -> wh::core::result<wh::compose::graph_value> {
                return wh::core::any(10);
              })
              .has_value());
  REQUIRE(graph
              .add_lambda("b", [](const wh::compose::graph_value &,
                                  wh::core::run_context &,
                                  const wh::compose::graph_call_scope &)
                            -> wh::core::result<wh::compose::graph_value> {
                return wh::core::any(20);
              })
              .has_value());
  REQUIRE(graph
              .add_lambda("join",
                          [&merged_count](const wh::compose::graph_value &input,
                                          wh::core::run_context &,
                                          const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<wh::compose::graph_value_map>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            merged_count = typed.value().size();
                            auto left = read_any<int>(typed.value().at("a"));
                            auto right = read_any<int>(typed.value().at("b"));
                            if (left.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  left.error());
                            }
                            if (right.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  right.error());
                            }
                            return wh::core::any(left.value() + right.value());
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());
  REQUIRE(graph.add_entry_edge("b").has_value());
  REQUIRE(graph.add_edge("a", "join").has_value());
  REQUIRE(graph.add_edge("b", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(0), context);
  REQUIRE(invoked.has_value());
  REQUIRE(merged_count == 2U);
}

TEST_CASE("compose graph allow-partial preserves static value fan-in shape",
          "[core][compose][graph][condition]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::any_predecessor;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::allow_partial;
  wh::compose::graph graph{std::move(options)};

  auto route = wh::compose::make_passthrough_node("route");
  REQUIRE(graph.add_passthrough(std::move(route)).has_value());
  REQUIRE(graph.add_lambda(make_int_add_node("left", 10)).has_value());
  REQUIRE(graph.add_lambda(make_int_add_node("right", 20)).has_value());

  std::size_t merged_count = 0U;
  auto add_join = graph.add_lambda(
      "join",
      [&merged_count](const wh::compose::graph_value &input,
                      wh::core::run_context &,
                      const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto typed = read_any<wh::compose::graph_value_map>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              typed.error());
        }
        merged_count = typed.value().size();
        REQUIRE(typed.value().contains("left"));
        REQUIRE(!typed.value().contains("right"));
        auto left = read_any<int>(typed.value().at("left"));
        if (left.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              left.error());
        }
        return wh::core::any(left.value() + 1);
      });
  REQUIRE(add_join.has_value());

  REQUIRE(graph.add_entry_edge("route").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());

  auto left_id = graph.node_id("left");
  REQUIRE(left_id.has_value());
  REQUIRE(graph
              .add_value_branch(wh::compose::graph_value_branch{
                  .from = "route",
                  .end_nodes = {"left", "right"},
                  .selector_ids =
                      [left = left_id.value()](
                          const wh::compose::graph_value &, wh::core::run_context &,
                          const wh::compose::graph_call_scope &)
                          -> wh::core::result<std::vector<std::uint32_t>> {
                    return std::vector<std::uint32_t>{left};
                  },
              })
              .has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(5), context);
  REQUIRE(invoked.has_value());
  auto typed = read_any<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 16);
  REQUIRE(merged_count == 1U);
}

TEST_CASE("compose graph enforces dag-pregel mode constraints",
          "[core][compose][graph][branch]") {
  wh::compose::graph_compile_options dag_options{};
  wh::compose::dag dag_graph{dag_options};
  REQUIRE(dag_graph.add_lambda(make_int_add_node("a", 1)).has_value());
  REQUIRE(dag_graph.add_lambda(make_int_add_node("b", 1)).has_value());
  REQUIRE(dag_graph.add_entry_edge("a").has_value());
  REQUIRE(dag_graph.add_edge("a", "b").has_value());
  REQUIRE(dag_graph.add_edge("b", "a").has_value());
  REQUIRE(dag_graph.add_exit_edge("b").has_value());
  auto dag_compile = dag_graph.compile();
  REQUIRE(dag_compile.has_error());
  REQUIRE(dag_compile.error() == wh::core::errc::contract_violation);

  wh::compose::graph_compile_options pregel_options{};
  pregel_options.max_steps = 8U;
  wh::compose::pregel pregel_graph{pregel_options};
  REQUIRE(pregel_graph.add_lambda(make_int_add_node("a", 1)).has_value());
  REQUIRE(pregel_graph.add_lambda(make_int_add_node("b", 1)).has_value());
  REQUIRE(pregel_graph.add_entry_edge("a").has_value());
  REQUIRE(pregel_graph.add_edge("a", "b").has_value());
  REQUIRE(pregel_graph.add_edge("b", "a").has_value());
  REQUIRE(pregel_graph.add_exit_edge("b").has_value());
  REQUIRE(pregel_graph.compile().has_value());

  wh::compose::graph_compile_options pregel_budget_options{};
  pregel_budget_options.max_steps = 8U;
  wh::compose::pregel pregel_budget_graph{pregel_budget_options};
  REQUIRE(
      pregel_budget_graph.add_lambda(make_int_add_node("single", 1)).has_value());
  REQUIRE(pregel_budget_graph.add_entry_edge("single").has_value());
  REQUIRE(pregel_budget_graph.add_exit_edge("single").has_value());
  REQUIRE(pregel_budget_graph.compile().has_value());

  wh::core::run_context context{};
  wh::compose::graph_call_options call_options{};
  call_options.pregel_max_steps = static_cast<std::size_t>(1U);
  auto invoked =
      invoke_graph_sync(pregel_budget_graph, wh::core::any(1), context,
                        call_options, {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() == wh::core::errc::timeout);
  REQUIRE(invoked.value().report.step_limit_error.has_value());
  const auto &step_limit_detail = *invoked.value().report.step_limit_error;
  REQUIRE(step_limit_detail.budget == 1U);
  REQUIRE(step_limit_detail.step > step_limit_detail.budget);
  REQUIRE(std::find(step_limit_detail.completed_node_keys.begin(),
                    step_limit_detail.completed_node_keys.end(),
                    std::string{wh::compose::graph_start_node_key}) !=
          step_limit_detail.completed_node_keys.end());

  wh::compose::graph_call_options pregel_call_options{};
  pregel_call_options.pregel_max_steps = static_cast<std::size_t>(4U);
  auto invoked_with_call_options =
      invoke_value_sync(pregel_budget_graph, wh::core::any(1), context,
                        std::move(pregel_call_options));
  REQUIRE(invoked_with_call_options.has_value());
  auto called_output = read_any<int>(invoked_with_call_options.value());
  REQUIRE(called_output.has_value());
  REQUIRE(called_output.value() == 2);

  wh::compose::graph dag_budget_graph{};
  REQUIRE(dag_budget_graph.add_lambda(make_int_add_node("single", 1)).has_value());
  REQUIRE(dag_budget_graph.add_entry_edge("single").has_value());
  REQUIRE(dag_budget_graph.add_exit_edge("single").has_value());
  REQUIRE(dag_budget_graph.compile().has_value());
  wh::compose::graph_call_options dag_call_options{};
  dag_call_options.pregel_max_steps = static_cast<std::size_t>(2U);
  auto dag_invoked =
      invoke_value_sync(dag_budget_graph, wh::core::any(1), context,
                        std::move(dag_call_options));
  REQUIRE(dag_invoked.has_error());
  REQUIRE(dag_invoked.error() == wh::core::errc::contract_violation);
}

TEST_CASE("compose pregel runtime uses strict superstep barriers",
          "[core][compose][pregel][runtime]") {
  wh::compose::graph_compile_options options{};
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.max_steps = 8U;

  wh::compose::pregel pregel{options};
  REQUIRE(pregel.add_lambda(make_int_add_node("a", 1)).has_value());
  REQUIRE(pregel.add_lambda(make_int_add_node("b", 2)).has_value());
  REQUIRE(pregel
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          merged.error());
                    }
                    auto a = read_any<int>(merged.value().at("a"));
                    auto b = read_any<int>(merged.value().at("b"));
                    if (a.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          a.error());
                    }
                    if (b.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          b.error());
                    }
                    return wh::core::any(a.value() + b.value() + 3);
                  })
              .has_value());
  REQUIRE(pregel.add_entry_edge("a").has_value());
  REQUIRE(pregel.add_entry_edge("b").has_value());
  REQUIRE(pregel.add_edge("a", "join").has_value());
  REQUIRE(pregel.add_edge("b", "join").has_value());
  REQUIRE(pregel.add_exit_edge("join").has_value());
  REQUIRE(pregel.compile().has_value());

  wh::core::run_context context{};
  wh::compose::graph_call_options call_options{};
  call_options.record_transition_log = true;
  auto invoked =
      invoke_graph_sync(pregel, wh::core::any(1), context, call_options, {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 8);

  const auto find_enter_step = [&](const std::string_view node_key)
      -> std::optional<std::size_t> {
    const auto &events = invoked.value().report.transition_log;
    const auto iter = std::find_if(
        events.begin(), events.end(),
        [node_key](const wh::compose::graph_state_transition_event &event) {
          return event.kind == wh::compose::graph_state_transition_kind::node_enter &&
                 event.cause.node_key == node_key;
        });
    if (iter == events.end()) {
      return std::nullopt;
    }
    return iter->cause.step;
  };

  const auto a_step = find_enter_step("a");
  const auto b_step = find_enter_step("b");
  const auto join_step = find_enter_step("join");
  REQUIRE(a_step.has_value());
  REQUIRE(b_step.has_value());
  REQUIRE(join_step.has_value());
  REQUIRE(*a_step == 1U);
  REQUIRE(*b_step == 1U);
  REQUIRE(*join_step == 2U);
}

TEST_CASE("compose dag and pregel helpers enforce cycle policy",
          "[core][compose][dag][pregel][condition]") {
  wh::compose::dag dag_graph{};
  REQUIRE(dag_graph.add_lambda(make_int_add_node("a", 1)).has_value());
  REQUIRE(dag_graph.add_lambda(make_int_add_node("b", 1)).has_value());
  REQUIRE(dag_graph.add_entry_edge("a").has_value());
  REQUIRE(dag_graph.add_edge("a", "b").has_value());
  REQUIRE(dag_graph.add_edge("b", "a").has_value());
  REQUIRE(dag_graph.add_exit_edge("b").has_value());
  auto dag_compile = dag_graph.compile();
  REQUIRE(dag_compile.has_error());
  REQUIRE(dag_compile.error() == wh::core::errc::contract_violation);

  wh::compose::pregel pregel_graph{};
  REQUIRE(pregel_graph.add_lambda(make_int_add_node("a", 1)).has_value());
  REQUIRE(pregel_graph.add_lambda(make_int_add_node("b", 1)).has_value());
  REQUIRE(pregel_graph.add_entry_edge("a").has_value());
  REQUIRE(pregel_graph.add_edge("a", "b").has_value());
  REQUIRE(pregel_graph.add_edge("b", "a").has_value());
  REQUIRE(pregel_graph.add_exit_edge("b").has_value());
  REQUIRE(pregel_graph.compile().has_value());
}

TEST_CASE("compose graph applies retry budget",
          "[core][compose][graph][branch]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.retry_budget = 2U;
  wh::compose::graph graph{std::move(options)};
  int run_count = 0;
  REQUIRE(graph
              .add_lambda(
                  "flaky",
                  [&run_count](const wh::compose::graph_value &input,
                               wh::core::run_context &,
                               const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    ++run_count;
                    if (run_count <= 2) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          wh::core::errc::unavailable);
                    }
                    auto typed = read_any<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          typed.error());
                    }
                    return wh::core::any(typed.value() + 1);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("flaky").has_value());
  REQUIRE(graph.add_exit_edge("flaky").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto first = invoke_value_sync(graph, wh::core::any(7), context);
  REQUIRE(first.has_value());
  auto first_value = read_any<int>(first.value());
  REQUIRE(first_value.has_value());
  REQUIRE(first_value.value() == 8);
  REQUIRE(run_count == 3);

  auto second = invoke_value_sync(graph, wh::core::any(7), context);
  REQUIRE(second.has_value());
  auto second_value = read_any<int>(second.value());
  REQUIRE(second_value.has_value());
  REQUIRE(second_value.value() == 8);
  REQUIRE(run_count == 4);
}

TEST_CASE("compose graph supports node-level retry and timeout overrides",
          "[core][compose][graph][branch]") {
  SECTION("node retry override wins over graph default") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.retry_budget = 0U;
    wh::compose::graph graph{std::move(options)};
    int attempts = 0;
    wh::compose::graph_add_node_options node_options{};
    node_options.retry_budget_override = 2U;
    REQUIRE(graph
                .add_lambda(
                    "flaky",
                    [&attempts](const wh::compose::graph_value &input,
                                wh::core::run_context &,
                                const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      ++attempts;
                      if (attempts <= 2) {
                        return wh::core::result<wh::compose::graph_value>::failure(
                            wh::core::errc::unavailable);
                      }
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(
                            typed.error());
                      }
                      return wh::core::any(typed.value() + 1);
                    },
                    std::move(node_options))
                .has_value());
    REQUIRE(graph.add_entry_edge("flaky").has_value());
    REQUIRE(graph.add_exit_edge("flaky").has_value());
    REQUIRE(graph.compile().has_value());

    wh::core::run_context context{};
    auto invoked = invoke_value_sync(graph, wh::core::any(3), context);
    REQUIRE(invoked.has_value());
    auto typed = read_any<int>(invoked.value());
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == 4);
    REQUIRE(attempts == 3);
  }

  SECTION("node timeout override wins over graph default timeout") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.node_timeout = std::chrono::milliseconds{50};
    wh::compose::graph graph{std::move(options)};
    wh::compose::graph_add_node_options node_options{};
    node_options.timeout_override = std::chrono::milliseconds{5};
    REQUIRE(graph
                .add_lambda(
                    "slow",
                    [](wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      std::this_thread::sleep_for(std::chrono::milliseconds{20});
                      return std::move(input);
                    },
                    std::move(node_options))
                .has_value());
    REQUIRE(graph.add_entry_edge("slow").has_value());
    REQUIRE(graph.add_exit_edge("slow").has_value());
    REQUIRE(graph.compile().has_value());

    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::timeout);
    REQUIRE(invoked.value().report.node_timeout_error.has_value());
    const auto &timeout_detail = *invoked.value().report.node_timeout_error;
    REQUIRE(timeout_detail.node == "slow");
    REQUIRE(timeout_detail.timeout == std::chrono::milliseconds{5});
    REQUIRE(timeout_detail.elapsed >= std::chrono::milliseconds{5});
  }
}
