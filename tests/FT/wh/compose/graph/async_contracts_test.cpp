#include <catch2/catch_test_macros.hpp>

#include <exec/static_thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include "helper/async_coordination.hpp"
#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/compose/graph.hpp"

namespace {

using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_dual_scheduler_env;
using wh::testing::helper::make_graph_request;
using wh::testing::helper::read_any;

template <typename value_t>
auto require_value_capture(
    wh::testing::helper::sender_capture<value_t> &capture,
    const std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
    -> value_t {
  REQUIRE(capture.ready.try_acquire_for(timeout));
  REQUIRE(capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  return std::move(*capture.value);
}

} // namespace

TEST_CASE("compose graph dispatch policy controls frontier wave promotion",
          "[core][compose][graph][condition]") {
  auto wait_for_atomic_true =
      [](const std::atomic<bool> &flag,
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

  auto run_case =
      [&](const wh::compose::graph_dispatch_policy dispatch_policy) -> bool {
    exec::static_thread_pool launch_pool{1U};
    exec::static_thread_pool worker_pool{2U};
    std::atomic<bool> x_started{false};
    std::atomic<bool> x_released{false};
    std::atomic<bool> b_started{false};
    std::atomic<bool> b_started_before_release{false};
    auto release_x = std::make_shared<wh::testing::helper::async_event>();

    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.dispatch_policy = dispatch_policy;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    options.max_parallel_nodes = 2U;
    wh::compose::graph graph{std::move(options)};

    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value,
                            wh::compose::node_contract::value,
                            wh::compose::node_exec_mode::async>(
                    "a",
                    [&](const wh::compose::graph_value &, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
                      return stdexec::starts_on(
                          worker_pool.get_scheduler(),
                          stdexec::just() |
                              stdexec::then([] {
                                return wh::core::result<
                                    wh::compose::graph_value>{wh::core::any(1)};
                              }));
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value,
                            wh::compose::node_contract::value,
                            wh::compose::node_exec_mode::async>(
                    "x",
                    [&](const wh::compose::graph_value &, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
                      return stdexec::starts_on(
                          worker_pool.get_scheduler(),
                          stdexec::just() |
                              stdexec::then([&] {
                                x_started.store(true, std::memory_order_release);
                                x_started.notify_one();
                              }) |
                              stdexec::let_value(
                                  [release_x] { return release_x->wait(); }) |
                              stdexec::let_value([scheduler =
                                                      worker_pool.get_scheduler()] {
                                return stdexec::schedule(scheduler);
                              }) |
                              stdexec::then([]() -> wh::core::result<
                                                  wh::compose::graph_value> {
                                return wh::core::any(20);
                              }));
                    })
                .has_value());
    REQUIRE(graph.add_lambda(
                "b",
                [&](const wh::compose::graph_value &, wh::core::run_context &,
                    const wh::compose::graph_call_scope &)
                    -> wh::core::result<wh::compose::graph_value> {
                  const auto released =
                      x_released.load(std::memory_order_acquire);
                  b_started_before_release.store(!released,
                                                 std::memory_order_release);
                  b_started.store(true, std::memory_order_release);
                  b_started.notify_one();
                  return wh::core::any(10);
                })
                .has_value());
    REQUIRE(graph.add_lambda(
                "join",
                [](const wh::compose::graph_value &input, wh::core::run_context &,
                   const wh::compose::graph_call_scope &)
                    -> wh::core::result<wh::compose::graph_value> {
                  auto merged = read_any<wh::compose::graph_value_map>(input);
                  if (merged.has_error()) {
                    return wh::core::result<wh::compose::graph_value>::failure(
                        merged.error());
                  }
                  auto x_value = read_any<int>(merged.value().at("x"));
                  auto b_value = read_any<int>(merged.value().at("b"));
                  if (x_value.has_error()) {
                    return wh::core::result<wh::compose::graph_value>::failure(
                        x_value.error());
                  }
                  if (b_value.has_error()) {
                    return wh::core::result<wh::compose::graph_value>::failure(
                        b_value.error());
                  }
                  return wh::core::any(x_value.value() + b_value.value());
                })
                .has_value());
    REQUIRE(graph.add_entry_edge("a").has_value());
    REQUIRE(graph.add_entry_edge("x").has_value());
    REQUIRE(graph.add_edge("a", "b").has_value());
    REQUIRE(graph.add_edge("x", "join").has_value());
    REQUIRE(graph.add_edge("b", "join").has_value());
    REQUIRE(graph.add_exit_edge("join").has_value());
    REQUIRE(graph.compile().has_value());

    wh::core::run_context context{};
    using status_t = wh::core::result<wh::compose::graph_invoke_result>;
    auto env = make_dual_scheduler_env(launch_pool.get_scheduler(),
                                       launch_pool.get_scheduler());
    wh::testing::helper::sender_capture<status_t> capture{};
    auto op = stdexec::connect(
        graph.invoke(context, make_graph_request(wh::core::any(1))),
        wh::testing::helper::sender_capture_receiver<status_t, decltype(env)>{
            &capture,
            env,
        });
    stdexec::start(op);

    x_started.wait(false, std::memory_order_acquire);

    if (dispatch_policy == wh::compose::graph_dispatch_policy::same_wave) {
      REQUIRE(wait_for_atomic_true(b_started, std::chrono::milliseconds{250}));
      REQUIRE(b_started_before_release.load(std::memory_order_acquire));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      REQUIRE(!b_started.load(std::memory_order_acquire));
    }

    x_released.store(true, std::memory_order_release);
    release_x->signal();

    auto status = require_value_capture(capture);
    REQUIRE(status.has_value());
    REQUIRE(status.value().output_status.has_value());
    auto output = read_any<int>(status.value().output_status.value());
    REQUIRE(output.has_value());
    REQUIRE(output.value() == 30);
    REQUIRE(b_started.load(std::memory_order_acquire));
    return b_started_before_release.load(std::memory_order_acquire);
  };

  REQUIRE(run_case(wh::compose::graph_dispatch_policy::same_wave));
  REQUIRE(!run_case(wh::compose::graph_dispatch_policy::next_wave));
}

TEST_CASE("compose dag executes ready async branches concurrently",
          "[core][compose][dag][async]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};
  exec::static_thread_pool pool{2U};
  std::atomic<int> started{0};
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
                       struct active_guard {
                         std::atomic<int> *counter{nullptr};
                         ~active_guard() {
                           if (counter != nullptr) {
                             counter->fetch_sub(1, std::memory_order_acq_rel);
                           }
                         }
                       };

                       active.fetch_add(1, std::memory_order_acq_rel);
                       active_guard guard{std::addressof(active)};
                       update_max_active();
                       started.fetch_add(1, std::memory_order_acq_rel);
                       const auto deadline = std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds{50};
                       while (started.load(std::memory_order_acquire) < 2 &&
                              std::chrono::steady_clock::now() < deadline) {
                         std::this_thread::yield();
                       }
                       if (started.load(std::memory_order_acquire) < 2) {
                         return wh::core::result<wh::compose::graph_value>::failure(
                             wh::core::errc::contract_violation);
                       }
                       return wh::core::any(value);
                     }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "left", make_branch(10))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "right", make_branch(20))
              .has_value());
  REQUIRE(graph
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
                    auto left = read_any<int>(merged.value().at("left"));
                    auto right = read_any<int>(merged.value().at("right"));
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
  REQUIRE(graph.add_entry_edge("left").has_value());
  REQUIRE(graph.add_entry_edge("right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto status = invoke_value_sync(graph, wh::core::any(0), context);
  REQUIRE(status.has_value());
  auto output = read_any<int>(status.value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 30);
  REQUIRE(max_active.load(std::memory_order_acquire) >= 2);
}

TEST_CASE("compose dag repeated async branch invoke remains stable",
          "[core][compose][dag][async]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};
  exec::static_thread_pool pool{2U};

  auto make_branch = [&](const int value) {
    return [&, value](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &) {
      return stdexec::starts_on(
          pool.get_scheduler(),
          stdexec::just(wh::core::any(value)) |
              stdexec::then([](wh::compose::graph_value payload)
                                -> wh::core::result<wh::compose::graph_value> {
                return payload;
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "left", make_branch(10))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "right", make_branch(20))
              .has_value());
  REQUIRE(graph
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
                    auto left = read_any<int>(merged.value().at("left"));
                    auto right = read_any<int>(merged.value().at("right"));
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
  REQUIRE(graph.add_entry_edge("left").has_value());
  REQUIRE(graph.add_entry_edge("right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  for (std::size_t iteration = 0; iteration < 256; ++iteration) {
    auto status = invoke_value_sync(graph, wh::core::any(0), context);
    REQUIRE(status.has_value());
    auto output = read_any<int>(status.value());
    REQUIRE(output.has_value());
    REQUIRE(output.value() == 30);
  }
}

TEST_CASE("compose dag outer stop completes async execution exactly once",
          "[core][compose][dag][async]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 1U;
  wh::compose::graph graph{std::move(options)};
  exec::static_thread_pool pool{2U};
  std::atomic<bool> started{false};
  std::atomic<bool> stop_observed{false};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "worker",
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
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  stdexec::inplace_stop_source stop_source{};
  auto env = make_dual_scheduler_env(pool.get_scheduler(), pool.get_scheduler(),
                                     stop_source.get_token());
  using status_t = wh::core::result<wh::compose::graph_invoke_result>;
  wh::testing::helper::sender_capture<status_t> capture{};
  auto op = stdexec::connect(
      graph.invoke(context, make_graph_request(wh::core::any(1))),
      wh::testing::helper::sender_capture_receiver<status_t, decltype(env)>{
          &capture,
          env,
      });
  stdexec::start(op);

  started.wait(false, std::memory_order_acquire);
  stop_source.request_stop();

  auto status = require_value_capture(capture);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds{500};
  while (!stop_observed.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  REQUIRE(status.has_value());
  REQUIRE(status.value().output_status.has_error());
  REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);
  REQUIRE(stop_observed.load(std::memory_order_acquire));
}

TEST_CASE("compose dag async invoke passes call options directly to async lambda",
          "[core][compose][dag][async]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 1U;
  wh::compose::graph graph{std::move(options)};
  exec::static_thread_pool pool{2U};
  std::atomic<bool> saw_call_options{false};
  std::atomic<bool> saw_timeout{false};
  std::atomic<bool> saw_policy{false};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "worker",
                  [&](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &call_options) {
                    auto captured_options = call_options;
                    return stdexec::starts_on(
                        pool.get_scheduler(),
                        stdexec::just() |
                            stdexec::then([captured_options, &saw_call_options,
                                           &saw_timeout, &saw_policy]()
                                              -> wh::core::result<
                                                  wh::compose::graph_value> {
                              saw_call_options.store(true,
                                                     std::memory_order_release);
                              if (captured_options.interrupt_timeout() !=
                                  std::optional<std::chrono::milliseconds>{
                                      std::chrono::milliseconds{25}}) {
                                return wh::core::result<
                                    wh::compose::graph_value>::failure(
                                    wh::core::errc::contract_violation);
                              }
                              saw_timeout.store(true,
                                                std::memory_order_release);
                              if (!captured_options.external_interrupt_policy()
                                       .has_value() ||
                                  captured_options.external_interrupt_policy()
                                          ->mode !=
                                      wh::compose::graph_interrupt_timeout_mode::
                                          wait_inflight ||
                                  captured_options.external_interrupt_policy()
                                          ->timeout !=
                                      std::optional<std::chrono::milliseconds>{
                                          std::chrono::milliseconds{40}}) {
                                return wh::core::result<
                                    wh::compose::graph_value>::failure(
                                    wh::core::errc::contract_violation);
                              }
                              saw_policy.store(true,
                                               std::memory_order_release);
                              return wh::core::any(7);
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
  auto invoked =
      invoke_value_sync(graph, wh::core::any(0), context,
                        std::move(call_options));
  REQUIRE(invoked.has_value());
  auto output = read_any<int>(invoked.value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 7);
  REQUIRE(saw_call_options.load(std::memory_order_acquire));
  REQUIRE(saw_timeout.load(std::memory_order_acquire));
  REQUIRE(saw_policy.load(std::memory_order_acquire));
}

TEST_CASE("compose dag respects max_parallel_nodes when launching async branches",
          "[core][compose][dag][condition]") {
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
                       struct active_guard {
                         std::atomic<int> *counter{nullptr};
                         ~active_guard() {
                           if (counter != nullptr) {
                             counter->fetch_sub(1, std::memory_order_acq_rel);
                           }
                         }
                       };

                       active.fetch_add(1, std::memory_order_acq_rel);
                       active_guard guard{std::addressof(active)};
                       update_max_active();
                       std::this_thread::sleep_for(std::chrono::milliseconds{10});
                       return wh::core::any(value);
                     }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "left", make_branch(3))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "right", make_branch(4))
              .has_value());
  REQUIRE(graph
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
                    auto left = read_any<int>(merged.value().at("left"));
                    auto right = read_any<int>(merged.value().at("right"));
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
  REQUIRE(graph.add_entry_edge("left").has_value());
  REQUIRE(graph.add_entry_edge("right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto status = invoke_value_sync(graph, wh::core::any(0), context);
  REQUIRE(status.has_value());
  auto output = read_any<int>(status.value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 7);
  REQUIRE(max_active.load(std::memory_order_acquire) == 1);
}
