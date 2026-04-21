#include <catch2/catch_test_macros.hpp>

#include "wh/tool/utils/streamable_func.hpp"

TEST_CASE("streamable func accepts stream readers and result readers",
          "[UT][wh/tool/utils/streamable_func.hpp][make_streamable_func][branch][boundary]") {
  auto direct = wh::tool::utils::make_streamable_func(
      [](const std::string_view, const wh::tool::tool_options &) {
        return wh::tool::tool_output_stream_reader{
            wh::schema::stream::make_single_value_stream_reader<std::string>("direct")};
      });
  auto direct_result = direct("", wh::tool::tool_options{});
  REQUIRE(direct_result.has_value());
  auto direct_values = wh::schema::stream::collect_stream_reader(std::move(direct_result).value());
  REQUIRE(direct_values.has_value());
  REQUIRE(direct_values.value() == std::vector<std::string>{"direct"});

  auto wrapped = wh::tool::utils::make_streamable_func(
      [](const std::string_view,
         const wh::tool::tool_options &) -> wh::core::result<wh::tool::tool_output_stream_reader> {
        return wh::tool::tool_output_stream_reader{
            wh::schema::stream::make_single_value_stream_reader<std::string>("wrapped")};
      });
  auto wrapped_result = wrapped("", wh::tool::tool_options{});
  REQUIRE(wrapped_result.has_value());
}

TEST_CASE("streamable func returns contract violation for invalid void outputs",
          "[UT][wh/tool/utils/streamable_func.hpp][make_streamable_func][branch]") {
  auto invalid = wh::tool::utils::make_streamable_func(
      [](const std::string_view, const wh::tool::tool_options &) {});
  auto result = invalid("", wh::tool::tool_options{});
  REQUIRE(result.has_error());
  REQUIRE(result.error() == wh::core::errc::contract_violation);
}

TEST_CASE(
    "streamable func propagates explicit stream errors unchanged",
    "[UT][wh/tool/utils/streamable_func.hpp][make_streamable_func][condition][branch][boundary]") {
  auto failing = wh::tool::utils::make_streamable_func(
      [](const std::string_view,
         const wh::tool::tool_options &) -> wh::core::result<wh::tool::tool_output_stream_reader> {
        return wh::core::result<wh::tool::tool_output_stream_reader>::failure(
            wh::core::errc::timeout);
      });
  auto status = failing("ignored", wh::tool::tool_options{});
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::timeout);
}
