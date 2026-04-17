#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

#include <exec/static_thread_pool.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "helper/manual_scheduler.hpp"
#include "wh/compose/graph/detail/invoke_join.hpp"

namespace {

struct invoke_join_receiver_state {
  int value_count{0};
  int error_count{0};
  int stopped_count{0};
  std::optional<wh::core::result<wh::compose::graph_value>> result{};
};

struct invoke_join_receiver {
  using receiver_concept = stdexec::receiver_t;

  invoke_join_receiver_state *state{nullptr};

  auto set_value(wh::core::result<wh::compose::graph_value> result_value) && noexcept
      -> void {
    ++state->value_count;
    state->result.emplace(std::move(result_value));
  }

  template <typename error_t> auto set_error(error_t &&) && noexcept -> void {
    ++state->error_count;
  }

  auto set_stopped() && noexcept -> void { ++state->stopped_count; }

  [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
};

struct throwing_scheduler_state {
  std::size_t connect_calls{0U};
  std::optional<std::size_t> fail_on_connect{};
};

struct throwing_scheduler {
  using scheduler_concept = stdexec::scheduler_t;

  template <typename receiver_t> struct schedule_op {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;

    auto start() noexcept -> void { stdexec::set_value(std::move(receiver)); }
  };

  struct schedule_sender {
    throwing_scheduler_state *state{nullptr};

    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename receiver_t>
    auto connect(receiver_t receiver) const -> schedule_op<receiver_t> {
      ++state->connect_calls;
      if (state->fail_on_connect.has_value() &&
          state->connect_calls == *state->fail_on_connect) {
        throw std::runtime_error("schedule connect failed");
      }
      return schedule_op<receiver_t>{std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  throwing_scheduler_state *state{nullptr};

  [[nodiscard]] auto schedule() const noexcept -> schedule_sender {
    return schedule_sender{state};
  }

  [[nodiscard]] auto operator==(const throwing_scheduler &) const noexcept
      -> bool = default;
};

class invoke_join_probe
    : public wh::compose::detail::invoke_runtime::invoke_join_base<
          invoke_join_receiver, invoke_join_probe, throwing_scheduler> {
  using base_t = wh::compose::detail::invoke_runtime::invoke_join_base<
      invoke_join_receiver, invoke_join_probe, throwing_scheduler>;

public:
  explicit invoke_join_probe(throwing_scheduler scheduler,
                             invoke_join_receiver receiver)
      : base_t(0U, std::move(scheduler), std::move(receiver)) {}

  auto prepare_finish_delivery() noexcept -> void {}

  auto resume() noexcept -> void {
    if (this->terminal_pending()) {
      return;
    }
    ++resume_calls;
    this->enter_terminal(wh::core::result<wh::compose::graph_value>{
        wh::compose::graph_value{7}});
  }

  int resume_calls{0};
};

class invoke_join_edge_probe
    : public wh::compose::detail::invoke_runtime::invoke_join_base<
          invoke_join_receiver, invoke_join_edge_probe,
          wh::testing::helper::manual_scheduler<void>> {
  using scheduler_t = wh::testing::helper::manual_scheduler<void>;
  using base_t = wh::compose::detail::invoke_runtime::invoke_join_base<
      invoke_join_receiver, invoke_join_edge_probe, scheduler_t>;

public:
  explicit invoke_join_edge_probe(scheduler_t scheduler,
                                  invoke_join_receiver receiver)
      : base_t(2U, std::move(scheduler), std::move(receiver)) {}

  auto start_ready_child(const std::uint32_t slot, const int value)
      -> wh::core::result<void> {
    return this->start_child(
        wh::compose::detail::bridge_graph_sender(stdexec::just(
            wh::core::result<wh::compose::graph_value>{
                wh::compose::graph_value{value}})),
        wh::compose::detail::invoke_runtime::attempt_id{slot});
  }

  auto prepare_finish_delivery() noexcept -> void {}

  auto shutdown_for_test() noexcept -> void {
    this->enter_terminal(wh::core::result<wh::compose::graph_value>{
        wh::compose::graph_value{0}});
    this->signal_resume_edge();
  }

  auto resume() noexcept -> void {
    ++resume_calls;
    this->drain_completions(
        [](const wh::compose::detail::invoke_runtime::attempt_id) noexcept {},
        [this](const wh::compose::detail::invoke_runtime::attempt_id attempt,
               wh::core::result<wh::compose::graph_value> &&result)
            -> wh::core::result<void> {
          if (result.has_error()) {
            return wh::core::result<void>::failure(result.error());
          }
          settled_slots.push_back(attempt.slot);
          ++settled_count;
          return {};
        });
  }

  int resume_calls{0};
  int settled_count{0};
  std::vector<std::uint32_t> settled_slots{};
};

} // namespace

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

TEST_CASE("invoke join maps resume scheduling setup failure into one terminal completion",
          "[UT][wh/compose/graph/detail/invoke_join.hpp][invoke_join_base::request_resume][error][terminal]") {
  throwing_scheduler_state scheduler_state{};
  scheduler_state.fail_on_connect = 1U;

  invoke_join_receiver_state receiver_state{};
  invoke_join_probe operation{throwing_scheduler{&scheduler_state},
                              invoke_join_receiver{&receiver_state}};

  operation.start();

  REQUIRE(scheduler_state.connect_calls == 2U);
  REQUIRE(operation.resume_calls == 0);
  REQUIRE(receiver_state.value_count == 1);
  REQUIRE(receiver_state.error_count == 0);
  REQUIRE(receiver_state.stopped_count == 0);
  REQUIRE(receiver_state.result.has_value());
  REQUIRE(receiver_state.result->has_error());
}

TEST_CASE("invoke join coalesces multiple child completions behind one scheduled resume edge",
          "[UT][wh/compose/graph/detail/invoke_join.hpp][invoke_join_base::signal_resume_edge][branch][concurrency]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  wh::testing::helper::manual_scheduler<void> scheduler{&scheduler_state};

  invoke_join_receiver_state receiver_state{};
  invoke_join_edge_probe operation{scheduler, invoke_join_receiver{&receiver_state}};

  REQUIRE(operation.start_ready_child(0U, 3).has_value());
  REQUIRE(operation.start_ready_child(1U, 4).has_value());

  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(operation.resume_calls == 0);
  REQUIRE(operation.settled_count == 0);

  REQUIRE(scheduler_state.run_one());

  REQUIRE(operation.resume_calls == 1);
  REQUIRE(operation.settled_count == 2);
  REQUIRE(operation.settled_slots.size() == 2U);
  REQUIRE(std::find(operation.settled_slots.begin(),
                    operation.settled_slots.end(),
                    0U) != operation.settled_slots.end());
  REQUIRE(std::find(operation.settled_slots.begin(),
                    operation.settled_slots.end(),
                    1U) != operation.settled_slots.end());
  REQUIRE(scheduler_state.pending_count() == 0U);

  operation.shutdown_for_test();
  scheduler_state.run_all();
}

TEST_CASE("invoke join maps join setup failure over a prior terminal success",
          "[UT][wh/compose/graph/detail/invoke_join.hpp][invoke_join_base::finish][error][terminal]") {
  throwing_scheduler_state scheduler_state{};
  scheduler_state.fail_on_connect = 2U;

  invoke_join_receiver_state receiver_state{};
  invoke_join_probe operation{throwing_scheduler{&scheduler_state},
                              invoke_join_receiver{&receiver_state}};

  operation.start();

  REQUIRE(scheduler_state.connect_calls == 2U);
  REQUIRE(operation.resume_calls == 1);
  REQUIRE(receiver_state.value_count == 1);
  REQUIRE(receiver_state.error_count == 0);
  REQUIRE(receiver_state.stopped_count == 0);
  REQUIRE(receiver_state.result.has_value());
  REQUIRE(receiver_state.result->has_error());
}
