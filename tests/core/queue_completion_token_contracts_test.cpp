#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
#include <exception>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <exec/task.hpp>
#include <exec/timed_thread_scheduler.hpp>
#include <stdexec/execution.hpp>

#include "wh/async/completion_tokens.hpp"
#include "wh/core/mpmc_queue.hpp"
#include "wh/core/type_utils.hpp"
#include "wh/scheduler/context_helper.hpp"
#include "wh/scheduler/timer_helper.hpp"

namespace {

using queue_t = wh::core::mpmc_queue<int>;
using context_t =
    decltype(wh::core::make_scheduler_context(stdexec::inline_scheduler{}));

using void_callback_token_t =
    decltype(wh::core::use_callback(+[](wh::core::result<void>) {}));

static_assert(wh::core::is_sender_v<decltype(std::declval<queue_t &>().push(
                  std::declval<context_t>(), std::declval<int>(),
                  wh::core::use_sender))>);
static_assert(wh::core::is_sender_v<decltype(std::declval<queue_t &>().push(
                  std::declval<context_t>(), std::declval<int>(),
                  wh::core::use_sender))>);

static_assert(wh::core::is_sender_v<decltype(std::declval<queue_t &>().pop(
                  std::declval<context_t>(), wh::core::use_sender))>);

static_assert(wh::core::is_sender_v<decltype(std::declval<queue_t &>().push(
                  std::declval<context_t>(), std::declval<int>(),
                  wh::core::use_awaitable))>);

static_assert(wh::core::is_sender_v<decltype(std::declval<queue_t &>().pop(
                  std::declval<context_t>(), wh::core::use_awaitable))>);

using timed_context_t = decltype(wh::core::make_scheduler_context(
    std::declval<exec::timed_thread_scheduler>()));

static_assert(
    wh::core::is_sender_v<decltype(std::declval<queue_t &>().push_until(
        std::declval<timed_context_t>(),
        std::declval<exec::timed_thread_scheduler::time_point>(),
        std::declval<int>(), wh::core::use_sender))>);

static_assert(
    wh::core::is_sender_v<decltype(std::declval<queue_t &>().pop_until(
        std::declval<timed_context_t>(),
        std::declval<exec::timed_thread_scheduler::time_point>(),
        wh::core::use_awaitable))>);

static_assert(wh::core::is_sender_v<decltype(std::declval<queue_t &>().push(
                  std::declval<context_t>(), std::declval<int>(),
                  wh::core::use_sender))>);

static_assert(wh::core::is_sender_v<decltype(std::declval<queue_t &>().pop(
                  std::declval<context_t>(), wh::core::use_awaitable))>);

static_assert(std::same_as<decltype(std::declval<queue_t &>().push(
                               std::declval<context_t>(), std::declval<int>(),
                               std::declval<void_callback_token_t>())),
                           void>);

static_assert(std::same_as<decltype(std::declval<queue_t &>().push(
                               std::declval<context_t>(), std::declval<int>(),
                               std::declval<void_callback_token_t>())),
                           void>);

static_assert(!exec::timed_scheduler<stdexec::inline_scheduler>);

template <typename value_t, typename sender_t>
[[nodiscard]] auto consume_sender(sender_t &&sender) -> value_t {
  auto sync_result = stdexec::sync_wait(std::forward<sender_t>(sender));
  REQUIRE(sync_result.has_value());
  return std::move(std::get<0>(sync_result.value()));
}

template <typename value_t> class plain_task {
public:
  struct promise_type;
  using handle_t = std::coroutine_handle<promise_type>;

  struct promise_type : stdexec::with_awaitable_senders<promise_type> {
    std::optional<value_t> value;
    std::exception_ptr exception;
    std::binary_semaphore done{0};

    [[nodiscard]] auto get_return_object() noexcept -> plain_task {
      return plain_task{handle_t::from_promise(*this)};
    }

    [[nodiscard]] auto initial_suspend() noexcept -> std::suspend_never {
      return {};
    }

    struct final_awaiter {
      [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

      template <typename promise_t>
      void
      await_suspend(std::coroutine_handle<promise_t> handle) const noexcept {
        handle.promise().done.release();
      }

      void await_resume() const noexcept {}
    };

    [[nodiscard]] auto final_suspend() noexcept -> final_awaiter { return {}; }

    template <typename value_u>
    void return_value(value_u &&returned_value) noexcept(
        std::is_nothrow_constructible_v<value_t, value_u &&>) {
      value.emplace(std::forward<value_u>(returned_value));
    }

    void unhandled_exception() noexcept {
      exception = std::current_exception();
    }
  };

  explicit plain_task(handle_t handle) noexcept : handle_(handle) {}

  plain_task(plain_task &&other) noexcept
      : handle_(std::exchange(other.handle_, {})) {}

  auto operator=(plain_task &&other) noexcept -> plain_task & {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  plain_task(const plain_task &) = delete;
  auto operator=(const plain_task &) -> plain_task & = delete;

  ~plain_task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  [[nodiscard]] auto get() -> value_t {
    auto &promise = handle_.promise();
    promise.done.acquire();
    if (promise.exception) {
      std::rethrow_exception(promise.exception);
    }
    return std::move(promise.value.value());
  }

private:
  handle_t handle_{};
};

[[nodiscard]] auto co_await_push_pop_plain_coroutine(queue_t &queue,
                                                     const context_t &context)
    -> exec::task<wh::core::result<int>> {
  auto push_status = co_await queue.push(context, 17, wh::core::use_awaitable);
  if (push_status.has_error()) {
    co_return wh::core::result<int>::failure(push_status.error());
  }

  co_return co_await queue.pop(context, wh::core::use_awaitable);
}

} // namespace

TEST_CASE("mpmc queue completion token three-mode contracts",
          "[core][mpmc][condition]") {
  queue_t queue(8U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  auto sender_push0 = queue.push(context, 101, wh::core::use_sender);
  auto sender_push0_status =
      consume_sender<wh::core::result<void>>(std::move(sender_push0));
  REQUIRE(sender_push0_status.has_value());

  bool callback_called = false;
  wh::core::result<int> callback_result =
      wh::core::result<int>::failure(wh::core::errc::unavailable);
  queue.pop(context, wh::core::use_callback([&](wh::core::result<int> status) {
              callback_called = true;
              callback_result = std::move(status);
            }));
  REQUIRE(callback_called);
  REQUIRE(callback_result.has_value());
  REQUIRE(callback_result.value() == 101);

  auto sender_push = queue.push(context, 202, wh::core::use_sender);
  auto sender_push_status =
      consume_sender<wh::core::result<void>>(std::move(sender_push));
  REQUIRE(sender_push_status.has_value());

  auto awaitable_pop = queue.pop(context, wh::core::use_awaitable);
  auto awaitable_pop_status =
      consume_sender<wh::core::result<int>>(std::move(awaitable_pop));
  REQUIRE(awaitable_pop_status.has_value());
  REQUIRE(awaitable_pop_status.value() == 202);
}

TEST_CASE("mpmc queue completion token use_callback contract",
          "[core][mpmc][branch]") {
  queue_t queue(8U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  bool push_called = false;
  wh::core::result<void> push_status =
      wh::core::result<void>::failure(wh::core::errc::unavailable);

  queue.push(context, 9,
             wh::core::use_callback([&](wh::core::result<void> status) {
               push_called = true;
               push_status = std::move(status);
             }));

  REQUIRE(push_called);
  REQUIRE(push_status.has_value());

  bool pop_called = false;
  int popped_value = -1;

  queue.pop(context, wh::core::use_callback([&](wh::core::result<int> value) {
              pop_called = true;
              REQUIRE(value.has_value());
              popped_value = value.value();
            }));

  REQUIRE(pop_called);
  REQUIRE(popped_value == 9);
}

TEST_CASE(
    "mpmc queue completion token use_sender scheduler context integration",
    "[core][mpmc][condition]") {
  queue_t queue(8U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  auto push_sender = queue.push(context, 11);
  auto push_status =
      consume_sender<wh::core::result<void>>(std::move(push_sender));
  REQUIRE(push_status.has_value());

  auto pop_sender = queue.pop(context);
  auto pop_status =
      consume_sender<wh::core::result<int>>(std::move(pop_sender));
  REQUIRE(pop_status.has_value());
  REQUIRE(pop_status.value() == 11);
}

TEST_CASE("mpmc queue completion token use_awaitable sender bridge",
          "[core][mpmc][boundary]") {
  queue_t queue(8U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  auto push_sender = queue.push(context, 13, wh::core::use_awaitable);
  auto push_status =
      consume_sender<wh::core::result<void>>(std::move(push_sender));
  REQUIRE(push_status.has_value());

  auto pop_sender = queue.pop(context, wh::core::use_awaitable);
  auto pop_status =
      consume_sender<wh::core::result<int>>(std::move(pop_sender));
  REQUIRE(pop_status.has_value());
  REQUIRE(pop_status.value() == 13);
}

TEST_CASE("mpmc queue push_until/pop_until supports stdexec timed scheduler",
          "[core][mpmc][time][boundary]") {
  queue_t queue(1U);
  exec::timed_thread_context timed_context;
  auto context =
      wh::core::make_scheduler_context(timed_context.get_scheduler());

  REQUIRE(queue.try_push(1).has_value());
  const auto short_deadline =
      wh::core::context_now(context) + std::chrono::milliseconds(1);
  auto write_until_sender =
      queue.push_until(context, short_deadline, 2, wh::core::use_sender);
  auto write_until_status =
      consume_sender<wh::core::result<void>>(std::move(write_until_sender));
  REQUIRE(write_until_status.has_error());
  REQUIRE(write_until_status.error() == wh::core::errc::timeout);

  auto read_until_sender =
      queue.pop_until(context, short_deadline, wh::core::use_sender);
  auto read_until_status =
      consume_sender<wh::core::result<int>>(std::move(read_until_sender));
  REQUIRE(read_until_status.has_value());
  REQUIRE(read_until_status.value() == 1);

  auto write_sender = queue.push(context, 9, wh::core::use_sender);
  auto write_status =
      consume_sender<wh::core::result<void>>(std::move(write_sender));
  REQUIRE(write_status.has_value());

  auto read_sender = queue.pop(context, wh::core::use_sender);
  auto read_status =
      consume_sender<wh::core::result<int>>(std::move(read_sender));
  REQUIRE(read_status.has_value());
  REQUIRE(read_status.value() == 9);
}

TEST_CASE("mpmc queue dual scheduler context keeps one binding",
          "[core][mpmc][time][condition]") {
  queue_t queue(1U);
  exec::timed_thread_context timed_context;
  auto context =
      wh::core::make_scheduler_context(timed_context.get_scheduler());

  static_assert(wh::core::scheduler_context_like<decltype(context)>);

  REQUIRE(queue.try_push(1).has_value());
  const auto short_deadline =
      wh::core::context_now(context) + std::chrono::milliseconds(1);
  auto write_until_sender =
      queue.push_until(context, short_deadline, 2, wh::core::use_sender);
  auto write_until_status =
      consume_sender<wh::core::result<void>>(std::move(write_until_sender));
  REQUIRE(write_until_status.has_error());
  REQUIRE(write_until_status.error() == wh::core::errc::timeout);

  auto read_until_sender =
      queue.pop_until(context, short_deadline, wh::core::use_sender);
  auto read_until_status =
      consume_sender<wh::core::result<int>>(std::move(read_until_sender));
  REQUIRE(read_until_status.has_value());
  REQUIRE(read_until_status.value() == 1);

  auto write_sender = queue.push(context, 7, wh::core::use_sender);
  auto write_status =
      consume_sender<wh::core::result<void>>(std::move(write_sender));
  REQUIRE(write_status.has_value());

  auto read_sender = queue.pop(context, wh::core::use_sender);
  auto read_status =
      consume_sender<wh::core::result<int>>(std::move(read_sender));
  REQUIRE(read_status.has_value());
  REQUIRE(read_status.value() == 7);
}

TEST_CASE("mpmc queue push_until/pop_until three-mode contracts",
          "[core][mpmc][time][condition]") {
  queue_t queue(1U);
  exec::timed_thread_context timed_context;
  auto context =
      wh::core::make_scheduler_context(timed_context.get_scheduler());

  const auto deadline =
      wh::core::context_now(context) + std::chrono::milliseconds(5);

  auto sync_push_sender =
      queue.push_until(context, deadline, 1, wh::core::use_sender);
  auto sync_push =
      consume_sender<wh::core::result<void>>(std::move(sync_push_sender));
  REQUIRE(sync_push.has_value());

  auto callback_done = std::make_shared<std::atomic<bool>>(false);
  auto callback_status = std::make_shared<wh::core::result<int>>(
      wh::core::result<int>::failure(wh::core::errc::unavailable));
  queue.pop_until(context, deadline,
                  wh::core::use_callback([callback_done, callback_status](
                                             wh::core::result<int> status) {
                    *callback_status = std::move(status);
                    callback_done->store(true, std::memory_order_release);
                  }));
  const auto callback_wait_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (!callback_done->load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < callback_wait_deadline) {
    std::this_thread::yield();
  }
  REQUIRE(callback_done->load(std::memory_order_acquire));
  REQUIRE(callback_status->has_value());
  REQUIRE(callback_status->value() == 1);

  REQUIRE(queue.try_push(2).has_value());
  const auto short_deadline =
      wh::core::context_now(context) + std::chrono::milliseconds(1);
  auto timeout_sender =
      queue.push_until(context, short_deadline, 3, wh::core::use_sender);
  auto timeout_status =
      consume_sender<wh::core::result<void>>(std::move(timeout_sender));
  REQUIRE(timeout_status.has_error());
  REQUIRE(timeout_status.error() == wh::core::errc::timeout);

  auto awaitable_pop =
      queue.pop_until(context, deadline, wh::core::use_awaitable);
  auto awaitable_pop_status =
      consume_sender<wh::core::result<int>>(std::move(awaitable_pop));
  REQUIRE(awaitable_pop_status.has_value());
  REQUIRE(awaitable_pop_status.value() == 2);

}

TEST_CASE("mpmc queue use_awaitable supports plain coroutine co_await",
          "[core][mpmc][condition]") {
  queue_t queue(8U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  auto status = consume_sender<wh::core::result<int>>(
      co_await_push_pop_plain_coroutine(queue, context));
  REQUIRE(status.has_value());
  REQUIRE(status.value() == 17);
}
