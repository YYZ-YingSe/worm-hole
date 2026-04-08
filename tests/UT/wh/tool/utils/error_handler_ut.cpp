#include <catch2/catch_test_macros.hpp>

#include "wh/tool/utils/error_handler.hpp"

namespace {

struct failing_policy_impl {
  [[nodiscard]] auto invoke(const wh::tool::tool_request &) const
      -> wh::tool::tool_invoke_result {
    return wh::tool::tool_invoke_result::failure(
        wh::core::errc::invalid_argument);
  }

  [[nodiscard]] auto stream(const wh::tool::tool_request &) const
      -> wh::tool::tool_output_stream_result {
    return wh::tool::tool_output_stream_result::failure(
        wh::core::errc::invalid_argument);
  }
};

} // namespace

TEST_CASE("tool error handler preserves control flow errors and wraps others",
          "[UT][wh/tool/utils/error_handler.hpp][pass_through_or_wrap][branch][boundary]") {
  REQUIRE(wh::tool::utils::pass_through_or_wrap(
              wh::core::make_error(wh::core::errc::canceled)) ==
          wh::core::errc::canceled);
  REQUIRE(wh::tool::utils::pass_through_or_wrap(
              wh::core::make_error(wh::core::errc::invalid_argument)) ==
          wh::core::errc::internal_error);

  REQUIRE(wh::tool::utils::is_interrupt_or_resume_error(
      wh::core::make_error(wh::core::errc::contract_violation)));
  REQUIRE_FALSE(wh::tool::utils::is_interrupt_or_resume_error(
      wh::core::make_error(wh::core::errc::invalid_argument)));
}

TEST_CASE("tool error handler wraps invoke and stream startup failures",
          "[UT][wh/tool/utils/error_handler.hpp][wrap_stream_error][branch]") {
  auto ok = wh::tool::utils::wrap_invoke_error(std::string{"ok"});
  REQUIRE(ok.has_value());
  REQUIRE(ok.value() == "ok");

  auto wrapped = wh::tool::utils::wrap_invoke_error(
      wh::core::result<std::string>::failure(wh::core::errc::invalid_argument));
  REQUIRE(wrapped.has_error());
  REQUIRE(wrapped.error() == wh::core::errc::internal_error);

  auto propagated = wh::tool::utils::wrap_stream_error(
      wh::core::result<wh::tool::tool_output_stream_reader>::failure(
          wh::core::errc::canceled),
      "search");
  REQUIRE(propagated.has_error());
  REQUIRE(propagated.error() == wh::core::errc::canceled);

  auto fallback = wh::tool::utils::wrap_stream_error(
      wh::core::result<wh::tool::tool_output_stream_reader>::failure(
          wh::core::errc::invalid_argument),
      "search");
  REQUIRE(fallback.has_value());
  auto collected =
      wh::schema::stream::collect_stream_reader(std::move(fallback).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value() == std::vector<std::string>{
                                  "[search] invalid_argument"});
}

TEST_CASE("tool error handler can wrap full tool interfaces with the standard policy",
          "[UT][wh/tool/utils/error_handler.hpp][apply_error_policy][condition][branch][boundary]") {
  wh::schema::tool_schema_definition schema{};
  schema.name = "wrapped_tool";
  schema.description = "wrapped";

  wh::tool::tool input_tool{schema, failing_policy_impl{}};
  auto wrapped = wh::tool::utils::apply_error_policy(input_tool);

  wh::core::run_context context{};
  auto invoked = wrapped.invoke(wh::tool::tool_request{"{}", {}}, context);
  REQUIRE(invoked.has_error());
  REQUIRE(invoked.error() == wh::core::errc::internal_error);

  auto streamed = wrapped.stream(wh::tool::tool_request{"{}", {}}, context);
  REQUIRE(streamed.has_value());
  auto collected =
      wh::schema::stream::collect_stream_reader(std::move(streamed).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value() == std::vector<std::string>{
                                  "[wrapped_tool] invalid_argument"});
}
