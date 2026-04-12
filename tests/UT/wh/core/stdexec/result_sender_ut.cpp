#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <stdexcept>
#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/result_sender.hpp"

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
