#include <catch2/catch_test_macros.hpp>

#include <string>

#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/core/stdexec/map_result_sender.hpp"

TEST_CASE("map result helpers cover value void nested-result and error branches",
          "[UT][wh/core/stdexec/map_result_sender.hpp][map_result_status][condition][branch][boundary]") {
  using int_result = wh::core::result<int>;
  using string_result = wh::core::result<std::string>;
  using void_result = wh::core::result<void>;

  const auto mapped_value =
      wh::core::detail::map_result_value<int_result>(7);
  REQUIRE(mapped_value.has_value());
  REQUIRE(mapped_value.value() == 7);

  const auto mapped_nested =
      wh::core::detail::map_result_value<int_result>(int_result{8});
  REQUIRE(mapped_nested.has_value());
  REQUIRE(mapped_nested.value() == 8);

  const auto string_status = wh::core::detail::map_result_status<string_result>(
      [](int value) { return std::to_string(value); }, int_result{42});
  REQUIRE(string_status.has_value());
  REQUIRE(string_status.value() == "42");

  const auto nested_status = wh::core::detail::map_result_status<string_result>(
      [](int value) { return string_result{std::to_string(value + 1)}; },
      int_result{9});
  REQUIRE(nested_status.has_value());
  REQUIRE(nested_status.value() == "10");

  int void_mapper_calls = 0;
  const auto void_status = wh::core::detail::map_result_status<void_result>(
      [&void_mapper_calls](int value) {
        ++void_mapper_calls;
        REQUIRE(value == 5);
      },
      int_result{5});
  REQUIRE(void_status.has_value());
  REQUIRE(void_mapper_calls == 1);

  int void_input_calls = 0;
  const auto void_input_status = wh::core::detail::map_result_status<string_result>(
      [&void_input_calls] {
        ++void_input_calls;
        return std::string{"done"};
      },
      void_result{});
  REQUIRE(void_input_status.has_value());
  REQUIRE(void_input_status.value() == "done");
  REQUIRE(void_input_calls == 1);

  const auto propagated_error = wh::core::detail::map_result_status<string_result>(
      [](int) { return std::string{"unused"}; },
      int_result::failure(wh::core::errc::not_found));
  REQUIRE(propagated_error.has_error());
  REQUIRE(propagated_error.error() == wh::core::errc::not_found);
}

TEST_CASE("map result sender covers sender pipeline conversions",
          "[UT][wh/core/stdexec/map_result_sender.hpp][map_result_sender][branch]") {
  auto mapped_string = wh::core::detail::map_result_sender<wh::core::result<std::string>>(
      stdexec::just(wh::core::result<int>{4}),
      [](int value) { return std::to_string(value * 2); });
  const auto string_status =
      wh::testing::helper::wait_value_on_test_thread(std::move(mapped_string));
  REQUIRE(string_status.has_value());
  REQUIRE(string_status.value() == "8");

  auto mapped_void = wh::core::detail::map_result_sender<wh::core::result<void>>(
      stdexec::just(wh::core::result<int>{3}),
      [](int value) { REQUIRE(value == 3); });
  const auto void_status =
      wh::testing::helper::wait_value_on_test_thread(std::move(mapped_void));
  REQUIRE(void_status.has_value());
}
