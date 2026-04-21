#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/test_thread_wait.hpp"
#include "helper/thread_support.hpp"
#include "wh/core/stdexec/concurrent_sender_vector.hpp"
#include "wh/core/stdexec/ready_result_sender.hpp"

namespace {

using result_t = wh::core::result<int>;

enum class terminal_mode : std::uint8_t {
  value = 0U,
  value_error = 1U,
  error = 2U,
  stopped = 3U,
};

struct sender_probe {
  std::atomic<int> start_calls{0};
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  std::atomic<int> destroy_calls{0};
};

inline auto observe_active(sender_probe &probe) noexcept -> void {
  const auto current = probe.active.fetch_add(1, std::memory_order_acq_rel) + 1;
  auto previous = probe.max_active.load(std::memory_order_acquire);
  while (previous < current &&
         !probe.max_active.compare_exchange_weak(previous, current, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
  }
}

struct completion_lifetime_probe {
  bool destroyed{false};
  bool destroyed_before_start_return{false};
};

struct scripted_sender {
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(result_t),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;

  std::shared_ptr<sender_probe> probe{};
  wh::testing::helper::manual_scheduler_state *scheduler_state{nullptr};
  int value{0};
  terminal_mode mode{terminal_mode::value};
  bool async{false};
  bool observe_stop{false};
  bool throw_on_connect{false};

  template <typename receiver_t> struct operation final : wh::testing::helper::manual_task {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;
    std::shared_ptr<sender_probe> probe{};
    wh::testing::helper::manual_scheduler_state *scheduler_state{nullptr};
    int value{0};
    terminal_mode mode{terminal_mode::value};
    bool async{false};
    bool observe_stop{false};
    bool engaged{false};

    operation(receiver_t receiver_value, std::shared_ptr<sender_probe> probe_value,
              wh::testing::helper::manual_scheduler_state *scheduler_state_value,
              const int value_value, const terminal_mode mode_value, const bool async_value,
              const bool observe_stop_value)
        : receiver(std::move(receiver_value)), probe(std::move(probe_value)),
          scheduler_state(scheduler_state_value), value(value_value), mode(mode_value),
          async(async_value), observe_stop(observe_stop_value) {}

    auto start() & noexcept -> void {
      ++probe->start_calls;
      engaged = true;
      observe_active(*probe);
      if (!async) {
        complete();
        return;
      }
      scheduler_state->enqueue(this);
    }

    auto execute() noexcept -> void override { complete(); }

    auto complete() noexcept -> void {
      auto keep_alive = probe;
      if (engaged) {
        probe->active.fetch_sub(1, std::memory_order_acq_rel);
        engaged = false;
      }
      if (observe_stop && stdexec::get_stop_token(stdexec::get_env(receiver)).stop_requested()) {
        stdexec::set_stopped(std::move(receiver));
        return;
      }
      switch (mode) {
      case terminal_mode::value:
        stdexec::set_value(std::move(receiver), result_t{value});
        return;
      case terminal_mode::value_error:
        stdexec::set_value(std::move(receiver), result_t::failure(wh::core::errc::timeout));
        return;
      case terminal_mode::error:
        stdexec::set_error(std::move(receiver),
                           std::make_exception_ptr(std::runtime_error{"child-error"}));
        return;
      case terminal_mode::stopped:
        stdexec::set_stopped(std::move(receiver));
        return;
      }
    }

    ~operation() {
      if (engaged) {
        probe->active.fetch_sub(1, std::memory_order_acq_rel);
      }
      ++probe->destroy_calls;
    }
  };

  template <typename receiver_t> auto connect(receiver_t receiver) && -> operation<receiver_t> {
    if (throw_on_connect) {
      throw std::runtime_error{"connect-failure"};
    }
    return operation<receiver_t>{
        std::move(receiver), std::move(probe), scheduler_state, value, mode, async, observe_stop};
  }
};

struct inline_result_probe_sender {
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(result_t)>;

  std::shared_ptr<completion_lifetime_probe> probe{};
  int value{0};

  template <typename receiver_t> struct operation {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;
    std::shared_ptr<completion_lifetime_probe> probe{};
    int value{0};

    operation(receiver_t receiver_value, std::shared_ptr<completion_lifetime_probe> probe_value,
              const int value_value)
        : receiver(std::move(receiver_value)), probe(std::move(probe_value)), value(value_value) {}

    auto start() & noexcept -> void {
      auto keep_alive = probe;
      stdexec::set_value(std::move(receiver), result_t{value});
      if (keep_alive->destroyed) {
        keep_alive->destroyed_before_start_return = true;
      }
    }

    ~operation() { probe->destroyed = true; }
  };

  template <typename receiver_t> auto connect(receiver_t receiver) && -> operation<receiver_t> {
    return operation<receiver_t>{std::move(receiver), std::move(probe), value};
  }
};

} // namespace

TEST_CASE(
    "concurrent sender vector factory handles empty input and exposes sender surface",
    "[UT][wh/core/stdexec/concurrent_sender_vector.hpp][make_concurrent_sender_vector][boundary]") {
  using sender_t = scripted_sender;
  using vector_sender_t = decltype(wh::core::detail::make_concurrent_sender_vector<result_t>(
      std::vector<sender_t>{}, 0U));

  STATIC_REQUIRE(stdexec::sender<vector_sender_t>);

  auto collected = wh::testing::helper::wait_value_on_test_thread(
      wh::core::detail::make_concurrent_sender_vector<result_t>(std::vector<sender_t>{}, 0U));
  REQUIRE(collected.empty());
}

TEST_CASE(
    "concurrent sender vector enforces in-flight budget and normalizes child terminal branches",
    "[UT][wh/core/stdexec/"
    "concurrent_sender_vector.hpp][concurrent_sender_vector::connect][condition][branch]["
    "concurrency]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  auto probe = std::make_shared<sender_probe>();

  std::vector<scripted_sender> senders{};
  senders.push_back(scripted_sender{
      .probe = probe,
      .scheduler_state = &scheduler_state,
      .value = 1,
      .mode = terminal_mode::value,
      .async = true,
  });
  senders.push_back(scripted_sender{
      .probe = probe,
      .scheduler_state = &scheduler_state,
      .value = 0,
      .mode = terminal_mode::error,
      .async = true,
  });
  senders.push_back(scripted_sender{
      .probe = probe,
      .scheduler_state = &scheduler_state,
      .value = 0,
      .mode = terminal_mode::stopped,
      .async = true,
  });
  senders.push_back(scripted_sender{
      .probe = probe,
      .scheduler_state = &scheduler_state,
      .value = 0,
      .mode = terminal_mode::value_error,
      .async = true,
  });

  wh::testing::helper::sender_capture<std::vector<result_t>> capture{};
  {
    auto operation = stdexec::connect(
        wh::core::detail::make_concurrent_sender_vector<result_t>(std::move(senders), 1U),
        wh::testing::helper::sender_capture_receiver<std::vector<result_t>>{&capture});
    stdexec::start(operation);

    REQUIRE_FALSE(capture.ready.try_acquire());
    REQUIRE(scheduler_state.pending_count() == 1U);
    REQUIRE(probe->max_active.load(std::memory_order_acquire) == 1);

    REQUIRE(scheduler_state.run_one());
    REQUIRE_FALSE(capture.ready.try_acquire());
    REQUIRE(scheduler_state.pending_count() == 1U);
    REQUIRE(probe->max_active.load(std::memory_order_acquire) == 1);

    REQUIRE(scheduler_state.run_one());
    REQUIRE_FALSE(capture.ready.try_acquire());
    REQUIRE(scheduler_state.pending_count() == 1U);

    REQUIRE(scheduler_state.run_one());
    REQUIRE_FALSE(capture.ready.try_acquire());
    REQUIRE(scheduler_state.pending_count() == 1U);

    REQUIRE(scheduler_state.run_one());
    REQUIRE(capture.ready.try_acquire());
    REQUIRE(capture.value.has_value());
    REQUIRE(capture.value->size() == 4U);
    REQUIRE((*capture.value)[0].has_value());
    REQUIRE((*capture.value)[0].value() == 1);
    REQUIRE((*capture.value)[1].has_error());
    REQUIRE((*capture.value)[1].error() == wh::core::errc::internal_error);
    REQUIRE((*capture.value)[2].has_error());
    REQUIRE((*capture.value)[2].error() == wh::core::errc::canceled);
    REQUIRE((*capture.value)[3].has_error());
    REQUIRE((*capture.value)[3].error() == wh::core::errc::timeout);
    REQUIRE(probe->destroy_calls.load(std::memory_order_acquire) == 0);
  }
  REQUIRE(probe->destroy_calls.load(std::memory_order_acquire) == 4);
}

TEST_CASE("concurrent sender vector preserves index order across out-of-order completions and "
          "start failures",
          "[UT][wh/core/stdexec/"
          "concurrent_sender_vector.hpp][concurrent_sender_vector::connect][branch][boundary]") {
  wh::testing::helper::manual_scheduler_state left_scheduler{};
  wh::testing::helper::manual_scheduler_state right_scheduler{};
  auto async_probe = std::make_shared<sender_probe>();

  std::vector<scripted_sender> async_senders{};
  async_senders.push_back(scripted_sender{
      .probe = async_probe,
      .scheduler_state = &left_scheduler,
      .value = 10,
      .mode = terminal_mode::value,
      .async = true,
  });
  async_senders.push_back(scripted_sender{
      .probe = async_probe,
      .scheduler_state = &right_scheduler,
      .value = 20,
      .mode = terminal_mode::value,
      .async = true,
  });
  async_senders.push_back(scripted_sender{
      .probe = async_probe,
      .scheduler_state = &left_scheduler,
      .value = 30,
      .mode = terminal_mode::value,
      .async = true,
  });

  wh::testing::helper::sender_capture<std::vector<result_t>> async_capture{};
  auto async_operation = stdexec::connect(
      wh::core::detail::make_concurrent_sender_vector<result_t>(std::move(async_senders), 0U),
      wh::testing::helper::sender_capture_receiver<std::vector<result_t>>{&async_capture});
  stdexec::start(async_operation);

  REQUIRE(left_scheduler.pending_count() == 2U);
  REQUIRE(right_scheduler.pending_count() == 1U);
  REQUIRE(async_probe->max_active.load(std::memory_order_acquire) == 3);

  REQUIRE(right_scheduler.run_one());
  REQUIRE_FALSE(async_capture.ready.try_acquire());
  REQUIRE(left_scheduler.run_one());
  REQUIRE_FALSE(async_capture.ready.try_acquire());
  REQUIRE(left_scheduler.run_one());
  REQUIRE(async_capture.ready.try_acquire());
  REQUIRE(async_capture.value.has_value());
  REQUIRE(async_capture.value->size() == 3U);
  REQUIRE((*async_capture.value)[0].has_value());
  REQUIRE((*async_capture.value)[0].value() == 10);
  REQUIRE((*async_capture.value)[1].has_value());
  REQUIRE((*async_capture.value)[1].value() == 20);
  REQUIRE((*async_capture.value)[2].has_value());
  REQUIRE((*async_capture.value)[2].value() == 30);

  auto failure_probe = std::make_shared<sender_probe>();
  std::vector<scripted_sender> failure_senders{};
  failure_senders.push_back(scripted_sender{
      .probe = failure_probe,
      .value = 3,
      .mode = terminal_mode::value,
      .async = false,
  });
  failure_senders.push_back(scripted_sender{
      .probe = failure_probe,
      .value = 0,
      .mode = terminal_mode::value,
      .async = false,
      .throw_on_connect = true,
  });
  failure_senders.push_back(scripted_sender{
      .probe = failure_probe,
      .value = 5,
      .mode = terminal_mode::value,
      .async = false,
  });

  auto failure_results = wh::testing::helper::wait_value_on_test_thread(
      wh::core::detail::make_concurrent_sender_vector<result_t>(std::move(failure_senders), 8U));
  REQUIRE(failure_results.size() == 3U);
  REQUIRE(failure_results[0].has_value());
  REQUIRE(failure_results[0].value() == 3);
  REQUIRE(failure_results[1].has_error());
  REQUIRE(failure_results[1].error() == wh::core::errc::internal_error);
  REQUIRE(failure_results[2].has_value());
  REQUIRE(failure_results[2].value() == 5);
}

TEST_CASE("concurrent sender vector keeps child operation alive until inline callback returns",
          "[UT][wh/core/stdexec/"
          "concurrent_sender_vector.hpp][concurrent_sender_vector::connect][lifecycle][branch]") {
  auto probe = std::make_shared<completion_lifetime_probe>();
  std::vector<inline_result_probe_sender> senders{};
  senders.push_back(inline_result_probe_sender{
      .probe = probe,
      .value = 7,
  });

  auto collected = wh::testing::helper::wait_value_on_test_thread(
      wh::core::detail::make_concurrent_sender_vector<result_t>(std::move(senders), 1U));
  REQUIRE(collected.size() == 1U);
  REQUIRE(collected.front().has_value());
  REQUIRE(collected.front().value() == 7);
  REQUIRE(probe->destroyed);
  REQUIRE_FALSE(probe->destroyed_before_start_return);
}

TEST_CASE("concurrent sender vector remains stable across repeated cross-thread completions",
          "[UT][wh/core/stdexec/"
          "concurrent_sender_vector.hpp][concurrent_sender_vector::connect][concurrency][stress]") {
  exec::static_thread_pool pool{4U};
  using sender_t = decltype(stdexec::starts_on(pool.get_scheduler(), stdexec::just(result_t{0})));

  for (int iteration = 0; iteration < 64; ++iteration) {
    std::vector<sender_t> senders{};
    for (int index = 0; index < 8; ++index) {
      senders.push_back(stdexec::starts_on(pool.get_scheduler(),
                                           stdexec::just(result_t{iteration * 100 + index})));
    }

    auto collected = wh::testing::helper::wait_value_on_test_thread(
        wh::core::detail::make_concurrent_sender_vector<result_t>(std::move(senders), 3U));
    REQUIRE(collected.size() == 8U);
    for (int index = 0; index < 8; ++index) {
      REQUIRE(collected[static_cast<std::size_t>(index)].has_value());
      REQUIRE(collected[static_cast<std::size_t>(index)].value() == iteration * 100 + index);
    }
  }
}

TEST_CASE("concurrent sender vector collapses long inline chains under a single in-flight slot",
          "[UT][wh/core/stdexec/"
          "concurrent_sender_vector.hpp][concurrent_sender_vector::connect][inline][stress]") {
  using sender_t = wh::core::detail::ready_sender_t<result_t>;

  std::vector<sender_t> senders{};
  senders.reserve(2048U);
  for (int index = 0; index < 2048; ++index) {
    senders.push_back(wh::core::detail::ready_sender(result_t{index}));
  }

  auto collected = wh::testing::helper::wait_value_on_test_thread(
      wh::core::detail::make_concurrent_sender_vector<result_t>(std::move(senders), 1U));
  REQUIRE(collected.size() == 2048U);
  for (int index = 0; index < 2048; ++index) {
    REQUIRE(collected[static_cast<std::size_t>(index)].has_value());
    REQUIRE(collected[static_cast<std::size_t>(index)].value() == index);
  }
}

TEST_CASE(
    "concurrent sender vector propagates outer stop and prevents launching remaining children",
    "[UT][wh/core/stdexec/"
    "concurrent_sender_vector.hpp][concurrent_sender_vector::connect][stop][concurrency]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  auto probe = std::make_shared<sender_probe>();

  std::vector<scripted_sender> senders{};
  for (int index = 0; index < 3; ++index) {
    senders.push_back(scripted_sender{
        .probe = probe,
        .scheduler_state = &scheduler_state,
        .value = index + 1,
        .mode = terminal_mode::value,
        .async = true,
        .observe_stop = true,
    });
  }

  wh::testing::helper::sender_capture<std::vector<result_t>> capture{};
  wh::testing::helper::stop_source stop_source{};
  auto operation = stdexec::connect(
      wh::core::detail::make_concurrent_sender_vector<result_t>(std::move(senders), 1U),
      wh::testing::helper::sender_capture_receiver{
          &capture,
          wh::testing::helper::make_scheduler_env(stdexec::inline_scheduler{},
                                                  stop_source.get_token()),
      });

  stdexec::start(operation);
  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(probe->start_calls.load(std::memory_order_acquire) == 1);

  stop_source.request_stop();

  REQUIRE(scheduler_state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);
  REQUIRE_FALSE(capture.value.has_value());
  REQUIRE(probe->start_calls.load(std::memory_order_acquire) == 1);
  REQUIRE(scheduler_state.pending_count() == 0U);
}
