#include <catch2/catch_test_macros.hpp>

#include "wh/tool/utils/invokable_func.hpp"

TEST_CASE("invokable func decodes typed inputs and normalizes outputs",
          "[UT][wh/tool/utils/invokable_func.hpp][make_invokable_func][branch][boundary]") {
  auto untyped = wh::tool::utils::make_invokable_func(
      [](const std::string_view input, const wh::tool::tool_options &) {
        return std::string{input};
      });
  auto untyped_result = untyped("hello", wh::tool::tool_options{});
  REQUIRE(untyped_result.has_value());
  REQUIRE(untyped_result.value() == "hello");

  auto typed = wh::tool::utils::make_invokable_func<int>(
      [](const int value, const wh::tool::tool_options &) { return value + 1; });
  auto typed_result = typed("7", wh::tool::tool_options{});
  REQUIRE(typed_result.has_value());
  REQUIRE(typed_result.value() == "8");

  auto void_result = wh::tool::utils::detail::normalize_invokable_output(wh::core::result<void>{});
  REQUIRE(void_result.has_value());
  REQUIRE(void_result.value().empty());
}

TEST_CASE("invokable func supports custom deserializers and propagates decode failures",
          "[UT][wh/tool/utils/invokable_func.hpp][decode_tool_input][branch]") {
  auto decoded = wh::tool::utils::decode_tool_input<int>(
      "ignored", wh::tool::utils::input_deserializer<int>{
                     [](const std::string_view text) -> wh::core::result<int> {
                       return static_cast<int>(text.size());
                     }});
  REQUIRE(decoded.has_value());
  REQUIRE(decoded.value() == 7);

  auto failed = wh::tool::utils::decode_tool_input<int>("not-json");
  REQUIRE(failed.has_error());
}

TEST_CASE("invokable func serializes structured outputs and propagates custom deserializer errors",
          "[UT][wh/tool/utils/"
          "invokable_func.hpp][detail::normalize_invokable_output][condition][branch][boundary]") {
  auto structured = wh::tool::utils::make_invokable_func<int>(
      [](const int value, const wh::tool::tool_options &) {
        return std::vector<int>{value, value + 1};
      });
  auto structured_result = structured("3", wh::tool::tool_options{});
  REQUIRE(structured_result.has_value());
  REQUIRE(structured_result.value() == "[3,4]");

  auto failing = wh::tool::utils::make_invokable_func<int>(
      [](const int value, const wh::tool::tool_options &) { return value; },
      wh::tool::utils::input_deserializer<int>{[](const std::string_view) -> wh::core::result<int> {
        return wh::core::result<int>::failure(wh::core::errc::invalid_argument);
      }});
  auto failed = failing("ignored", wh::tool::tool_options{});
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::invalid_argument);
}
