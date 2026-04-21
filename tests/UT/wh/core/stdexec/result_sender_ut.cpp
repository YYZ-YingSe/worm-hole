#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <stdexcept>
#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/core/stdexec/result_sender.hpp"

namespace {

struct throwing_stop_token {
  template <typename callback_t> struct callback_type {
    explicit callback_type(throwing_stop_token, callback_t) {
      throw std::runtime_error{"throwing stop callback"};
    }
  };

  bool stop_requested() const noexcept { return false; }
  bool stop_possible() const noexcept { return true; }

  auto operator==(const throwing_stop_token &) const noexcept -> bool = default;
};

struct throwing_stop_env {
  throwing_stop_token stop_token{};

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> throwing_stop_token {
    return stop_token;
  }
};

} // namespace

TEST_CASE("normalize_result_sender preserves result values and result failures",
          "[UT][wh/core/stdexec/result_sender.hpp][normalize_result_sender][condition][branch]") {
  using result_t = wh::core::result<int>;

  auto ready_sender = wh::core::detail::normalize_result_sender<result_t>(
      stdexec::just(result_t{7}));
  auto ready_awaited = stdexec::sync_wait(std::move(ready_sender));

  REQUIRE(ready_awaited.has_value());
  REQUIRE(std::get<0>(*ready_awaited).has_value());
  REQUIRE(std::get<0>(*ready_awaited).value() == 7);

  auto failed_sender = wh::core::detail::normalize_result_sender<result_t>(
      stdexec::just(result_t::failure(wh::core::errc::timeout)));
  auto failed_awaited = stdexec::sync_wait(std::move(failed_sender));

  REQUIRE(failed_awaited.has_value());
  REQUIRE(std::get<0>(*failed_awaited).has_error());
  REQUIRE(std::get<0>(*failed_awaited).error() == wh::core::errc::timeout);
}

TEST_CASE("normalize_result_sender turns exception sender errors into internal result failures",
          "[UT][wh/core/stdexec/result_sender.hpp][normalize_result_sender][branch][error]") {
  using result_t = wh::core::result<int>;

  auto sender = wh::core::detail::normalize_result_sender<result_t>(
      stdexec::just_error(std::make_exception_ptr(std::runtime_error{"boom"})));
  auto awaited = stdexec::sync_wait(std::move(sender));

  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_error());
  REQUIRE(std::get<0>(*awaited).error() == wh::core::errc::internal_error);
}

TEST_CASE("normalize_result_sender maps arbitrary sender errors for void results",
          "[UT][wh/core/stdexec/result_sender.hpp][normalize_result_sender][branch][boundary]") {
  using result_t = wh::core::result<void>;

  auto sender = wh::core::detail::normalize_result_sender<result_t>(
      stdexec::just_error(wh::core::errc::queue_full));
  auto awaited = stdexec::sync_wait(std::move(sender));

  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_error());
  REQUIRE(std::get<0>(*awaited).error() == wh::core::errc::internal_error);
}

TEST_CASE("result_sender reports outer stop binding failures as internal result failures",
          "[UT][wh/core/stdexec/result_sender.hpp][result_sender][error][boundary]") {
  using result_t = wh::core::result<int>;

  wh::testing::helper::sender_capture<result_t> capture{};
  auto operation = stdexec::connect(
      wh::core::detail::result_sender<result_t>{stdexec::just(result_t{7})},
      wh::testing::helper::sender_capture_receiver<result_t, throwing_stop_env>{
          &capture, {}});
  stdexec::start(operation);

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_error());
  REQUIRE(capture.value->error() == wh::core::errc::internal_error);
}
