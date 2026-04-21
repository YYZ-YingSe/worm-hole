#include <future>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/stdexec/counting_scope.hpp"

TEST_CASE("simple counting scope joins after associated work is released",
          "[UT][wh/core/stdexec/counting_scope.hpp][simple_counting_scope][join][concurrency]") {
  wh::core::detail::simple_counting_scope scope{};
  auto token = scope.get_token();
  auto association = token.try_associate();
  REQUIRE(static_cast<bool>(association));

  auto releaser = std::async(std::launch::async, [association = std::move(association)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    association = {};
  });

  scope.close();
  auto joined = stdexec::sync_wait(scope.join());
  REQUIRE(joined.has_value());
  releaser.wait();
}

TEST_CASE("counting scope request_stop does not prevent join completion",
          "[UT][wh/core/stdexec/counting_scope.hpp][counting_scope][request_stop][branch]") {
  wh::core::detail::counting_scope scope{};
  auto token = scope.get_token();
  auto association = token.try_associate();
  REQUIRE(static_cast<bool>(association));

  scope.request_stop();
  association = {};
  scope.close();

  auto joined = stdexec::sync_wait(scope.join());
  REQUIRE(joined.has_value());
}
