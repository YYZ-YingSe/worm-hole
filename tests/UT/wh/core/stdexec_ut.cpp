#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec.hpp"

TEST_CASE("stdexec facade exports ready sender helpers through public header",
          "[UT][wh/core/stdexec.hpp][ready_sender][condition][branch][boundary]") {
  auto sender = wh::core::detail::ready_sender(3);
  auto awaited = stdexec::sync_wait(std::move(sender));

  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited) == 3);
}

TEST_CASE("stdexec facade exports failure result sender helpers through public header",
          "[UT][wh/core/stdexec.hpp][failure_result_sender][condition][branch]") {
  auto sender =
      wh::core::detail::failure_result_sender<wh::core::result<int>>(
          wh::core::errc::invalid_argument);
  auto awaited = stdexec::sync_wait(std::move(sender));

  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_error());
  REQUIRE(std::get<0>(*awaited).error() == wh::core::errc::invalid_argument);
}
