#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <tuple>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/ready_result_sender.hpp"

namespace {

struct move_only_box {
  std::unique_ptr<int> value{};

  move_only_box() = default;
  explicit move_only_box(const int input) : value(std::make_unique<int>(input)) {}
  move_only_box(const move_only_box &) = delete;
  auto operator=(const move_only_box &) -> move_only_box & = delete;
  move_only_box(move_only_box &&) noexcept = default;
  auto operator=(move_only_box &&) noexcept -> move_only_box & = default;
};

} // namespace

TEST_CASE("ready_result_sender aliases match the sender factories they describe",
          "[UT][wh/core/stdexec/ready_result_sender.hpp][ready_sender_t][branch]") {
  using ready_sender_alias_t = wh::core::detail::ready_sender_t<int>;
  using ready_sender_expected_t = decltype(stdexec::just(std::declval<int>()));

  using failure_sender_alias_t =
      wh::core::detail::failure_result_sender_t<wh::core::result<int>,
                                                wh::core::error_code>;
  using failure_sender_expected_t = decltype(
      wh::core::detail::ready_sender(
          wh::core::result<int>::failure(std::declval<wh::core::error_code>())));

  REQUIRE((std::is_same_v<ready_sender_alias_t, ready_sender_expected_t>));
  REQUIRE(
      (std::is_same_v<failure_sender_alias_t, failure_sender_expected_t>));
}

TEST_CASE("ready_sender produces immediate copyable and move-only values",
          "[UT][wh/core/stdexec/ready_result_sender.hpp][ready_sender][condition][boundary]") {
  auto text_sender = wh::core::detail::ready_sender(std::string{"alpha"});
  auto text_result = stdexec::sync_wait(std::move(text_sender));
  REQUIRE(text_result.has_value());
  REQUIRE(std::get<0>(*text_result) == "alpha");

  auto move_sender = wh::core::detail::ready_sender(move_only_box{11});
  auto move_result = stdexec::sync_wait(std::move(move_sender));
  REQUIRE(move_result.has_value());
  REQUIRE(std::get<0>(*move_result).value != nullptr);
  REQUIRE(*std::get<0>(*move_result).value == 11);
}

TEST_CASE("failure_result_sender produces immediate failures for value and void results",
          "[UT][wh/core/stdexec/ready_result_sender.hpp][failure_result_sender][branch][boundary]") {
  auto failed = wh::core::detail::failure_result_sender<wh::core::result<int>>(
      wh::core::errc::timeout);
  auto failed_result = stdexec::sync_wait(std::move(failed));
  REQUIRE(failed_result.has_value());
  REQUIRE(std::get<0>(*failed_result).has_error());
  REQUIRE(std::get<0>(*failed_result).error() == wh::core::errc::timeout);

  auto failed_void =
      wh::core::detail::failure_result_sender<wh::core::result<void>>(
          wh::core::errc::channel_closed);
  auto failed_void_result = stdexec::sync_wait(std::move(failed_void));
  REQUIRE(failed_void_result.has_value());
  REQUIRE(std::get<0>(*failed_void_result).has_error());
  REQUIRE(std::get<0>(*failed_void_result).error() ==
          wh::core::errc::channel_closed);
}
