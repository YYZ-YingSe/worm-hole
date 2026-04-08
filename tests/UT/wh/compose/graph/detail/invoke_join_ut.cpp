#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <exec/static_thread_pool.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/invoke_join.hpp"

TEST_CASE("invoke join serializes async branch launches under graph parallel limits",
          "[UT][wh/compose/graph/detail/invoke_join.hpp][invoke_join_base::start_child][condition][branch][boundary][concurrency]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 1U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  exec::static_thread_pool pool{2U};
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};

  auto update_max_active = [&]() noexcept -> void {
    auto observed = max_active.load(std::memory_order_acquire);
    const auto current = active.load(std::memory_order_acquire);
    while (observed < current &&
           !max_active.compare_exchange_weak(observed, current,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    }
  };

  auto make_branch = [&](const int value) {
    return [&, value](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &) {
      return stdexec::starts_on(
          pool.get_scheduler(),
          stdexec::just() |
              stdexec::then([&, value]() -> wh::core::result<
                                            wh::compose::graph_value> {
                struct guard {
                  std::atomic<int> *counter{nullptr};
                  ~guard() {
                    if (counter != nullptr) {
                      counter->fetch_sub(1, std::memory_order_acq_rel);
                    }
                  }
                };

                active.fetch_add(1, std::memory_order_acq_rel);
                guard current{std::addressof(active)};
                update_max_active();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                return wh::compose::graph_value{value};
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>("left",
                                                              make_branch(3))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>("right",
                                                              make_branch(4))
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged =
                        wh::testing::helper::read_graph_value<
                            wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          merged.error());
                    }
                    auto left = wh::testing::helper::read_graph_value<int>(
                        merged.value().at("left"));
                    auto right = wh::testing::helper::read_graph_value<int>(
                        merged.value().at("right"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          right.error());
                    }
                    return wh::compose::graph_value{left.value() + right.value()};
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("left").has_value());
  REQUIRE(graph.add_entry_edge("right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked =
      wh::testing::helper::invoke_value_sync(graph, wh::compose::graph_value{0},
                                             context);
  REQUIRE(invoked.has_value());
  auto typed = wh::testing::helper::read_graph_value<int>(std::move(invoked).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 7);
  REQUIRE(max_active.load(std::memory_order_acquire) == 1);
}

TEST_CASE("invoke join also allows parallel launches up to a wider graph limit",
          "[UT][wh/compose/graph/detail/invoke_join.hpp][invoke_join_base::drain_completions][condition][branch][concurrency]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  exec::static_thread_pool pool{2U};
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};

  auto update_max_active = [&]() noexcept -> void {
    auto observed = max_active.load(std::memory_order_acquire);
    const auto current = active.load(std::memory_order_acquire);
    while (observed < current &&
           !max_active.compare_exchange_weak(observed, current,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    }
  };

  auto make_branch = [&](const int value) {
    return [&, value](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &) {
      return stdexec::starts_on(
          pool.get_scheduler(),
          stdexec::just() |
              stdexec::then([&, value]() -> wh::core::result<
                                            wh::compose::graph_value> {
                struct guard {
                  std::atomic<int> *counter{nullptr};
                  ~guard() {
                    if (counter != nullptr) {
                      counter->fetch_sub(1, std::memory_order_acq_rel);
                    }
                  }
                };

                active.fetch_add(1, std::memory_order_acq_rel);
                guard current{std::addressof(active)};
                update_max_active();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                return wh::compose::graph_value{value};
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>("left",
                                                              make_branch(3))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>("right",
                                                              make_branch(4))
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged =
                        wh::testing::helper::read_graph_value<
                            wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          merged.error());
                    }
                    auto left = wh::testing::helper::read_graph_value<int>(
                        merged.value().at("left"));
                    auto right = wh::testing::helper::read_graph_value<int>(
                        merged.value().at("right"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          right.error());
                    }
                    return wh::compose::graph_value{left.value() + right.value()};
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("left").has_value());
  REQUIRE(graph.add_entry_edge("right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked =
      wh::testing::helper::invoke_value_sync(graph, wh::compose::graph_value{0},
                                             context);
  REQUIRE(invoked.has_value());
  auto typed = wh::testing::helper::read_graph_value<int>(std::move(invoked).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 7);
  REQUIRE(max_active.load(std::memory_order_acquire) == 2);
}
