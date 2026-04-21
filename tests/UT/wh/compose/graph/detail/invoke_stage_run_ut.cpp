#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/invoke_stage_run.hpp"

TEST_CASE(
    "invoke stage run retries node execution until graph retry budget is exhausted",
    "[UT][wh/compose/graph/detail/"
    "invoke_stage_run.hpp][invoke_stage_run::settle_node_stage][condition][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.retry_budget = 2U;
  wh::compose::graph graph{std::move(options)};

  int attempts = 0;
  REQUIRE(
      graph
          .add_lambda("flaky",
                      [&attempts](const wh::compose::graph_value &input, wh::core::run_context &,
                                  const wh::compose::graph_call_scope &)
                          -> wh::core::result<wh::compose::graph_value> {
                        ++attempts;
                        if (attempts <= 2) {
                          return wh::core::result<wh::compose::graph_value>::failure(
                              wh::core::errc::unavailable);
                        }
                        auto typed = wh::testing::helper::read_graph_value<int>(input);
                        if (typed.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                        }
                        return wh::compose::graph_value{typed.value() + 1};
                      })
          .has_value());
  REQUIRE(graph.add_entry_edge("flaky").has_value());
  REQUIRE(graph.add_exit_edge("flaky").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked =
      wh::testing::helper::invoke_value_sync(graph, wh::compose::graph_value{3}, context);
  REQUIRE(invoked.has_value());
  auto typed = wh::testing::helper::read_graph_value<int>(std::move(invoked).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 4);
  REQUIRE(attempts == 3);
}

TEST_CASE("invoke stage run honors node timeout overrides when settling execution output",
          "[UT][wh/compose/graph/detail/"
          "invoke_stage_run.hpp][invoke_stage_run::launch_node_stage][branch][timeout]") {
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
  auto invoked =
      wh::testing::helper::invoke_graph_sync(graph, wh::compose::graph_value{1}, context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked->output_status.has_error());
  REQUIRE(invoked->output_status.error() == wh::core::errc::timeout);
  REQUIRE(invoked->report.node_timeout_error.has_value());
  REQUIRE(invoked->report.node_timeout_error->node == "slow");
  REQUIRE(invoked->report.node_timeout_error->timeout == std::chrono::milliseconds{5});
}

TEST_CASE("invoke stage run preserves timeout overrides for inline-control sync nodes",
          "[UT][wh/compose/graph/detail/"
          "invoke_stage_run.hpp][invoke_stage_run::launch_node_stage][inline_control][timeout]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.node_timeout = std::chrono::milliseconds{50};
  wh::compose::graph graph{std::move(options)};

  wh::compose::graph_add_node_options node_options{};
  node_options.dispatch = wh::compose::sync_dispatch::inline_control;
  node_options.timeout_override = std::chrono::milliseconds{5};
  REQUIRE(graph
              .add_lambda(
                  "slow_inline",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    return std::move(input);
                  },
                  std::move(node_options))
              .has_value());
  REQUIRE(graph.add_entry_edge("slow_inline").has_value());
  REQUIRE(graph.add_exit_edge("slow_inline").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked =
      wh::testing::helper::invoke_graph_sync(graph, wh::compose::graph_value{1}, context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked->output_status.has_error());
  REQUIRE(invoked->output_status.error() == wh::core::errc::timeout);
  REQUIRE(invoked->report.node_timeout_error.has_value());
  REQUIRE(invoked->report.node_timeout_error->node == "slow_inline");
  REQUIRE(invoked->report.node_timeout_error->timeout == std::chrono::milliseconds{5});
}

TEST_CASE(
    "invoke stage run restores retained stream input before retrying a consumed node",
    "[UT][wh/compose/graph/detail/"
    "invoke_stage_run.hpp][invoke_stage_run::launch_node_from_retained_input][retry][stream]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.retry_budget = 1U;
  options.boundary = {
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::value,
  };
  wh::compose::graph graph{std::move(options)};

  int attempts = 0;
  auto add_worker =
      graph.add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
          "worker",
          [&attempts](
              wh::compose::graph_stream_reader input, wh::core::run_context &,
              const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
            auto chunks = wh::compose::collect_graph_stream_reader(std::move(input));
            if (chunks.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
            }
            ++attempts;
            if (attempts == 1) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::unavailable);
            }
            REQUIRE(chunks->size() == 1U);
            auto typed = wh::testing::helper::read_graph_value<std::string>(chunks->front());
            if (typed.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(typed.error());
            }
            return wh::compose::graph_value{typed.value() + "-replayed"};
          });
  REQUIRE(add_worker.has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto input_reader = wh::compose::make_single_value_stream_reader(std::string{"chunk"});
  REQUIRE(input_reader.has_value());

  wh::core::run_context context{};
  auto invoked = wh::testing::helper::invoke_graph_sync(
      graph, wh::compose::graph_value{std::move(input_reader).value()}, context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked->output_status.has_value());
  auto typed =
      wh::testing::helper::read_graph_value<std::string>(std::move(invoked->output_status).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "chunk-replayed");
  REQUIRE(attempts == 2);
}
