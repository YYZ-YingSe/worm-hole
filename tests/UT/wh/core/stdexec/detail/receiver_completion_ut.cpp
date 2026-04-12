#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <memory>
#include <stdexcept>

#include "helper/sender_capture.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"

TEST_CASE("receiver completion delivers move-only values exactly once",
          "[UT][wh/core/stdexec/detail/receiver_completion.hpp][receiver_completion::set_value][boundary][branch]") {
  using value_t = std::unique_ptr<int>;
  using receiver_t = wh::testing::helper::sender_capture_receiver<value_t>;

  wh::testing::helper::sender_capture<value_t> capture{};
  auto completion = wh::core::detail::receiver_completion<receiver_t, value_t>::set_value(
      receiver_t{&capture}, std::make_unique<int>(7));

  std::move(completion).complete();

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(*capture.value->get() == 7);
}

TEST_CASE("receiver completion delivers error and stopped terminals",
          "[UT][wh/core/stdexec/detail/receiver_completion.hpp][receiver_completion::complete][branch]") {
  using receiver_t = wh::testing::helper::sender_capture_receiver<int>;
  using completion_t = wh::core::detail::receiver_completion<receiver_t, int>;

  wh::testing::helper::sender_capture<int> error_capture{};
  auto error_completion = completion_t::set_error(
      receiver_t{&error_capture},
      std::make_exception_ptr(std::runtime_error{"boom"}));
  std::move(error_completion).complete();

  REQUIRE(error_capture.ready.try_acquire());
  REQUIRE(error_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(error_capture.error != nullptr);

  wh::testing::helper::sender_capture<int> stopped_capture{};
  auto stopped_completion = completion_t::set_stopped(receiver_t{&stopped_capture});
  std::move(stopped_completion).complete();

  REQUIRE(stopped_capture.ready.try_acquire());
  REQUIRE(stopped_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::stopped);
}

TEST_CASE("receiver completion preserves plain value payloads after moves",
          "[UT][wh/core/stdexec/detail/receiver_completion.hpp][receiver_completion][condition][branch][boundary]") {
  using receiver_t = wh::testing::helper::sender_capture_receiver<int>;
  using completion_t = wh::core::detail::receiver_completion<receiver_t, int>;

  wh::testing::helper::sender_capture<int> capture{};
  auto completion = completion_t::set_value(receiver_t{&capture}, 13);
  auto moved = std::move(completion);
  std::move(moved).complete();

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(*capture.value == 13);
}
