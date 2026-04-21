#include <exception>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/core/stdexec/defer_sender.hpp"

TEST_CASE("defer sender constructs child lazily and propagates factory exceptions",
          "[UT][wh/core/stdexec/defer_sender.hpp][defer_sender][condition][branch]") {
  bool factory_called = false;
  auto sender = wh::core::detail::defer_sender([&factory_called] {
    factory_called = true;
    return stdexec::just(9);
  });

  REQUIRE_FALSE(factory_called);
  REQUIRE(wh::testing::helper::wait_value_on_test_thread(std::move(sender)) == 9);
  REQUIRE(factory_called);

  auto throwing_sender = wh::core::detail::defer_sender([] {
    throw std::runtime_error{"boom"};
    return stdexec::just();
  });

  wh::testing::helper::sender_capture<void> capture{};
  auto operation = stdexec::connect(std::move(throwing_sender),
                                    wh::testing::helper::sender_capture_receiver<void>{&capture});
  stdexec::start(operation);
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(capture.error != nullptr);
}

TEST_CASE("defer result sender normalizes downstream sender errors into result failures",
          "[UT][wh/core/stdexec/defer_sender.hpp][defer_result_sender][branch]") {
  using result_t = wh::core::result<int>;

  auto sender = wh::core::detail::defer_result_sender<result_t>(
      [] { return stdexec::just_error(std::make_exception_ptr(std::runtime_error{"boom"})); });

  const auto status = wh::testing::helper::wait_value_on_test_thread(std::move(sender));
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::internal_error);
}

TEST_CASE(
    "defer result sender preserves already-materialized result payloads",
    "[UT][wh/core/stdexec/defer_sender.hpp][defer_result_sender][condition][branch][boundary]") {
  using result_t = wh::core::result<int>;

  auto success_sender =
      wh::core::detail::defer_result_sender<result_t>([] { return stdexec::just(result_t{41}); });
  const auto success = wh::testing::helper::wait_value_on_test_thread(std::move(success_sender));
  REQUIRE(success.has_value());
  REQUIRE(success.value() == 41);

  auto failure_sender = wh::core::detail::defer_result_sender<result_t>(
      [] { return stdexec::just(result_t::failure(wh::core::errc::timeout)); });
  const auto failure = wh::testing::helper::wait_value_on_test_thread(std::move(failure_sender));
  REQUIRE(failure.has_error());
  REQUIRE(failure.error() == wh::core::errc::timeout);
}
