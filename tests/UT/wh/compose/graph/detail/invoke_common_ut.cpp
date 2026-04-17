#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>

#include <exec/static_thread_pool.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/invoke_common.hpp"

TEST_CASE("invoke common trace token formatter keeps stable compact ids",
          "[UT][wh/compose/graph/detail/invoke_common.hpp][format_trace_token][condition][branch][boundary]") {
  REQUIRE(wh::compose::detail::format_trace_token("trace-", 7U) == "trace-7");
  REQUIRE(wh::compose::detail::format_trace_token("trace-", 7U, 3U) ==
          "trace-7.3");
  REQUIRE(wh::compose::detail::format_trace_token("", 42U, 1U) == "42.1");
}

TEST_CASE("invoke common runtime forwards call options into async node attempts",
          "[UT][wh/compose/graph/detail/invoke_common.hpp][invoke_session::bind_node_runtime_call_options][condition][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 1U;
  wh::compose::graph graph{std::move(options)};

  exec::static_thread_pool pool{2U};
  std::atomic<bool> saw_timeout{false};
  std::atomic<bool> saw_policy{false};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "worker",
                  [&](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &call_options) {
                    auto captured = call_options;
                    return stdexec::starts_on(
                        pool.get_scheduler(),
                        stdexec::just() |
                            stdexec::then([captured, &saw_timeout, &saw_policy]()
                                              -> wh::core::result<
                                                  wh::compose::graph_value> {
                              if (captured.interrupt_timeout() ==
                                  std::optional<std::chrono::milliseconds>{
                                      std::chrono::milliseconds{25}}) {
                                saw_timeout.store(true,
                                                  std::memory_order_release);
                              }
                              if (captured.external_interrupt_policy().has_value() &&
                                  captured.external_interrupt_policy()->mode ==
                                      wh::compose::graph_interrupt_timeout_mode::
                                          wait_inflight &&
                                  captured.external_interrupt_policy()->timeout ==
                                      std::optional<std::chrono::milliseconds>{
                                          std::chrono::milliseconds{40}}) {
                                saw_policy.store(true,
                                                 std::memory_order_release);
                              }
                              return wh::compose::graph_value{7};
                            }));
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  wh::compose::graph_call_options call_options{};
  call_options.interrupt_timeout = std::chrono::milliseconds{25};
  call_options.external_interrupt_policy =
      wh::compose::graph_external_interrupt_policy{
          .timeout = std::chrono::milliseconds{40},
          .mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight,
      };

  auto invoked = wh::testing::helper::invoke_value_sync(
      graph, wh::compose::graph_value{0}, context, std::move(call_options));
  REQUIRE(invoked.has_value());
  auto typed = wh::testing::helper::read_graph_value<int>(std::move(invoked).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 7);
  REQUIRE(saw_timeout.load(std::memory_order_acquire));
  REQUIRE(saw_policy.load(std::memory_order_acquire));
}
