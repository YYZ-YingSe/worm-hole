#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/pregel_commit.hpp"
#include "wh/compose/graph/detail/start.hpp"
#include "wh/compose/runtime/interrupt.hpp"

TEST_CASE("pregel commit path preserves successful node output through full graph run",
          "[UT][wh/compose/graph/detail/"
          "pregel_commit.hpp][pregel_runtime::commit_node_output][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_commit");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(wh::compose::detail::invoke_runtime::start_pregel_run(
      wh::testing::helper::make_invoke_session(graph.value(), wh::compose::graph_value{31},
                                               context)));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*waited).value()) == 31);
}

TEST_CASE("pregel commit path preserves stream output through full graph run",
          "[UT][wh/compose/graph/detail/"
          "pregel_commit.hpp][pregel_runtime::commit_node_output][condition][branch][stream]") {
  auto graph = wh::testing::helper::make_runtime_stream_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_commit_stream");
  REQUIRE(graph.has_value());

  auto input = wh::compose::make_single_value_stream_reader(31);
  REQUIRE(input.has_value());

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(wh::compose::detail::invoke_runtime::start_pregel_run(
      wh::testing::helper::make_invoke_session(
          graph.value(), wh::compose::graph_value{std::move(input).value()}, context)));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());

  auto *reader =
      wh::core::any_cast<wh::compose::graph_stream_reader>(&std::get<0>(*waited).value());
  REQUIRE(reader != nullptr);

  auto collected = wh::compose::collect_graph_stream_reader(std::move(*reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
  auto *typed = wh::core::any_cast<int>(&collected.value().front());
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 31);
}

TEST_CASE("pregel commit path surfaces canceled when post-node interrupt hook fires",
          "[UT][wh/compose/graph/detail/"
          "pregel_commit.hpp][pregel_runtime::commit_node_output][condition][branch][error]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_commit_interrupt");
  REQUIRE(graph.has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(31);
  request.controls.interrupt.post_hook =
      [](std::string_view node_key, const wh::compose::graph_value &,
         wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
    return std::optional<wh::core::interrupt_signal>{
        wh::compose::make_interrupt_signal(std::string{"interrupt-"} + std::string{node_key},
                                           wh::core::make_address({"graph", "interrupt"}))};
  };

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph->invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  REQUIRE(status->output_status.has_error());
  REQUIRE(status->output_status.error() == wh::core::errc::canceled);
  REQUIRE(context.interrupt_info.has_value());
}

TEST_CASE(
    "pregel commit path propagates post-node interrupt hook failure",
    "[UT][wh/compose/graph/detail/"
    "pregel_commit.hpp][pregel_runtime::commit_node_output][condition][branch][boundary][error]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_commit_hook_error");
  REQUIRE(graph.has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(31);
  request.controls.interrupt.post_hook =
      [](std::string_view, const wh::compose::graph_value &,
         wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
    return wh::core::result<std::optional<wh::core::interrupt_signal>>::failure(
        wh::core::errc::timeout);
  };

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph->invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  REQUIRE(status->output_status.has_error());
  REQUIRE(status->output_status.error() == wh::core::errc::timeout);
  REQUIRE_FALSE(context.interrupt_info.has_value());
  REQUIRE_FALSE(status->report.node_run_error.has_value());
  REQUIRE_FALSE(status->report.graph_run_error.has_value());
}

TEST_CASE(
    "pregel commit path surfaces canceled when post-node interrupt hook fires on stream output",
    "[UT][wh/compose/graph/detail/"
    "pregel_commit.hpp][pregel_runtime::commit_node_output][condition][branch][stream][error]") {
  auto graph = wh::testing::helper::make_runtime_stream_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_commit_stream_interrupt");
  REQUIRE(graph.has_value());

  auto input = wh::compose::make_single_value_stream_reader(31);
  REQUIRE(input.has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::stream(std::move(input).value());
  request.controls.interrupt.post_hook =
      [](std::string_view node_key, const wh::compose::graph_value &,
         wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
    return std::optional<wh::core::interrupt_signal>{
        wh::compose::make_interrupt_signal(std::string{"interrupt-"} + std::string{node_key},
                                           wh::core::make_address({"graph", "interrupt"}))};
  };

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph->invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  REQUIRE(status->output_status.has_error());
  REQUIRE(status->output_status.error() == wh::core::errc::canceled);
  REQUIRE(context.interrupt_info.has_value());
}

TEST_CASE(
    "pregel commit path propagates post-node interrupt hook failure on stream output",
    "[UT][wh/compose/graph/detail/"
    "pregel_commit.hpp][pregel_runtime::commit_node_output][condition][branch][stream][boundary]"
    "[error]") {
  auto graph = wh::testing::helper::make_runtime_stream_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_commit_stream_hook_error");
  REQUIRE(graph.has_value());

  auto input = wh::compose::make_single_value_stream_reader(31);
  REQUIRE(input.has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::stream(std::move(input).value());
  request.controls.interrupt.post_hook =
      [](std::string_view, const wh::compose::graph_value &,
         wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
    return wh::core::result<std::optional<wh::core::interrupt_signal>>::failure(
        wh::core::errc::timeout);
  };

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph->invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  REQUIRE(status->output_status.has_error());
  REQUIRE(status->output_status.error() == wh::core::errc::timeout);
  REQUIRE_FALSE(context.interrupt_info.has_value());
  REQUIRE_FALSE(status->report.node_run_error.has_value());
  REQUIRE_FALSE(status->report.graph_run_error.has_value());
}
