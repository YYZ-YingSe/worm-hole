#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/node.hpp"

namespace {

using wh::testing::helper::invoke_graph_sync;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_dual_scheduler_env;
using wh::testing::helper::make_graph_request;
using wh::testing::helper::make_int_add_node;
using wh::testing::helper::read_any;

} // namespace

TEST_CASE("compose graph transition log is opt-in", "[core][compose][state][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker")).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  REQUIRE(invoked.value().report.transition_log.empty());
}

TEST_CASE("compose graph unresolved predecessor stall publishes last completed node set",
          "[core][compose][graph][condition]") {
  wh::compose::graph_compile_options options{};
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;

  wh::compose::graph graph{options};
  REQUIRE(graph.add_lambda(make_int_add_node("blocked", 1)).has_value());
  REQUIRE(graph.add_lambda(make_int_add_node("worker", 1)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_edge("blocked", "worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);
  REQUIRE(invoked.value().report.completed_node_keys.size() == 1U);
  REQUIRE(invoked.value().report.completed_node_keys.front() == wh::compose::graph_start_node_key);
  REQUIRE(std::find(invoked.value().report.completed_node_keys.begin(),
                    invoked.value().report.completed_node_keys.end(),
                    std::string{wh::compose::graph_end_node_key}) ==
          invoked.value().report.completed_node_keys.end());
}

TEST_CASE("compose graph runtime emits subscribed state message and custom streams",
          "[core][compose][graph][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_call_options options{};
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::state_snapshot});
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::state_delta});
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::message});
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::custom, .custom_channel = "metrics"});
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::debug});

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(3), context, options, {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());

  REQUIRE_FALSE(invoked.value().report.state_snapshot_events.empty());
  REQUIRE_FALSE(invoked.value().report.state_delta_events.empty());
  REQUIRE_FALSE(invoked.value().report.runtime_message_events.empty());
  REQUIRE_FALSE(invoked.value().report.custom_events.empty());
  REQUIRE(std::any_of(
      invoked.value().report.custom_events.begin(), invoked.value().report.custom_events.end(),
      [](const wh::compose::graph_custom_event &event) { return event.channel == "metrics"; }));
}

TEST_CASE("compose graph nested stream events keep drilled node-path namespace",
          "[core][compose][graph][condition]") {
  wh::compose::graph child{};
  REQUIRE(child
              .add_lambda(
                  "leaf",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(child.add_entry_edge("leaf").has_value());
  REQUIRE(child.add_exit_edge("leaf").has_value());
  REQUIRE(child.compile().has_value());

  wh::compose::graph parent{};
  REQUIRE(parent.add_subgraph(wh::compose::make_subgraph_node("subgraph", std::move(child)))
              .has_value());
  REQUIRE(parent.add_entry_edge("subgraph").has_value());
  REQUIRE(parent.add_exit_edge("subgraph").has_value());
  REQUIRE(parent.compile().has_value());

  wh::compose::graph_call_options options{};
  options.stream_subscriptions.push_back(wh::compose::graph_stream_subscription{
      .kind = wh::compose::graph_stream_channel_kind::state_snapshot});

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(parent, wh::core::any(7), context, options, {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  REQUIRE(std::any_of(invoked.value().report.state_snapshot_events.begin(),
                      invoked.value().report.state_snapshot_events.end(),
                      [](const wh::compose::graph_state_snapshot_event &event) {
                        return event.scope.path == "subgraph/leaf";
                      }));
}

TEST_CASE("compose graph runtime publishes node run and graph run structured errors",
          "[core][compose][error][branch]") {
  SECTION("node run error detail includes path and raw error") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda("failer",
                            [](const wh::compose::graph_value &, wh::core::run_context &,
                               const wh::compose::graph_call_scope &)
                                -> wh::core::result<wh::compose::graph_value> {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  wh::core::errc::not_supported);
                            })
                .has_value());
    REQUIRE(graph.add_entry_edge("failer").has_value());
    REQUIRE(graph.add_exit_edge("failer").has_value());
    REQUIRE(graph.compile().has_value());

    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::not_supported);
    REQUIRE(invoked.value().report.node_run_error.has_value());
    const auto &detail = *invoked.value().report.node_run_error;
    REQUIRE(detail.node == "failer");
    REQUIRE(detail.code == wh::core::errc::not_supported);
    REQUIRE(detail.raw_error == wh::core::errc::not_supported);
    REQUIRE(detail.path.to_string().find("failer") != std::string::npos);
  }

  SECTION("canceled node run error is passed through without NodeRunError wrapping") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda("canceler",
                            [](const wh::compose::graph_value &, wh::core::run_context &,
                               const wh::compose::graph_call_scope &)
                                -> wh::core::result<wh::compose::graph_value> {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  wh::core::errc::canceled);
                            })
                .has_value());
    REQUIRE(graph.add_entry_edge("canceler").has_value());
    REQUIRE(graph.add_exit_edge("canceler").has_value());
    REQUIRE(graph.compile().has_value());

    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::canceled);
    REQUIRE_FALSE(invoked.value().report.node_run_error.has_value());
  }

  SECTION("async node timeout cooperatively requests stop for stop-aware sender") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.node_timeout = std::chrono::milliseconds{20};
    wh::compose::graph graph{std::move(options)};
    exec::static_thread_pool pool{2U};
    std::atomic<bool> started{false};
    std::atomic<bool> stop_observed{false};

    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                            wh::compose::node_exec_mode::async>(
                    "slow",
                    [&](const wh::compose::graph_value &, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
                      return stdexec::starts_on(
                          pool.get_scheduler(),
                          stdexec::read_env(stdexec::get_stop_token) |
                              stdexec::let_value([&](auto stop_token) {
                                started.store(true, std::memory_order_release);
                                started.notify_one();
                                while (!stop_token.stop_requested()) {
                                  std::this_thread::yield();
                                }
                                stop_observed.store(true, std::memory_order_release);
                                return stdexec::just(
                                    wh::core::result<wh::compose::graph_value>::failure(
                                        wh::core::errc::canceled));
                              }));
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("slow").has_value());
    REQUIRE(graph.add_exit_edge("slow").has_value());
    REQUIRE(graph.compile().has_value());

    auto wait_for_atomic_true = [](const std::atomic<bool> &flag,
                                   const std::chrono::milliseconds timeout) -> bool {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
          return true;
        }
        std::this_thread::yield();
      }
      return flag.load(std::memory_order_acquire);
    };

    wh::core::run_context context{};
    auto env = make_dual_scheduler_env(pool.get_scheduler(), pool.get_scheduler());
    using status_t = wh::core::result<wh::compose::graph_invoke_result>;
    wh::testing::helper::sender_capture<status_t> capture{};
    auto operation =
        stdexec::connect(graph.invoke(context, make_graph_request(wh::core::any(0))),
                         wh::testing::helper::sender_capture_receiver<status_t, decltype(env)>{
                             &capture,
                             env,
                         });
    stdexec::start(operation);

    REQUIRE(wait_for_atomic_true(started, std::chrono::milliseconds{2000}));
    REQUIRE(capture.ready.try_acquire_for(std::chrono::milliseconds{2000}));
    REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
    REQUIRE(capture.value.has_value());

    auto invoked = std::move(*capture.value);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::timeout);
    REQUIRE(wait_for_atomic_true(stop_observed, std::chrono::milliseconds{2000}));
  }

  SECTION("graph run error detail is emitted when step budget is exceeded") {
    wh::compose::graph_compile_options options{};
    options.max_steps = 1U;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph.add_lambda(make_int_add_node("n1", 1)).has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("n2", 1)).has_value());
    REQUIRE(graph.add_entry_edge("n1").has_value());
    REQUIRE(graph.add_edge("n1", "n2").has_value());
    REQUIRE(graph.add_exit_edge("n2").has_value());
    REQUIRE(graph.compile().has_value());

    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::timeout);
    REQUIRE(invoked.value().report.graph_run_error.has_value());
    REQUIRE(invoked.value().report.graph_run_error->phase ==
            wh::compose::compose_error_phase::schedule);
    REQUIRE(invoked.value().report.graph_run_error->code == wh::core::errc::timeout);
  }
}
