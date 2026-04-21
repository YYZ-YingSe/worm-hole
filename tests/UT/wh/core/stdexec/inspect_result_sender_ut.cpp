#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/core/stdexec/inspect_result_sender.hpp"

TEST_CASE(
    "inspect result sender observes successful status and forwards it unchanged",
    "[UT][wh/core/stdexec/inspect_result_sender.hpp][inspect_result_sender][condition][branch]") {
  int seen_value = 0;
  bool inspector_called = false;

  auto sender =
      wh::core::detail::inspect_result_sender(stdexec::just(wh::core::result<int>{5}),
                                              [&inspector_called, &seen_value](const auto &status) {
                                                inspector_called = true;
                                                REQUIRE(status.has_value());
                                                seen_value = status.value();
                                              });

  const auto status = wh::testing::helper::wait_value_on_test_thread(std::move(sender));
  REQUIRE(inspector_called);
  REQUIRE(seen_value == 5);
  REQUIRE(status.has_value());
  REQUIRE(status.value() == 5);
}

TEST_CASE("inspect result sender observes error status and forwards it unchanged",
          "[UT][wh/core/stdexec/inspect_result_sender.hpp][inspect_result_sender][branch]") {
  bool saw_error = false;

  auto sender = wh::core::detail::inspect_result_sender(
      stdexec::just(wh::core::result<std::string>::failure(wh::core::errc::not_found)),
      [&saw_error](const auto &status) {
        saw_error = true;
        REQUIRE(status.has_error());
        REQUIRE(status.error() == wh::core::errc::not_found);
      });

  const auto status = wh::testing::helper::wait_value_on_test_thread(std::move(sender));
  REQUIRE(saw_error);
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::not_found);
}

TEST_CASE("inspect result sender also forwards result<void> without altering readiness",
          "[UT][wh/core/stdexec/"
          "inspect_result_sender.hpp][inspect_result_sender][condition][branch][boundary]") {
  bool inspector_called = false;

  auto sender = wh::core::detail::inspect_result_sender(stdexec::just(wh::core::result<void>{}),
                                                        [&inspector_called](const auto &status) {
                                                          inspector_called = true;
                                                          REQUIRE(status.has_value());
                                                        });

  const auto status = wh::testing::helper::wait_value_on_test_thread(std::move(sender));
  REQUIRE(inspector_called);
  REQUIRE(status.has_value());
}
