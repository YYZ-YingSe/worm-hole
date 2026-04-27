#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/agent/middlewares/reduction/clear_tool_result.hpp"

namespace {

[[nodiscard]] auto make_text_message(const wh::schema::message_role role, std::string text,
                                     std::string tool_name = {}) -> wh::schema::message {
  wh::schema::message message{};
  message.role = role;
  message.tool_name = std::move(tool_name);
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

} // namespace

TEST_CASE("clear tool result detail helpers estimate tokens and classify reducible tool messages",
          "[UT][wh/agent/middlewares/reduction/"
          "clear_tool_result.hpp][count_part_tokens][condition][branch][boundary]") {
  wh::schema::message_part text_part{wh::schema::text_part{"abcd"}};
  REQUIRE(wh::agent::middlewares::reduction::detail::count_part_tokens(text_part) == 1U);

  wh::schema::message_part tool_part{wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "search",
      .arguments = "abcdefgh",
      .complete = true,
  }};
  REQUIRE(wh::agent::middlewares::reduction::detail::count_part_tokens(tool_part) == 2U);

  wh::schema::message tool_message =
      make_text_message(wh::schema::message_role::tool, "payload", "search");
  wh::schema::message user_message = make_text_message(wh::schema::message_role::user, "payload");
  wh::agent::middlewares::reduction::clear_tool_result_options options{};
  options.excluded_tool_names.insert("keep");
  REQUIRE(wh::agent::middlewares::reduction::detail::default_estimate_tokens(tool_message) == 2U);
  REQUIRE(wh::agent::middlewares::reduction::detail::should_reduce_message(tool_message, options));
  REQUIRE_FALSE(wh::agent::middlewares::reduction::detail::should_reduce_message(
      make_text_message(wh::schema::message_role::tool, "payload", "keep"), options));
  REQUIRE_FALSE(
      wh::agent::middlewares::reduction::detail::should_reduce_message(user_message, options));
}

TEST_CASE(
    "clear tool result transform trims only old tool messages outside protected suffix and honors "
    "custom estimator",
    "[UT][wh/agent/middlewares/reduction/"
    "clear_tool_result.hpp][make_clear_tool_result_transform][condition][branch][boundary]") {
  wh::model::chat_request request{};
  request.messages.push_back(make_text_message(wh::schema::message_role::system, "system"));

  wh::schema::message assistant_call{};
  assistant_call.role = wh::schema::message_role::assistant;
  assistant_call.parts.emplace_back(wh::schema::tool_call_part{
      .id = "call-1",
      .name = "search",
      .arguments = std::string(120U, 'a'),
  });
  request.messages.push_back(std::move(assistant_call));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "old tool payload", "search"));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "excluded payload", "keep"));
  request.messages.push_back(make_text_message(wh::schema::message_role::user, "recent user text"));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "recent tool payload", "search"));

  auto transform = wh::agent::middlewares::reduction::make_clear_tool_result_transform(
      wh::agent::middlewares::reduction::clear_tool_result_options{
          .max_history_tokens = 20U,
          .protected_recent_tokens = 8U,
          .placeholder = "[[trimmed]]",
          .excluded_tool_names = {"keep"},
      });

  wh::core::run_context context{};
  auto reduced = transform.sync(std::move(request), context);
  REQUIRE(reduced.has_value());
  REQUIRE(std::get<wh::schema::text_part>(reduced->messages[2].parts.front()).text ==
          "[[trimmed]]");
  REQUIRE(std::get<wh::schema::text_part>(reduced->messages[3].parts.front()).text ==
          "excluded payload");
  REQUIRE(std::get<wh::schema::text_part>(reduced->messages.back().parts.front()).text ==
          "recent tool payload");

  wh::model::chat_request unchanged{};
  unchanged.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "short", "search"));
  auto no_trim = wh::agent::middlewares::reduction::make_clear_tool_result_transform(
      wh::agent::middlewares::reduction::clear_tool_result_options{
          .max_history_tokens = 100U,
      });
  auto unchanged_result = no_trim.sync(std::move(unchanged), context);
  REQUIRE(unchanged_result.has_value());
  REQUIRE(std::get<wh::schema::text_part>(unchanged_result->messages.front().parts.front()).text ==
          "short");

  wh::model::chat_request fallback_placeholder{};
  fallback_placeholder.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "payload", "search"));
  auto custom = wh::agent::middlewares::reduction::make_clear_tool_result_transform(
      wh::agent::middlewares::reduction::clear_tool_result_options{
          .max_history_tokens = 1U,
          .protected_recent_tokens = 0U,
          .placeholder = "",
          .estimate_tokens =
              wh::agent::middlewares::reduction::token_estimator{
                  [](const wh::schema::message &) -> std::size_t { return 10U; }},
      });
  auto custom_result = custom.sync(std::move(fallback_placeholder), context);
  REQUIRE(custom_result.has_value());
  REQUIRE(std::get<wh::schema::text_part>(custom_result->messages.front().parts.front()).text ==
          "[tool result omitted]");

  auto surface = wh::agent::middlewares::reduction::make_clear_tool_result_surface(
      wh::agent::middlewares::reduction::clear_tool_result_options{});
  REQUIRE(surface.tool_bindings.empty());
  REQUIRE(surface.instruction_fragments.empty());
  REQUIRE(surface.request_transforms.size() == 1U);
}
