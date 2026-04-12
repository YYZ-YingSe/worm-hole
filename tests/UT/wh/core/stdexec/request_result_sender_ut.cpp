#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/core/stdexec/request_result_sender.hpp"

namespace {

struct request_trace {
  int copies{0};
  int moves{0};
};

struct tracked_request {
  int value{0};
  request_trace *trace{nullptr};

  tracked_request() = default;
  tracked_request(int input, request_trace *trace_ptr)
      : value(input), trace(trace_ptr) {}

  tracked_request(const tracked_request &other)
      : value(other.value), trace(other.trace) {
    if (trace != nullptr) {
      ++trace->copies;
    }
  }

  tracked_request(tracked_request &&other) noexcept
      : value(other.value), trace(other.trace) {
    if (trace != nullptr) {
      ++trace->moves;
    }
    other.trace = nullptr;
  }

  auto operator=(const tracked_request &) -> tracked_request & = default;
  auto operator=(tracked_request &&) noexcept -> tracked_request & = default;
};

struct move_only_request {
  int value{0};

  move_only_request() = default;
  explicit move_only_request(int input) : value(input) {}
  move_only_request(const move_only_request &) = delete;
  auto operator=(const move_only_request &) -> move_only_request & = delete;
  move_only_request(move_only_request &&) noexcept = default;
  auto operator=(move_only_request &&) noexcept -> move_only_request & = default;
};

struct lvalue_value_only_call {
  auto operator()(tracked_request &) const
      -> decltype(stdexec::just(wh::core::result<int>{0})) = delete;

  [[nodiscard]] auto operator()(tracked_request request) const {
    return stdexec::just(wh::core::result<int>{request.value});
  }
};

struct rvalue_blocking_call {
  auto operator()(move_only_request &&) const
      -> decltype(stdexec::just(wh::core::result<int>{0})) = delete;

  [[nodiscard]] auto operator()(const move_only_request &request) const {
    return stdexec::just(wh::core::result<int>{request.value});
  }
};

} // namespace

TEST_CASE("request result sender preserves lvalue requests and uses copy fallback when needed",
          "[UT][wh/core/stdexec/request_result_sender.hpp][request_result_sender][condition][branch]") {
  request_trace direct_trace{};
  tracked_request direct_request{7, &direct_trace};
  const tracked_request *seen_address = nullptr;

  auto direct_sender = wh::core::detail::request_result_sender<wh::core::result<int>>(
      direct_request, [&seen_address](const tracked_request &request) {
        seen_address = &request;
        return stdexec::just(wh::core::result<int>{request.value});
      });
  const auto direct_status =
      wh::testing::helper::wait_value_on_test_thread(std::move(direct_sender));
  REQUIRE(direct_status.has_value());
  REQUIRE(direct_status.value() == 7);
  REQUIRE(seen_address == &direct_request);
  REQUIRE(direct_trace.copies == 0);
  REQUIRE(direct_trace.moves == 0);

  request_trace copy_trace{};
  tracked_request copied_request{8, &copy_trace};
  auto copied_sender = wh::core::detail::request_result_sender<wh::core::result<int>>(
      copied_request, lvalue_value_only_call{});
  const auto copied_status =
      wh::testing::helper::wait_value_on_test_thread(std::move(copied_sender));
  REQUIRE(copied_status.has_value());
  REQUIRE(copied_status.value() == 8);
  REQUIRE(copy_trace.copies >= 1);
}

TEST_CASE("request result sender covers rvalue direct path fallback path and error normalization",
          "[UT][wh/core/stdexec/request_result_sender.hpp][request_result_sender][branch][boundary]") {
  request_trace move_trace{};
  tracked_request moved_request{9, &move_trace};
  bool rvalue_overload_called = false;
  auto moved_sender = wh::core::detail::request_result_sender<wh::core::result<int>>(
      std::move(moved_request), [&rvalue_overload_called](tracked_request &&request) {
        rvalue_overload_called = true;
        return stdexec::just(wh::core::result<int>{request.value});
      });
  const auto moved_status =
      wh::testing::helper::wait_value_on_test_thread(std::move(moved_sender));
  REQUIRE(moved_status.has_value());
  REQUIRE(moved_status.value() == 9);
  REQUIRE(rvalue_overload_called);

  auto fallback_sender = wh::core::detail::request_result_sender<wh::core::result<int>>(
      move_only_request{11}, rvalue_blocking_call{});
  const auto fallback_status =
      wh::testing::helper::wait_value_on_test_thread(std::move(fallback_sender));
  REQUIRE(fallback_status.has_value());
  REQUIRE(fallback_status.value() == 11);

  auto error_sender = wh::core::detail::request_result_sender<wh::core::result<int>>(
      tracked_request{12, nullptr}, [](const tracked_request &) {
        return stdexec::just_error(
            std::make_exception_ptr(std::runtime_error{"boom"}));
      });
  const auto error_status =
      wh::testing::helper::wait_value_on_test_thread(std::move(error_sender));
  REQUIRE(error_status.has_error());
  REQUIRE(error_status.error() == wh::core::errc::internal_error);
}

TEST_CASE("defer request result sender is lazy and forwards stored request",
          "[UT][wh/core/stdexec/request_result_sender.hpp][defer_request_result_sender][branch]") {
  bool invoked = false;
  auto sender = wh::core::detail::defer_request_result_sender<wh::core::result<int>>(
      tracked_request{21, nullptr}, [&invoked](tracked_request request) {
        invoked = true;
        return stdexec::just(wh::core::result<int>{request.value});
      });

  REQUIRE_FALSE(invoked);
  const auto status =
      wh::testing::helper::wait_value_on_test_thread(std::move(sender));
  REQUIRE(status.has_value());
  REQUIRE(status.value() == 21);
  REQUIRE(invoked);
}
